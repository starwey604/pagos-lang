#include "pagos/cache/task_store.h"
#include "storage_io.h"
#include "wire.h"

#include <llvm/ADT/ScopeExit.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

namespace pagos::cache {
namespace {
namespace fs = std::filesystem;
namespace sysfs = llvm::sys::fs;
constexpr std::string_view marker = "pagos.query.task.v1\n";
constexpr std::size_t head_bytes = 127;
constexpr std::string_view prefix = "pagos-query-";
StoreError failure(StoreErrorCode code, std::error_code ec = {}) {
    return {code, ec, {}};
}
void require(bool condition,
             StoreErrorCode code = StoreErrorCode::CorruptManifest) {
    if (!condition)
        throw failure(code);
}
void checked(std::error_code ec) {
    if (ec)
        throw failure(StoreErrorCode::IoFailure, ec);
}
void inject(const StoreOptions& options, IoOperation operation,
            const fs::path& path) {
    if (options.fault)
        checked(options.fault(operation, path));
}
std::mutex lifecycle;
std::condition_variable lifecycle_changed;
std::timed_mutex writers;
struct Lease;
struct RegisteredLease {
    std::weak_ptr<Lease> weak;
    const Lease* owner;
};
std::unordered_map<std::string, RegisteredLease> live;
struct Descriptor {
    int fd{-1};
    ~Descriptor() {
        if (fd != -1) {
            (void)sysfs::unlockFile(fd);
            (void)llvm::sys::Process::SafelyCloseFileDescriptor(fd);
        }
    }
};
struct Lease : Descriptor {
    fs::path path;
    ~Lease() {
        {
            const std::lock_guard guard(lifecycle);
            const auto found = live.find(path.generic_string());
            if (found != live.end() && found->second.owner == this)
                live.erase(found);
            if (fd != -1) {
                (void)sysfs::unlockFile(fd);
                (void)llvm::sys::Process::SafelyCloseFileDescriptor(fd);
                fd = -1;
            }
        }
        lifecycle_changed.notify_all();
    }
};
void lock_descriptor(int fd, const StoreOptions& options,
                     sysfs::LockKind kind) {
    const auto ec = sysfs::tryLockFile(fd, options.lock_timeout, kind);
    if (ec)
        throw failure(ec == std::errc::no_lock_available
                          ? StoreErrorCode::LockBusy
                          : StoreErrorCode::IoFailure,
                      ec);
}
void validate_marker(int fd, const fs::path& path) {
    sysfs::file_status opened, named;
    checked(sysfs::status(fd, opened));
    checked(sysfs::status(path.string(), named));
    require(opened.getUniqueID() == named.getUniqueID() &&
            sysfs::is_regular_file(opened) &&
            opened.getSize() == marker.size());
    std::array<char, marker.size()> bytes{};
    auto read = sysfs::readNativeFile(sysfs::convertFDToNativeFile(fd), bytes);
    if (!read)
        throw failure(StoreErrorCode::IoFailure,
                      llvm::errorToErrorCode(read.takeError()));
    require(*read == marker.size() &&
            std::string_view(bytes.data(), bytes.size()) == marker);
}
std::shared_ptr<Lease> acquire(const fs::path& parent, Digest task,
                               TaskAccess access,
                               const TaskStoreOptions& options) {
    // Declare before the guard: initialization failure closes the descriptor
    // only after releasing lifecycle, including failure inserting the registry.
    auto lease = std::make_shared<Lease>();
    std::unique_lock guard(lifecycle);
    std::error_code ec;
    if (access == TaskAccess::ReadWrite) {
        inject(options.store, IoOperation::CreateDirectories, parent);
        fs::create_directories(parent, ec);
        checked(ec);
    }
    const auto canonical = fs::canonical(parent, ec);
    if (ec)
        throw failure(ec == std::errc::no_such_file_or_directory
                          ? StoreErrorCode::NotFound
                          : StoreErrorCode::IoFailure,
                      ec);
    lease->path = canonical / (std::string(prefix) + task.hex());
    const auto key = lease->path.generic_string();
    while (true) {
        const auto found = live.find(key);
        if (found == live.end())
            break;
        if (auto existing = found->second.weak.lock()) {
            // This unused lease must not erase the existing registry entry.
            lease->path.clear();
            return existing;
        }
        // The last shared_ptr may already be expiring while its destructor
        // waits for lifecycle. Opening a second fd before the old one closes
        // would let that close release our new process-scoped lock.
        require(lifecycle_changed.wait_for(
                    guard, options.store.lock_timeout,
                    [&] {
                        const auto current = live.find(key);
                        return current == live.end() ||
                               !current->second.weak.expired();
                    }),
                StoreErrorCode::LockBusy);
    }
    bool created = false;
    if (access == TaskAccess::ReadWrite) {
        created = fs::create_directory(lease->path, ec);
        checked(ec);
    }
    llvm::scope_exit rollback([&] {
        if (created) {
            std::error_code ignored;
            fs::remove_all(lease->path, ignored);
        }
    });
    const auto status = fs::symlink_status(lease->path, ec);
    if (ec)
        throw failure(ec == std::errc::no_such_file_or_directory
                          ? StoreErrorCode::NotFound
                          : StoreErrorCode::IoFailure,
                      ec);
    require(fs::is_directory(status), StoreErrorCode::InvalidRequest);
    const auto lock_path = lease->path / "session.lock";
    if (created) {
        checked(sysfs::openFileForReadWrite(lock_path.string(), lease->fd,
                                            sysfs::CD_CreateNew, sysfs::OF_None,
                                            0600));
        lock_descriptor(lease->fd, options.store, sysfs::LockKind::Exclusive);
        llvm::raw_fd_ostream output(lease->fd, false);
        output << marker;
        output.flush();
        const auto error = output.error();
        output.clear_error();
        checked(error);
        lock_descriptor(lease->fd, options.store, sysfs::LockKind::Shared);
    } else {
        const auto lock_status = fs::symlink_status(lock_path, ec);
        require(!ec && fs::is_regular_file(lock_status),
                StoreErrorCode::LockBusy);
        require(fs::file_size(lock_path, ec) == marker.size() && !ec,
                StoreErrorCode::LockBusy);
        checked(sysfs::openFileForRead(lock_path.string(), lease->fd));
        lock_descriptor(lease->fd, options.store, sysfs::LockKind::Shared);
        validate_marker(lease->fd, lock_path);
    }
    live[key] = RegisteredLease{lease, lease.get()};
    rollback.release();
    return lease;
}
Bytes encode_head(Digest task, const TaskHead& head) {
    wire::Writer writer({.bytes = head_bytes});
    wire::header(writer, ObjectKind::QueryTaskManifest);
    writer.u32(task_store_version);
    writer.raw(task.bytes);
    writer.u64(head.generation);
    writer.u8(static_cast<std::uint8_t>(head.status));
    writer.u8(static_cast<std::uint8_t>(head.manifest.kind));
    writer.raw(head.manifest.digest.bytes);
    writer.u64(head.manifest.size);
    auto bytes = writer.finish();
    require(bytes.has_value());
    const auto hash = fingerprint("pagos.task-head.v1", *bytes);
    bytes->insert(bytes->end(), hash.bytes.begin(), hash.bytes.end());
    return std::move(*bytes);
}
Digest digest(wire::Reader& reader) {
    Digest value;
    const auto bytes = reader.raw(32);
    if (reader.good())
        std::ranges::copy(bytes, value.bytes.begin());
    return value;
}
std::optional<TaskHead> read_head(const fs::path& root, Digest task,
                                  const StoreOptions& options) {
    auto bytes = detail::read_file(options, root / "head", head_bytes);
    if (!bytes) {
        if (bytes.error().code == StoreErrorCode::NotFound)
            return {};
        throw bytes.error();
    }
    require(bytes->size() == head_bytes);
    const auto payload = std::span(*bytes).first(head_bytes - 32);
    const auto hash = fingerprint("pagos.task-head.v1", payload);
    require(std::ranges::equal(hash.bytes, std::span(*bytes).last(32)));
    wire::Reader reader(payload, {.bytes = head_bytes});
    wire::header(reader, ObjectKind::QueryTaskManifest);
    require(reader.u32() == task_store_version,
            StoreErrorCode::IncompatibleVersion);
    require(digest(reader) == task);
    TaskHead head;
    head.generation = reader.u64();
    head.status = static_cast<TaskStatus>(reader.u8());
    head.manifest = {static_cast<ObjectKind>(reader.u8()), digest(reader),
                     reader.u64()};
    require(reader.finish().has_value() && head.generation &&
            head.status <= TaskStatus::Completed &&
            head.manifest.kind == ObjectKind::QueryTaskManifest &&
            head.manifest.size >= 9 &&
            head.manifest.size <= options.object_bytes);
    return head;
}
std::size_t used_bytes(const fs::path& root, std::size_t limit) {
    std::size_t total{};
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const auto status = entry.symlink_status();
        require(!fs::is_symlink(status), StoreErrorCode::InvalidRequest);
        if (!fs::is_regular_file(status))
            continue;
        const auto size = entry.file_size();
        require(size <= limit - total, StoreErrorCode::LimitExceeded);
        total += static_cast<std::size_t>(size);
    }
    return total;
}
} // namespace
struct TaskStore::Impl {
    std::shared_ptr<Lease> lease;
    Digest task;
    TaskAccess access;
    TaskStoreOptions options;
};
struct TaskWriter::Impl : Descriptor {
    std::shared_ptr<TaskStore::Impl> store;
    std::unique_lock<std::timed_mutex> lock{writers, std::defer_lock};
    std::uint64_t generation{};
    std::size_t charged{};
    bool finished{};
    ~Impl() {
        // Close the process-scoped OS lock before releasing the thread mutex.
        if (fd != -1) {
            (void)sysfs::unlockFile(fd);
            (void)llvm::sys::Process::SafelyCloseFileDescriptor(fd);
            fd = -1;
        }
    }
};
std::expected<TaskStore, StoreError> TaskStore::open(const fs::path& parent,
                                                     Digest task,
                                                     TaskAccess access,
                                                     TaskStoreOptions options) {
    try {
        require(!parent.empty() &&
                    parent.native().find('\0') == fs::path::string_type::npos &&
                    options.store.lock_timeout.count() >= 0 &&
                    (access == TaskAccess::ReadOnly ||
                     options.disk_bytes >= marker.size() + head_bytes),
                StoreErrorCode::InvalidRequest);
        auto result = std::make_shared<Impl>();
        result->lease = acquire(parent, task, access, options);
        result->task = task;
        result->access = access;
        result->options = std::move(options);
        return TaskStore(std::move(result));
    } catch (const StoreError& error) {
        return std::unexpected(error);
    } catch (const fs::filesystem_error& error) {
        return std::unexpected(
            failure(StoreErrorCode::IoFailure, error.code()));
    } catch (const std::bad_alloc&) {
        return std::unexpected(failure(StoreErrorCode::OutOfMemory));
    }
}
const fs::path& TaskStore::directory() const { return impl_->lease->path; }
Digest TaskStore::task() const { return impl_->task; }
std::expected<std::optional<TaskHead>, StoreError> TaskStore::head() const {
    try {
        return read_head(directory(), task(), impl_->options.store);
    } catch (const StoreError& error) {
        return std::unexpected(error);
    } catch (const fs::filesystem_error& error) {
        return std::unexpected(
            failure(StoreErrorCode::IoFailure, error.code()));
    } catch (const std::bad_alloc&) {
        return std::unexpected(failure(StoreErrorCode::OutOfMemory));
    }
}
std::expected<Bytes, StoreError> TaskStore::read(const ObjectRef& ref) const {
    if (ref.kind != ObjectKind::QueryTaskManifest &&
        ref.kind != ObjectKind::QueryStateSegment)
        return std::unexpected(failure(StoreErrorCode::InvalidRequest));
    return FileStore(directory(), impl_->options.store).read_object(ref);
}
std::expected<TaskWriter, StoreError>
TaskStore::begin(std::uint64_t expected) const {
    try {
        require(impl_->access == TaskAccess::ReadWrite,
                StoreErrorCode::ReadOnly);
        auto writer = std::make_unique<TaskWriter::Impl>();
        writer->store = impl_;
        require(writer->lock.try_lock_for(impl_->options.store.lock_timeout),
                StoreErrorCode::LockBusy);
        const auto path = directory() / "writer.lock";
        inject(impl_->options.store, IoOperation::LockFile, path);
        checked(sysfs::openFileForWrite(path.string(), writer->fd,
                                        sysfs::CD_OpenAlways, sysfs::OF_None,
                                        0600));
        lock_descriptor(writer->fd, impl_->options.store,
                        sysfs::LockKind::Exclusive);
        const auto head = read_head(directory(), task(), impl_->options.store);
        const auto generation = head ? head->generation : 0;
        require(generation == expected, StoreErrorCode::StaleGeneration);
        require(generation < UINT64_MAX, StoreErrorCode::LimitExceeded);
        writer->generation = generation + 1;
        writer->charged = used_bytes(directory(), impl_->options.disk_bytes);
        return TaskWriter(std::move(writer));
    } catch (const StoreError& error) {
        return std::unexpected(error);
    } catch (const fs::filesystem_error& error) {
        return std::unexpected(
            failure(StoreErrorCode::IoFailure, error.code()));
    } catch (const std::bad_alloc&) {
        return std::unexpected(failure(StoreErrorCode::OutOfMemory));
    }
}
TaskWriter::TaskWriter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TaskWriter::~TaskWriter() = default;
TaskWriter::TaskWriter(TaskWriter&&) noexcept = default;
TaskWriter& TaskWriter::operator=(TaskWriter&&) noexcept = default;
std::size_t TaskWriter::charged_bytes() const { return impl_->charged; }
std::expected<ObjectRef, StoreError> TaskWriter::write(ObjectKind kind,
                                                       Bytes bytes) {
    try {
        require(!impl_->finished && (kind == ObjectKind::QueryTaskManifest ||
                                     kind == ObjectKind::QueryStateSegment),
                StoreErrorCode::InvalidRequest);
        const auto& state = *impl_->store;
        FileStore objects(state.lease->path, state.options.store);
        const auto ref = detail::object_reference(kind, bytes);
        auto existing = objects.read_object(ref);
        if (existing)
            return ref;
        if (existing.error().code != StoreErrorCode::MissingObject)
            return std::unexpected(existing.error());
        const auto remaining = state.options.disk_bytes - impl_->charged;
        require(remaining >= head_bytes && ref.size <= remaining - head_bytes &&
                    remaining - head_bytes - ref.size >= 49,
                StoreErrorCode::LimitExceeded);
        impl_->charged += static_cast<std::size_t>(ref.size) + 49;
        return objects.write_object(kind, std::move(bytes));
    } catch (const StoreError& error) {
        return std::unexpected(error);
    } catch (const std::bad_alloc&) {
        return std::unexpected(failure(StoreErrorCode::OutOfMemory));
    }
}
std::expected<TaskHead, StoreError> TaskWriter::commit(ObjectRef manifest,
                                                       TaskStatus status) {
    try {
        require(!impl_->finished &&
                    manifest.kind == ObjectKind::QueryTaskManifest &&
                    status <= TaskStatus::Completed,
                StoreErrorCode::InvalidRequest);
        const auto& store = *impl_->store;
        auto object = FileStore(store.lease->path, store.options.store)
                          .read_object(manifest);
        if (!object)
            return std::unexpected(object.error());
        require(head_bytes <= store.options.disk_bytes - impl_->charged,
                StoreErrorCode::LimitExceeded);
        const TaskHead head{impl_->generation, manifest, status};
        const auto bytes = encode_head(store.task, head);
        impl_->finished = true;
        if (auto result =
                detail::replace_file(store.lease->path, store.options.store,
                                     store.lease->path / "head", bytes);
            !result)
            return std::unexpected(result.error());
        return head;
    } catch (const StoreError& error) {
        return std::unexpected(error);
    } catch (const std::bad_alloc&) {
        return std::unexpected(failure(StoreErrorCode::OutOfMemory));
    }
}
std::expected<TaskCleanupStats, StoreError>
reclaim_tasks(const fs::path& parent, TaskCleanupPolicy policy) {
    try {
        require(!parent.empty(), StoreErrorCode::InvalidRequest);
        std::error_code ec;
        const auto status = fs::symlink_status(parent, ec);
        if (ec == std::errc::no_such_file_or_directory)
            return TaskCleanupStats{};
        checked(ec);
        require(fs::is_directory(status), StoreErrorCode::InvalidRequest);
        const auto canonical = fs::canonical(parent);
        TaskCleanupStats stats;
        for (const auto& entry : fs::directory_iterator(canonical)) {
            const auto name = entry.path().filename().string();
            if (!name.starts_with(prefix))
                continue;
            ++stats.examined;
            const std::lock_guard guard(lifecycle);
            if (const auto found = live.find(entry.path().generic_string());
                found != live.end()) {
                // An expired weak_ptr may have a destructor still waiting
                // for this guard. It remains active until its fd is closed.
                ++stats.active;
                continue;
            }
            const auto candidate = entry.symlink_status(ec);
            if (ec || !fs::is_directory(candidate) ||
                name.size() != prefix.size() + 64) {
                ++stats.skipped;
                continue;
            }
            const auto hex = std::string_view(name).substr(prefix.size());
            if (!std::ranges::all_of(hex, [](char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                })) {
                ++stats.skipped;
                continue;
            }
            Digest task;
            for (unsigned i = 0; i < 32; ++i) {
                const auto digit = [](char c) {
                    return c <= '9' ? c - '0' : c - 'a' + 10;
                };
                task.bytes[i] = static_cast<std::uint8_t>(
                    digit(hex[2 * i]) * 16 + digit(hex[2 * i + 1]));
            }
            const auto marker_path = entry.path() / "session.lock";
            const auto marker_status = fs::symlink_status(marker_path, ec);
            if (ec || !fs::is_regular_file(marker_status) ||
                fs::file_size(marker_path, ec) != marker.size() || ec) {
                ++stats.skipped;
                continue;
            }
            Descriptor owner;
            if (sysfs::openFileForReadWrite(marker_path.string(), owner.fd,
                                            sysfs::CD_OpenExisting,
                                            sysfs::OF_None)) {
                ++stats.errors;
                continue;
            }
            const auto locked =
                sysfs::tryLockFile(owner.fd, std::chrono::milliseconds(0));
            if (locked) {
                ++stats.active;
                continue;
            }
            try {
                validate_marker(owner.fd, marker_path);
                const auto head = read_head(entry.path(), task, {});
                if (policy == TaskCleanupPolicy::TerminalOnly &&
                    (!head || head->status == TaskStatus::Paused ||
                     head->status == TaskStatus::AwaitingDelivery)) {
                    ++stats.skipped;
                    continue;
                }
            } catch (const StoreError&) {
                ++stats.skipped;
                continue;
            }
            fs::remove_all(entry.path(), ec);
            if (ec)
                ++stats.errors;
            else
                ++stats.reclaimed;
        }
        return stats;
    } catch (const StoreError& error) {
        return std::unexpected(error);
    } catch (const fs::filesystem_error& error) {
        return std::unexpected(
            failure(StoreErrorCode::IoFailure, error.code()));
    } catch (const std::bad_alloc&) {
        return std::unexpected(failure(StoreErrorCode::OutOfMemory));
    }
}
} // namespace pagos::cache
