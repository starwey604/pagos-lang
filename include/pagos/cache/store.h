#pragma once

#include "pagos/cache/key.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <system_error>

namespace pagos::cache {

inline constexpr std::uint32_t store_format_version = 1;

enum class StoreErrorCode {
    NotFound,
    MissingObject,
    CorruptManifest,
    CorruptObject,
    IncompatibleVersion,
    KeyMismatch,
    InputMismatch,
    LimitExceeded,
    InvalidRequest,
    IoFailure,
    LockBusy,
    DeterminismConflict,
    OutOfMemory,
    ReadOnly,
    StaleGeneration
};
struct StoreError {
    StoreErrorCode code;
    std::error_code io;
    std::optional<Error> codec;
};

struct ObjectRef {
    ObjectKind kind{ObjectKind::Value};
    Digest digest;
    std::uint64_t size{}; // Payload bytes, excluding the CAS envelope.
    bool operator==(const ObjectRef&) const = default;
};
struct ArtifactInput {
    std::string name;
    ObjectKind kind{ObjectKind::Value};
    Bytes bytes; // A versioned canonical payload, e.g. encode_value().
    bool required{true};
};
struct StoredArtifact {
    std::string name;
    ObjectRef object;
    bool required{true};
    // Optional details may be absent/corrupt; required objects fail lookup.
    std::optional<Bytes> bytes;
};
struct Candidate {
    QueryKey key;
    Digest inputs;
    Digest semantic_result;
    std::vector<StoredArtifact> artifacts;
};
enum class PublishResult { Published, AlreadyPresent };

// Test/fault-injection boundary. Callbacks must not throw. WriteTemporary is
// called after a partial write; the other operations precede their IO, except
// TemporaryCreated/ObjectPublished/ManifestPublished, which follow it.
enum class IoOperation {
    CreateDirectories,
    ReadFile,
    LockFile,
    TemporaryCreated,
    WriteTemporary,
    CloseTemporary,
    PublishObject,
    ObjectPublished,
    PublishManifest,
    ManifestPublished
};
using FaultInjector = std::function<std::error_code(
    IoOperation, const std::filesystem::path& destination)>;
struct StoreOptions {
    Limits codec;
    std::size_t object_bytes{16 * 1024 * 1024};
    std::size_t manifest_bytes{1024 * 1024};
    std::size_t artifacts{32};
    std::chrono::milliseconds lock_timeout{100};
    FaultInjector fault;
};

// Constructor/lookup do not create directories. No online GC or Stage calls.
// `verified_inputs` is supplied by the semantic adapter after validating all
// dependencies (C2 initially uses the conservative module fingerprint).
// `semantic_result` fingerprints semantic content, not optional display data.
class FileStore {
  public:
    explicit FileStore(std::filesystem::path root, StoreOptions options = {});

    [[nodiscard]] std::expected<Candidate, StoreError>
    lookup(const QueryKey& key, const Digest& verified_inputs) const;
    // Validates only the manifest, key and inputs. Artifact bytes are empty;
    // required objects have NOT been checked. Adapters must validate their
    // required summary closure before accepting reuse, then load cold details
    // on demand. This is not an interchangeable semantic cache hit API.
    [[nodiscard]] std::expected<Candidate, StoreError>
    lookup_references(const QueryKey& key, const Digest& verified_inputs) const;
    // Reads and checks one complete CAS object, including its payload header.
    // Neither operation creates directories, locks or objects on disk.
    [[nodiscard]] std::expected<Bytes, StoreError>
    read_object(const ObjectRef& reference) const;
    // Leaf-object publication for immutable object DAGs. Does not publish a
    // query candidate; the root manifest must be committed last by its adapter.
    [[nodiscard]] std::expected<ObjectRef, StoreError>
    write_object(ObjectKind kind, Bytes bytes) const;
    [[nodiscard]] std::expected<PublishResult, StoreError>
    publish(const QueryKey& key, const Digest& verified_inputs,
            const Digest& semantic_result,
            std::span<const ArtifactInput> artifacts) const;

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

  private:
    [[nodiscard]] std::expected<Candidate, StoreError>
    lookup_candidate(const QueryKey& key, const Digest& verified_inputs,
                     bool load_objects) const;
    std::filesystem::path root_;
    StoreOptions options_;
};

} // namespace pagos::cache
