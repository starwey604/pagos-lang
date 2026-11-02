#include "shared_checks.h"

#ifdef _WIN32
#define SHARED_IMPORT __declspec(dllimport)
#else
#define SHARED_IMPORT
#endif
SHARED_IMPORT uint32_t shared_apply(uint32_t, uint32_t*, uint32_t,
                                    const uint32_t*, uint32_t, uint32_t);
SHARED_IMPORT uint32_t* shared_state(void);
SHARED_IMPORT uint32_t shared_calls(void);
SHARED_IMPORT uint32_t shared_pid(void);
SHARED_IMPORT int32_t shared_signed(int32_t);
SHARED_IMPORT uint32_t shared_trap(const uint32_t*);

int main(int argc, char** argv) {
    struct SharedApi api = {shared_apply, shared_state,  shared_calls,
                            shared_pid,   shared_signed, shared_trap};
    pagos_test_disable_core_dumps();
    (void)argv;
    if (argc > 1)
        return (int)api.trap(0);
    return shared_check(api);
}
