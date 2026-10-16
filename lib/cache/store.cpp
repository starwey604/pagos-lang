#include "pagos/cache/store.h"
#include "wire.h"

#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_set>

namespace pagos::cache {
namespace {
namespace fs = std::filesystem;
namespace llvm_fs = llvm::sys::fs;
constexpr std::size_t object_header_size = 4 + 4 + 1 + 8 + 32;
constexpr std::size_t checksum_size = 32;

StoreError error(StoreErrorCode code, std::error_code io = {},
                 std::optional<Error> codec = {}) {
    return {code, io, codec};
}
StoreError codec_error(Error value, StoreErrorCode corrupt) {
    return error(value == Error::LimitExceeded ? StoreErrorCode::LimitExceeded
                 : value == Error::IncompatibleVersion
                     ? StoreErrorCode::IncompatibleVersion
                     : corrupt,
                 {}, value);
}
std::expected<void, StoreError> checkpoint(const StoreOptions& options,
                                           IoOperation operation,
                                           const fs::path& path) {
    if (options.fault)
        if (const auto ec = options.fault(operation, path))
            return std::unexpected(error(StoreErrorCode::IoFailure, ec));
    return {};
}
bool valid_options(const StoreOptions& options, const fs::path& root) {
    return !root.empty() &&
           root.native().find('\0') == fs::path::string_type::npos &&
           options.lock_timeout.count() >= 0 &&
           options.object_bytes <=
               std::numeric_limits<std::size_t>::max() - object_header_size;
}
bool valid_name(std::string_view name) {
    return !name.empty() && name.size() <= 64 &&
           std::ranges::all_of(name, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-';
           });
}
bool valid_kind(ObjectKind kind) {
    return kind == ObjectKind::Value || kind == ObjectKind::QueryKey ||
           kind == ObjectKind::SourceAnchor ||
           kind == ObjectKind::ResourceRecipe ||
           kind == ObjectKind::ProvenanceSnapshot ||
           kind == ObjectKind::ProvenancePositions ||
           kind == ObjectKind::ResidualTemplate ||
           kind == ObjectKind::EvidenceBlock ||
           kind == ObjectKind::EvidenceRoot ||
           kind == ObjectKind::EvidenceSummary ||
           kind == ObjectKind::EvidenceDetail ||
           kind == ObjectKind::QueryCheckpoint;
}
fs::path object_path(const fs::path& root, const Digest& digest) {
    const auto hex = digest.hex();
    return root / "objects" / hex.substr(0, 2) / hex;
}
fs::path query_directory(const fs::path& root, const Digest& digest) {
    const auto hex = digest.hex();
    return root / "queries" / hex.substr(0, 2);
}
fs::path manifest_path(const fs::path& root, const Digest& key,
                       const Digest& inputs) {
    return query_directory(root, key) / key.hex() /
           (inputs.hex() + ".manifest");
}
std::expected<void, StoreError> directories(const StoreOptions& options,
                                            const fs::path& path) {
    if (const auto result =
            checkpoint(options, IoOperation::CreateDirectories, path);
        !result)
        return result;
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec)
        return std::unexpected(error(StoreErrorCode::IoFailure, ec));
    return {};
}

