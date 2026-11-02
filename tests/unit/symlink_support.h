#pragma once

#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace pagos::test {
// Only capability/permission failures justify skipping a symlink-only case.
// A bad fixture path or another I/O failure must still fail the test.
inline bool symlink_unavailable(std::error_code error) {
#ifdef _WIN32
    if (error ==
        std::error_code(ERROR_PRIVILEGE_NOT_HELD, std::system_category()))
        return true;
#endif
    return error == std::errc::permission_denied ||
           error == std::errc::operation_not_permitted ||
           error == std::errc::operation_not_supported ||
           error == std::errc::function_not_supported;
}
} // namespace pagos::test
