#ifndef PAGOS_TEST_SHARED_HOST_H
#define PAGOS_TEST_SHARED_HOST_H

#include "host_signals.h"
#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32
typedef HMODULE SharedHandle;
typedef FARPROC SharedSymbol;
static inline uint32_t shared_host_pid(void) {
    return (uint32_t)GetCurrentProcessId();
}
static inline SharedHandle shared_host_load(void) {
    WCHAR path[32768];
    DWORD size = GetEnvironmentVariableW(L"PAGOS_TEST_LIBRARY", path, 32768);
    if (!size || size >= 32768)
        return 0;
    return LoadLibraryExW(path, 0,
                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                              LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}
static inline SharedSymbol shared_host_symbol(SharedHandle handle,
                                              const char* name) {
    return GetProcAddress(handle, name);
}
static inline int shared_host_close(SharedHandle handle) {
    return FreeLibrary(handle) ? 0 : 1;
}
#else
#include <dlfcn.h>
#include <unistd.h>
typedef void* SharedHandle;
typedef void (*SharedSymbol)(void);
static inline uint32_t shared_host_pid(void) { return (uint32_t)getpid(); }
static inline SharedHandle shared_host_load(void) {
    return dlopen(getenv("PAGOS_TEST_LIBRARY"), RTLD_NOW | RTLD_LOCAL);
}
static inline SharedSymbol shared_host_symbol(SharedHandle handle,
                                              const char* name) {
    return (SharedSymbol)dlsym(handle, name);
}
static inline int shared_host_close(SharedHandle handle) {
    return dlclose(handle);
}
#endif
#endif
