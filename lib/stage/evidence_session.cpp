#include "evidence_session.h"

#include <llvm/ADT/ScopeExit.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/raw_ostream.h>

#include <array>
#include <mutex>
#include <unordered_set>

namespace pagos::stage::provenance {
namespace {
namespace fs = std::filesystem;
namespace sysfs = llvm::sys::fs;
constexpr std::string_view marker = "pagos.evidence.session.v1\n";
constexpr std::string_view lock_name = "session.lock";
// Some OS locks are process-scoped. Never open/close an active session's lock
// again in this process; doing so could release the owner's process lock.
std::mutex lifecycle;
std::unordered_set<std::string> active;
EvidenceError io_error(std::error_code error) {
    return {EvidenceErrorCode::IoFailure,
            cache::StoreError{cache::StoreErrorCode::IoFailure, error, {}},
            {}};
}
struct Descriptor {
    int value{-1};
    ~Descriptor() {
        if (value != -1) {
            (void)sysfs::unlockFile(value);
            (void)llvm::sys::Process::SafelyCloseFileDescriptor(value);
        }
    }
};
} // namespace
EvidenceSession::~EvidenceSession() {
    const std::lock_guard lock(lifecycle);
    if (!directory.empty()) {
        active.erase(directory.generic_string());
        std::error_code ignored;
        fs::remove_all(directory, ignored);
    }
    if (descriptor_ != -1) {
        (void)sysfs::unlockFile(descriptor_);
        (void)llvm::sys::Process::SafelyCloseFileDescriptor(descriptor_);
    }
}
std::expected<std::shared_ptr<EvidenceSession>, EvidenceError>
EvidenceSession::create(const fs::path& parent_path) {
    auto result = std::make_shared<EvidenceSession>();
    const std::lock_guard lock(lifecycle);
    std::error_code ec;
    const auto parent = fs::absolute(parent_path, ec).lexically_normal();
    if (ec)
        return std::unexpected(io_error(ec));
    fs::create_directories(parent, ec);
    if (ec)
        return std::unexpected(io_error(ec));
    const auto canonical = fs::canonical(parent, ec);
    if (ec)
        return std::unexpected(io_error(ec));
    llvm::SmallString<256> name;
    if (auto error = sysfs::createUniqueDirectory(
            (canonical / "pagos-evidence").string(), name))
        return std::unexpected(io_error(error));
    llvm::scope_exit rollback([&] { (void)sysfs::remove_directories(name); });
    // Own immediately, including initialization failures. result is destroyed
    // after the local lock so its destructor cannot recursively lock lifecycle.
    result->directory = std::string(name);
    rollback.release();
    if (auto error = sysfs::openFileForWrite(
            (result->directory / lock_name).string(), result->descriptor_,
            sysfs::CD_CreateNew, sysfs::OF_None, 0600))
        return std::unexpected(io_error(error));
    if (auto error = sysfs::tryLockFile(result->descriptor_,
                                        std::chrono::milliseconds(0)))
        return std::unexpected(io_error(error));
    llvm::raw_fd_ostream output(result->descriptor_, false);
    output << marker;
    output.flush();
    const auto error = output.error();
    output.clear_error();
    if (error)
        return std::unexpected(io_error(error));
    active.insert(result->directory.generic_string());
    return result;
}
std::expected<EvidenceCleanupStats, EvidenceError>
reclaim_evidence_sessions(const fs::path& parent_path) {
    EvidenceCleanupStats result;
    if (parent_path.empty())
        return std::unexpected(
            EvidenceError{EvidenceErrorCode::InvalidReference, {}, {}});
    std::error_code ec;
    const auto parent = fs::absolute(parent_path, ec).lexically_normal();
    if (ec)
        return std::unexpected(io_error(ec));
    const auto status = fs::symlink_status(parent, ec);
    if (ec == std::errc::no_such_file_or_directory)
        return result;
    if (ec)
        return std::unexpected(io_error(ec));
    if (!fs::is_directory(status))
        return std::unexpected(
            EvidenceError{EvidenceErrorCode::InvalidReference, {}, {}});
    const auto canonical = fs::canonical(parent, ec);
    if (ec)
        return std::unexpected(io_error(ec));
    fs::directory_iterator entries(canonical, ec);
    if (ec)
        return std::unexpected(io_error(ec));
    while (entries != fs::directory_iterator{}) {
        const auto path = entries->path();
        entries.increment(ec);
        if (ec)
            return std::unexpected(io_error(ec));
        if (!path.filename().string().starts_with("pagos-evidence-"))
            continue;
        ++result.examined;
        const std::lock_guard lock(lifecycle);
        if (active.contains(path.generic_string())) {
            ++result.active;
            continue;
        }
        const auto candidate = fs::symlink_status(path, ec);
        if (ec || !fs::is_directory(candidate)) {
            ++result.skipped;
            continue;
        }
        const auto owner_path = path / lock_name;
        const auto owner = fs::symlink_status(owner_path, ec);
        if (ec || !fs::is_regular_file(owner)) {
            ++result.skipped;
            continue;
        }
        // A creator acquires its lock before writing the marker. Skip an
        // incomplete marker before opening/locking it, so maintenance cannot
        // win the creation window and make a healthy new task fail to lock.
        const auto marker_size = fs::file_size(owner_path, ec);
        if (ec || marker_size != marker.size()) {
            ++result.skipped;
            continue;
        }
        Descriptor descriptor;
        if (sysfs::openFileForReadWrite(owner_path.string(), descriptor.value,
                                        sysfs::CD_OpenExisting,
                                        sysfs::OF_None)) {
            ++result.errors;
            continue;
        }
        const auto locked =
            sysfs::tryLockFile(descriptor.value, std::chrono::milliseconds(0));
        if (locked) {
            if (locked == std::errc::no_lock_available)
                ++result.active;
            else
                ++result.errors;
            continue;
        }
        sysfs::file_status opened;
        if (sysfs::status(descriptor.value, opened) ||
            opened.getSize() != marker.size()) {
            ++result.skipped;
            continue;
        }
        std::array<char, marker.size()> bytes{};
        auto read = sysfs::readNativeFile(
            sysfs::convertFDToNativeFile(descriptor.value), bytes);
        if (!read) {
            llvm::consumeError(read.takeError());
            ++result.errors;
            continue;
        }
        if (*read != marker.size() ||
            std::string_view(bytes.data(), bytes.size()) != marker) {
            ++result.skipped;
            continue;
        }
        fs::remove_all(path, ec);
        if (ec)
            ++result.errors;
        else
            ++result.reclaimed;
    }
    return result;
}
} // namespace pagos::stage::provenance
