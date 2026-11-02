#ifndef PAGOS_TEST_SHARED_CHECKS_H
#define PAGOS_TEST_SHARED_CHECKS_H

#include "shared_host.h"

typedef uint32_t (*SharedApply)(uint32_t, uint32_t*, uint32_t, const uint32_t*,
                                uint32_t, uint32_t);
typedef uint32_t* (*SharedState)(void);
typedef uint32_t (*SharedCounter)(void);
typedef int32_t (*SharedSigned)(int32_t);
typedef uint32_t (*SharedTrap)(const uint32_t*);
struct SharedApi {
    SharedApply apply;
    SharedState state;
    SharedCounter calls;
    SharedCounter pid;
    SharedSigned signed_value;
    SharedTrap trap;
};

static int shared_check(struct SharedApi api) {
    uint32_t word = 5;
    const uint32_t other = 7;
    if (!api.apply || !api.state || !api.calls || !api.pid ||
        !api.signed_value || !api.trap)
        return 1;
    if (*api.state() != 17 || api.calls() != 0 ||
        api.pid() != shared_host_pid() || api.signed_value(-7) != -8)
        return 2;
    if (api.apply(3, &word, 11, &other, 13, 17) != 173 || word != 18 ||
        other != 7 || api.calls() != 1)
        return 3;
    *api.state() = 100;
    if (api.apply(3, &word, 11, &other, 13, 17) != 256 || word != 31 ||
        api.calls() != 2)
        return 4;
    return 0;
}
#endif
