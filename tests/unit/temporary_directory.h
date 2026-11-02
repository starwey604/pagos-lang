#pragma once

#include "pagos/source/path.h"

#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>

#include <filesystem>

namespace pagos::test {
// A timestamp and process-local counter can collide across parallel CTest
// processes. Let the filesystem reserve a unique directory atomically.
inline std::filesystem::path temporary_directory(const char* prefix) {
    llvm::SmallString<128> path;
    if (const auto error = llvm::sys::fs::createUniqueDirectory(prefix, path))
        throw std::filesystem::filesystem_error("create test directory", error);
    return source::path_from_utf8(std::string_view(path.data(), path.size()));
}

class TemporaryDirectory {
  public:
    explicit TemporaryDirectory(const char* prefix)
        : path_(temporary_directory(prefix)) {}
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};
} // namespace pagos::test
