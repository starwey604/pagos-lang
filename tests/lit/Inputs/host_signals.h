#ifndef PAGOS_TEST_HOST_SIGNALS_H
#define PAGOS_TEST_HOST_SIGNALS_H

#if defined(__linux__) && !defined(PAGOS_FREESTANDING)
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#endif

#if defined(_WIN32) && !defined(PAGOS_FREESTANDING)
#include <windows.h>
#endif

static inline void pagos_test_disable_core_dumps(void) {
#if defined(_WIN32) && !defined(PAGOS_FREESTANDING)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
#if defined(__linux__) && !defined(PAGOS_FREESTANDING)
    // Trap tests assert the real terminating signal. Kernel pipe-based core
    // handlers ignore RLIMIT_CORE and can block even with a zero size limit.
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
        perror("disable test-process core dumps");
        exit(125);
    }
#endif
}
#endif