// LLVM file locks can be process-scoped on some systems. This mutex also
// serializes threads in this process; lock files are never removed/recreated.
std::timed_mutex process_mutex;
// A failed conflict-marker write must still stop reuse in this process.
// Access is serialized by QueryLock's process mutex.
std::unordered_set<std::string> disabled_inputs;
std::string disabled_id(const fs::path& root, const Digest& key,
                        const Digest& inputs) {
    return fs::absolute(root).lexically_normal().generic_string() + '\0' +
           key.hex() + inputs.hex();
}
class QueryLock {
  public:
    explicit QueryLock(const StoreOptions& options)
        : thread_lock_(process_mutex, std::defer_lock), options_(options) {}
    ~QueryLock() {
        if (fd_ != -1) {
            if (locked_)
                (void)llvm_fs::unlockFile(fd_);
            (void)llvm::sys::Process::SafelyCloseFileDescriptor(fd_);
        }
    }
    std::expected<void, StoreError> acquire(const fs::path& path,
                                            bool publish) {
        if (!thread_lock_.try_lock_for(options_.lock_timeout))
            return std::unexpected(error(StoreErrorCode::LockBusy));
        if (const auto result =
                checkpoint(options_, IoOperation::LockFile, path);
            !result)
            return result;
        std::error_code status_error;
        const auto status = fs::symlink_status(path, status_error);
        if (status_error &&
            status_error != std::errc::no_such_file_or_directory)
            return std::unexpected(
                error(StoreErrorCode::IoFailure, status_error));
        if (fs::exists(status) && !fs::is_regular_file(status))
            return std::unexpected(
                error(StoreErrorCode::IoFailure,
                      std::make_error_code(std::errc::invalid_argument)));
        const auto ec = publish
                            ? llvm_fs::openFileForWrite(path.string(), fd_,
                                                        llvm_fs::CD_OpenAlways,
                                                        llvm_fs::OF_None, 0600)
                            : llvm_fs::openFileForRead(path.string(), fd_);
        if (ec)
            return std::unexpected(
                error(ec == std::errc::no_such_file_or_directory
                          ? StoreErrorCode::NotFound
                          : StoreErrorCode::IoFailure,
                      ec));
        const auto lock_error = llvm_fs::tryLockFile(
            fd_, options_.lock_timeout,
            publish ? llvm_fs::LockKind::Exclusive : llvm_fs::LockKind::Shared);
        if (lock_error)
            return std::unexpected(
                error(lock_error == std::errc::no_lock_available
                          ? StoreErrorCode::LockBusy
                          : StoreErrorCode::IoFailure,
                      lock_error));
        locked_ = true;
        return {};
    }

  private:
    std::unique_lock<std::timed_mutex> thread_lock_;
    const StoreOptions& options_;
    int fd_{-1};
    bool locked_{};
};

std::expected<Bytes, StoreError> read_file(const StoreOptions& options,
                                           const fs::path& path,
                                           std::size_t maximum,
                                           StoreErrorCode corrupt) {
    if (const auto result = checkpoint(options, IoOperation::ReadFile, path);
        !result)
        return std::unexpected(result.error());
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec || !fs::exists(status))
        return std::unexpected(
            error(!ec || ec == std::errc::no_such_file_or_directory
                      ? StoreErrorCode::NotFound
                      : StoreErrorCode::IoFailure,
                  ec));
    if (!fs::is_regular_file(status))
        return std::unexpected(error(corrupt));
    int fd = -1;
    if (const auto open_error = llvm_fs::openFileForRead(path.string(), fd))
        return std::unexpected(
            error(open_error == std::errc::no_such_file_or_directory
                      ? StoreErrorCode::NotFound
                      : StoreErrorCode::IoFailure,
                  open_error));
    struct Close {
        int fd;
        ~Close() { (void)llvm::sys::Process::SafelyCloseFileDescriptor(fd); }
    } close{fd};
    llvm_fs::file_status opened;
    if (const auto status_error = llvm_fs::status(fd, opened))
        return std::unexpected(error(StoreErrorCode::IoFailure, status_error));
    if (!llvm_fs::is_regular_file(opened))
        return std::unexpected(error(corrupt));
    if (opened.getSize() > maximum || opened.getSize() > Bytes{}.max_size())
        return std::unexpected(error(StoreErrorCode::LimitExceeded));
    Bytes bytes(static_cast<std::size_t>(opened.getSize()));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        auto count = llvm_fs::readNativeFile(
            llvm_fs::convertFDToNativeFile(fd),
            {reinterpret_cast<char*>(bytes.data() + offset),
             bytes.size() - offset});
        if (!count)
            return std::unexpected(
                error(StoreErrorCode::IoFailure,
                      llvm::errorToErrorCode(count.takeError())));
        if (*count == 0)
            return std::unexpected(error(corrupt));
        offset += *count;
    }
    char extra{};
    auto count = llvm_fs::readNativeFile(llvm_fs::convertFDToNativeFile(fd),
                                         {&extra, 1});
    if (!count)
        return std::unexpected(
            error(StoreErrorCode::IoFailure,
                  llvm::errorToErrorCode(count.takeError())));
    if (*count != 0)
        return std::unexpected(error(corrupt));
    return bytes;
}

