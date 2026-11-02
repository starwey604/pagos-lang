#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace pagos::source {
// Native filesystem paths cross CLI/LLVM boundaries as UTF-8 on Windows.
// These conversions must never be used to validate protocol byte strings.
inline std::filesystem::path path_from_utf8(std::string_view text) {
#ifdef _WIN32
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
#else
    return std::filesystem::path(text);
#endif
}
inline std::string path_to_utf8(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
} // namespace pagos::source
