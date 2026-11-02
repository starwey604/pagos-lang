#include "shared_host.h"

#ifdef _WIN32
#define SHARED_API __declspec(dllexport)
#else
#define SHARED_API __attribute__((visibility("default")))
#endif

static uint32_t calls;
SHARED_API uint32_t peer_mix(uint32_t a, uint32_t* p, uint32_t c,
                             const uint32_t* q, uint32_t e, uint32_t f) {
    ++calls;
    *p += e;
    return a + 2 * c + 3 * e + 5 * f + *q;
}
SHARED_API uint32_t peer_calls(void) { return calls; }
SHARED_API uint32_t peer_pid(void) { return shared_host_pid(); }