void magic(wire::Writer& writer, std::string_view text) {
    writer.raw(
        {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    writer.u32(store_format_version);
}
void magic(wire::Reader& reader, std::string_view text) {
    const auto bytes = reader.raw(text.size());
    if (!std::ranges::equal(bytes, text))
        reader.fail(Error::InvalidFormat);
    if (reader.u32() != store_format_version)
        reader.fail(Error::IncompatibleVersion);
}
Digest digest(wire::Reader& reader) {
    Digest value;
    const auto bytes = reader.raw(32);
    if (reader.good())
        std::ranges::copy(bytes, value.bytes.begin());
    return value;
}
std::expected<void, StoreError>
payload_header(ObjectKind kind, std::span<const std::uint8_t> bytes,
               const StoreOptions& options) {
    if (!valid_kind(kind))
        return std::unexpected(error(StoreErrorCode::InvalidRequest));
    if (bytes.size() > options.object_bytes)
        return std::unexpected(error(StoreErrorCode::LimitExceeded));
    wire::Reader reader(bytes, {.bytes = options.object_bytes});
    wire::header(reader, kind);
    // Payload semantics (including SourceAnchor resolution) belong to adapters.
    if (!reader.good()) {
        const auto result = reader.finish();
        return std::unexpected(
            codec_error(result.error(), StoreErrorCode::CorruptObject));
    }
    return {};
}
Digest object_digest(ObjectKind kind, std::span<const std::uint8_t> bytes) {
    wire::Writer writer({.bytes = bytes.size() + 13});
    writer.u32(store_format_version);
    writer.u8(static_cast<std::uint8_t>(kind));
    writer.u64(bytes.size());
    writer.raw(bytes);
    return fingerprint("pagos.cas-object.v1", *writer.finish());
}
std::expected<Bytes, StoreError> read_cas_object(const fs::path& root,
                                                 const StoreOptions& options,
                                                 const ObjectRef& reference) {
    if (reference.size > options.object_bytes)
        return std::unexpected(error(StoreErrorCode::LimitExceeded));
    const auto bytes = read_file(options, object_path(root, reference.digest),
                                 options.object_bytes + object_header_size,
                                 StoreErrorCode::CorruptObject);
    if (!bytes) {
        auto failure = bytes.error();
        if (failure.code == StoreErrorCode::NotFound)
            failure.code = StoreErrorCode::MissingObject;
        return std::unexpected(failure);
    }
    wire::Reader reader(*bytes,
                        {.bytes = options.object_bytes + object_header_size});
    magic(reader, "PGO1");
    const auto kind = static_cast<ObjectKind>(reader.u8());
    const auto size = reader.u64();
    const auto hash = digest(reader);
    if (!valid_kind(kind) || kind != reference.kind || size != reference.size ||
        size != reader.remaining() || hash != reference.digest)
        reader.fail(Error::InvalidFormat);
    const auto payload = reader.raw(reader.remaining());
    if (const auto result = reader.finish(); !result)
        return std::unexpected(
            codec_error(result.error(), StoreErrorCode::CorruptObject));
    if (object_digest(kind, payload) != hash)
        return std::unexpected(error(StoreErrorCode::CorruptObject));
    if (const auto result = payload_header(kind, payload, options); !result)
        return std::unexpected(result.error());
    return Bytes(payload.begin(), payload.end());
}

struct Manifest {
    bool conflict{};
    Bytes key;
    Digest inputs;
    Digest result;
    std::vector<StoredArtifact> artifacts;
};
std::expected<Bytes, StoreError> encode_manifest(const Manifest& manifest,
                                                 const StoreOptions& options) {
    wire::Writer writer(
        {.bytes = options.manifest_bytes, .elements = options.artifacts});
    magic(writer, "PGM1");
    writer.boolean(manifest.conflict);
    writer.u64(manifest.key.size());
    writer.raw(manifest.key);
    writer.raw(manifest.inputs.bytes);
    writer.raw(manifest.result.bytes);
    writer.count(manifest.artifacts.size());
    for (const auto& artifact : manifest.artifacts) {
        writer.string(artifact.name);
        writer.u8(static_cast<std::uint8_t>(artifact.object.kind));
        writer.boolean(artifact.required);
        writer.u64(artifact.object.size);
        writer.raw(artifact.object.digest.bytes);
    }
    const auto body = writer.finish();
    if (!body)
        return std::unexpected(
            codec_error(body.error(), StoreErrorCode::InvalidRequest));
    if (options.manifest_bytes < checksum_size ||
        body->size() > options.manifest_bytes - checksum_size)
        return std::unexpected(error(StoreErrorCode::LimitExceeded));
    Bytes bytes = *body;
    const auto hash = fingerprint("pagos.query-manifest.v1", bytes);
    bytes.insert(bytes.end(), hash.bytes.begin(), hash.bytes.end());
    return bytes;
}
std::expected<Manifest, StoreError> read_manifest(const fs::path& path,
                                                  const Bytes& key,
                                                  const Digest& inputs,
                                                  const StoreOptions& options) {
    const auto bytes = read_file(options, path, options.manifest_bytes,
                                 StoreErrorCode::CorruptManifest);
    if (!bytes)
        return std::unexpected(bytes.error());
    if (bytes->size() < checksum_size)
        return std::unexpected(error(StoreErrorCode::CorruptManifest));
    const auto body = std::span(*bytes).first(bytes->size() - checksum_size);
    const auto hash = fingerprint("pagos.query-manifest.v1", body);
    if (!std::ranges::equal(hash.bytes, std::span(*bytes).last(checksum_size)))
        return std::unexpected(error(StoreErrorCode::CorruptManifest));
    wire::Reader reader(
        body, {.bytes = options.manifest_bytes, .elements = options.artifacts});
    magic(reader, "PGM1");
    Manifest manifest;
    manifest.conflict = reader.boolean();
    const auto size = reader.u64();
    if (size > options.codec.bytes)
        reader.fail(Error::LimitExceeded);
    if (size > reader.remaining())
        reader.fail(Error::InvalidFormat);
    const auto key_bytes =
        reader.raw(reader.good() ? static_cast<std::size_t>(size) : 0);
    manifest.key.assign(key_bytes.begin(), key_bytes.end());
    manifest.inputs = digest(reader);
    manifest.result = digest(reader);
    const auto count = reader.count(4 + 1 + 1 + 8 + 32);
    manifest.artifacts.reserve(count);
    bool required = false;
    std::string previous;
    for (std::uint32_t index = 0; index < count && reader.good(); ++index) {
        StoredArtifact artifact;
        artifact.name = reader.string();
        artifact.object.kind = static_cast<ObjectKind>(reader.u8());
        artifact.required = reader.boolean();
        artifact.object.size = reader.u64();
        artifact.object.digest = digest(reader);
        if (!valid_name(artifact.name) || artifact.name <= previous ||
            !valid_kind(artifact.object.kind))
            reader.fail(Error::InvalidFormat);
        if (artifact.object.size > options.object_bytes)
            reader.fail(Error::LimitExceeded);
        previous = artifact.name;
        required |= artifact.required;
        manifest.artifacts.push_back(std::move(artifact));
    }
    if ((manifest.conflict && count != 0) || (!manifest.conflict && !required))
        reader.fail(Error::InvalidFormat);
    if (const auto result = reader.finish(); !result)
        return std::unexpected(
            codec_error(result.error(), StoreErrorCode::CorruptManifest));
    const auto decoded = decode_query_key(manifest.key, options.codec);
    if (!decoded)
        return std::unexpected(
            codec_error(decoded.error(), StoreErrorCode::CorruptManifest));
    const auto canonical = encode_query_key(*decoded, options.codec);
    if (!canonical || *canonical != manifest.key)
        return std::unexpected(error(StoreErrorCode::CorruptManifest));
    if (manifest.key != key)
        return std::unexpected(error(StoreErrorCode::KeyMismatch));
    if (manifest.inputs != inputs)
        return std::unexpected(error(StoreErrorCode::InputMismatch));
    return manifest;
}

std::expected<void, StoreError> load_artifacts(Manifest& manifest,
                                               const fs::path& root,
                                               const StoreOptions& options) {
    for (auto& artifact : manifest.artifacts) {
        auto bytes = read_cas_object(root, options, artifact.object);
        if (!bytes) {
            if (artifact.required)
                return std::unexpected(bytes.error());
        } else
            artifact.bytes = std::move(*bytes);
    }
    return {};
}

// A temporary is closed successfully before linking/renaming. Normal failures
// remove it; abrupt process exit may leave it for offline maintenance.
std::expected<void, StoreError> atomic_file(const fs::path& root,
                                            const StoreOptions& options,
                                            const fs::path& destination,
                                            std::span<const std::uint8_t> bytes,
                                            bool object) {
    llvm::SmallString<128> temporary;
    int fd = -1;
    const auto model =
        (root / "tmp" / "publish-%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%").string();
    if (const auto ec = llvm_fs::createUniqueFile(model, fd, temporary,
                                                  llvm_fs::OF_None, 0600))
        return std::unexpected(error(StoreErrorCode::IoFailure, ec));
    struct Remove {
        int fd;
        llvm::SmallString<128>& path;
        bool adopted{};
        ~Remove() {
            if (!adopted)
                (void)llvm::sys::Process::SafelyCloseFileDescriptor(fd);
            (void)llvm_fs::remove(path);
        }
    } remove{fd, temporary};
    llvm::raw_fd_ostream stream(fd, true);
    remove.adopted = true;
    // Always clear stream errors: LLVM's destructor otherwise treats them as
    // fatal. These failures must remain recoverable cache misses.
    struct Clear {
        llvm::raw_fd_ostream& stream;
        bool closed{};
        ~Clear() {
            if (!closed)
                stream.close();
            stream.clear_error();
        }
    } clear{stream};
    if (const auto result =
            checkpoint(options, IoOperation::TemporaryCreated, destination);
        !result)
        return result;
    const auto half = bytes.size() / 2;
    stream.write(reinterpret_cast<const char*>(bytes.data()), half);
    stream.flush();
    if (stream.has_error())
        return std::unexpected(
            error(StoreErrorCode::IoFailure, stream.error()));
    if (const auto result =
            checkpoint(options, IoOperation::WriteTemporary, destination);
        !result)
        return result;
    stream.write(reinterpret_cast<const char*>(bytes.data() + half),
                 bytes.size() - half);
    stream.flush();
    if (stream.has_error())
        return std::unexpected(
            error(StoreErrorCode::IoFailure, stream.error()));
    if (const auto result =
            checkpoint(options, IoOperation::CloseTemporary, destination);
        !result)
        return result;
    stream.close();
    clear.closed = true;
    if (stream.has_error())
        return std::unexpected(
            error(StoreErrorCode::IoFailure, stream.error()));
    const auto operation =
        object ? IoOperation::PublishObject : IoOperation::PublishManifest;
    if (const auto result = checkpoint(options, operation, destination);
        !result)
        return result;
    const auto ec =
        object ? llvm_fs::create_hard_link(temporary, destination.string())
               : llvm_fs::rename(temporary, destination.string());
    if (ec && !(object && ec == std::errc::file_exists))
        return std::unexpected(error(StoreErrorCode::IoFailure, ec));
    return checkpoint(options,
                      object ? IoOperation::ObjectPublished
                             : IoOperation::ManifestPublished,
                      destination);
}

std::expected<void, StoreError> publish_object(const fs::path& root,
                                               const StoreOptions& options,
                                               const ArtifactInput& artifact,
                                               const ObjectRef& reference) {
    const auto destination = object_path(root, reference.digest);
    auto old = read_cas_object(root, options, reference);
    if (old) {
        if (*old != artifact.bytes)
            return std::unexpected(error(StoreErrorCode::CorruptObject));
        return {};
    }
    if (old.error().code != StoreErrorCode::MissingObject)
        return std::unexpected(old.error());
    if (const auto result = directories(options, destination.parent_path());
        !result)
        return result;
    wire::Writer writer({.bytes = options.object_bytes + object_header_size});
    magic(writer, "PGO1");
    writer.u8(static_cast<std::uint8_t>(reference.kind));
    writer.u64(reference.size);
    writer.raw(reference.digest.bytes);
    writer.raw(artifact.bytes);
    const auto bytes = writer.finish();
    if (!bytes)
        return std::unexpected(
            codec_error(bytes.error(), StoreErrorCode::InvalidRequest));
    if (const auto result =
            atomic_file(root, options, destination, *bytes, true);
        !result)
        return result;
    // An independent writer may have won the no-replace link. Verify it too.
    old = read_cas_object(root, options, reference);
    if (!old)
        return std::unexpected(old.error());
    if (*old != artifact.bytes)
        return std::unexpected(error(StoreErrorCode::CorruptObject));
    return {};
}
std::expected<Bytes, StoreError> canonical_key(const QueryKey& key,
                                               const StoreOptions& options,
                                               const fs::path& root) {
    if (!valid_options(options, root))
        return std::unexpected(error(StoreErrorCode::InvalidRequest));
    const auto bytes = encode_query_key(key, options.codec);
    if (!bytes)
        return std::unexpected(
            codec_error(bytes.error(), StoreErrorCode::InvalidRequest));
    // Encoders can describe future versions; this reader/store cannot.
    if (const auto decoded = decode_query_key(*bytes, options.codec); !decoded)
        return std::unexpected(
            codec_error(decoded.error(), StoreErrorCode::InvalidRequest));
    return *bytes;
}
} // namespace

FileStore::FileStore(fs::path root, StoreOptions options)
    : root_(std::move(root)), options_(std::move(options)) {}

std::expected<Candidate, StoreError>
FileStore::lookup(const QueryKey& key, const Digest& verified_inputs) const {
    return lookup_candidate(key, verified_inputs, true);
}

std::expected<Bytes, StoreError>
FileStore::read_object(const ObjectRef& reference) const {
    try {
        if (!valid_kind(reference.kind))
            return std::unexpected(error(StoreErrorCode::InvalidRequest));
        return read_cas_object(root_, options_, reference);
    } catch (const std::bad_alloc&) {
        return std::unexpected(error(StoreErrorCode::OutOfMemory));
    } catch (const std::system_error& failure) {
        return std::unexpected(
            error(StoreErrorCode::IoFailure, failure.code()));
    }
}

std::expected<ObjectRef, StoreError>
FileStore::write_object(ObjectKind kind, Bytes bytes) const {
    try {
        if (!valid_options(options_, root_))
            return std::unexpected(error(StoreErrorCode::InvalidRequest));
        if (const auto checked = payload_header(kind, bytes, options_);
            !checked)
            return std::unexpected(checked.error());
        const ObjectRef reference{kind, object_digest(kind, bytes),
                                  bytes.size()};
        if (const auto ready = directories(options_, root_ / "tmp"); !ready)
            return std::unexpected(ready.error());
        const ArtifactInput artifact{"object", kind, std::move(bytes), true};
        if (const auto written =
                publish_object(root_, options_, artifact, reference);
            !written)
            return std::unexpected(written.error());
        return reference;
    } catch (const std::bad_alloc&) {
        return std::unexpected(error(StoreErrorCode::OutOfMemory));
    } catch (const std::system_error& failure) {
        return std::unexpected(
            error(StoreErrorCode::IoFailure, failure.code()));
    }
}

std::expected<Candidate, StoreError>
FileStore::lookup_references(const QueryKey& key,
                             const Digest& verified_inputs) const {
    return lookup_candidate(key, verified_inputs, false);
}

std::expected<Candidate, StoreError>
FileStore::lookup_candidate(const QueryKey& key, const Digest& verified_inputs,
                            bool load_objects) const {
    try {
        const auto bytes = canonical_key(key, options_, root_);
        if (!bytes)
            return std::unexpected(bytes.error());
        const auto hash = fingerprint("pagos.query-key.v1", *bytes);
        QueryLock lock(options_);
        if (const auto result = lock.acquire(
                query_directory(root_, hash) / (hash.hex() + ".lock"), false);
            !result)
            return std::unexpected(result.error());
        if (disabled_inputs.contains(disabled_id(root_, hash, verified_inputs)))
            return std::unexpected(error(StoreErrorCode::DeterminismConflict));
        auto manifest =
            read_manifest(manifest_path(root_, hash, verified_inputs), *bytes,
                          verified_inputs, options_);
        if (!manifest)
            return std::unexpected(manifest.error());
        if (manifest->conflict)
            return std::unexpected(error(StoreErrorCode::DeterminismConflict));
        // Keep the eager lookup's original lock boundary. The reference-only
        // path releases it after validating metadata; CAS payloads are
        // immutable.
        if (load_objects) {
            if (const auto result = load_artifacts(*manifest, root_, options_);
                !result)
                return std::unexpected(result.error());
        }
        return Candidate{key, verified_inputs, manifest->result,
                         std::move(manifest->artifacts)};
    } catch (const std::bad_alloc&) {
        return std::unexpected(error(StoreErrorCode::OutOfMemory));
    } catch (const std::system_error& failure) {
        return std::unexpected(
            error(StoreErrorCode::IoFailure, failure.code()));
    }
}

std::expected<PublishResult, StoreError>
FileStore::publish(const QueryKey& key, const Digest& verified_inputs,
                   const Digest& semantic_result,
                   std::span<const ArtifactInput> artifacts) const {
    try {
        const auto bytes = canonical_key(key, options_, root_);
        if (!bytes)
            return std::unexpected(bytes.error());
        if (artifacts.empty() || artifacts.size() > options_.artifacts)
            return std::unexpected(error(artifacts.empty()
                                             ? StoreErrorCode::InvalidRequest
                                             : StoreErrorCode::LimitExceeded));
        Manifest next{false, *bytes, verified_inputs, semantic_result, {}};
        std::vector<const ArtifactInput*> ordered;
        for (const auto& artifact : artifacts) {
            if (!valid_name(artifact.name))
                return std::unexpected(error(StoreErrorCode::InvalidRequest));
            if (const auto result =
                    payload_header(artifact.kind, artifact.bytes, options_);
                !result)
                return std::unexpected(result.error());
            ordered.push_back(&artifact);
        }
        std::ranges::sort(ordered, {},
                          [](const auto* item) { return item->name; });
        bool required = false;
        std::string previous;
        for (const auto* artifact : ordered) {
            if (artifact->name == previous)
                return std::unexpected(error(StoreErrorCode::InvalidRequest));
            previous = artifact->name;
            required |= artifact->required;
            next.artifacts.push_back(
                {artifact->name,
                 {artifact->kind,
                  object_digest(artifact->kind, artifact->bytes),
                  artifact->bytes.size()},
                 artifact->required,
                 {}});
        }
        if (!required)
            return std::unexpected(error(StoreErrorCode::InvalidRequest));
        auto encoded = encode_manifest(next, options_);
        if (!encoded)
            return std::unexpected(encoded.error());
        const auto hash = fingerprint("pagos.query-key.v1", *bytes);
        const auto destination = manifest_path(root_, hash, verified_inputs);
        if (const auto result =
                directories(options_, destination.parent_path());
            !result)
            return std::unexpected(result.error());
        if (const auto result = directories(options_, root_ / "tmp"); !result)
            return std::unexpected(result.error());
        QueryLock lock(options_);
        if (const auto result = lock.acquire(
                query_directory(root_, hash) / (hash.hex() + ".lock"), true);
            !result)
            return std::unexpected(result.error());
        const auto disabled = disabled_id(root_, hash, verified_inputs);
        if (disabled_inputs.contains(disabled))
            return std::unexpected(error(StoreErrorCode::DeterminismConflict));
        auto old =
            read_manifest(destination, *bytes, verified_inputs, options_);
        if (old) {
            if (old->conflict)
                return std::unexpected(
                    error(StoreErrorCode::DeterminismConflict));
            // A dependency generation is isolated by verified_inputs. Only
            // this exact verified input can be declared nondeterministic.
            if (old->result != semantic_result) {
                disabled_inputs.insert(disabled);
                old->conflict = true;
                old->artifacts.clear();
                const auto tombstone = encode_manifest(*old, options_);
                if (!tombstone)
                    return std::unexpected(tombstone.error());
                if (const auto result = atomic_file(
                        root_, options_, destination, *tombstone, false);
                    !result) {
                    auto failure = result.error();
                    failure.code = StoreErrorCode::DeterminismConflict;
                    return std::unexpected(failure);
                }
                return std::unexpected(
                    error(StoreErrorCode::DeterminismConflict));
            }
            if (load_artifacts(*old, root_, options_))
                return PublishResult::AlreadyPresent;
        } else if (old.error().code != StoreErrorCode::NotFound &&
                   old.error().code != StoreErrorCode::CorruptManifest &&
                   old.error().code != StoreErrorCode::IncompatibleVersion &&
                   old.error().code != StoreErrorCode::KeyMismatch &&
                   old.error().code != StoreErrorCode::InputMismatch)
            return std::unexpected(old.error());
        for (std::size_t index = 0; index < ordered.size(); ++index)
            if (const auto result =
                    publish_object(root_, options_, *ordered[index],
                                   next.artifacts[index].object);
                !result)
                return std::unexpected(result.error());
        if (const auto result =
                atomic_file(root_, options_, destination, *encoded, false);
            !result)
            return std::unexpected(result.error());
        return PublishResult::Published;
    } catch (const std::bad_alloc&) {
        return std::unexpected(error(StoreErrorCode::OutOfMemory));
    } catch (const std::system_error& failure) {
        return std::unexpected(
            error(StoreErrorCode::IoFailure, failure.code()));
    }
}
} // namespace pagos::cache
