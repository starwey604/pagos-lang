#include "shared_checks.h"
#include <string.h>

int main(int argc, char** argv) {
    pagos_test_disable_core_dumps();
    for (unsigned round = 0; round < 2; ++round) {
        SharedHandle library = shared_host_load();
        if (!library)
            return 31;
        if (argc > 1 && strcmp(argv[1], "missing-symbol") == 0) {
            int result = shared_host_symbol(library, "absent_export") ? 1 : 32;
            return shared_host_close(library) ? 33 : result;
        }
        struct SharedApi api = {
            (SharedApply)shared_host_symbol(library, "shared_apply"),
            (SharedState)shared_host_symbol(library, "shared_state"),
            (SharedCounter)shared_host_symbol(library, "shared_calls"),
            (SharedCounter)shared_host_symbol(library, "shared_pid"),
            (SharedSigned)shared_host_symbol(library, "shared_signed"),
            (SharedTrap)shared_host_symbol(library, "shared_trap")};
        if (argc > 1 && strcmp(argv[1], "trap") == 0)
            return (int)api.trap(0);
        int result = shared_check(api);
        if (shared_host_close(library))
            return 33;
        if (result)
            return result;
    }
    return 0;
}
