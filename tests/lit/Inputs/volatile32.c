#include "host_signals.h"
#include <stdint.h>
extern uint32_t sequence(uint32_t*, uint8_t*, int16_t*);
extern uint32_t discarded(const uint32_t*);
extern uint32_t early(uint32_t*, uint32_t);
int abi_check(void) {
    uint32_t word = 0;
    uint8_t byte = 0;
    int16_t half = 0;
    if (sequence(&word, &byte, &half) != 7 || word != 3 || byte != 255 ||
        half != -1234)
        return 1;
    if (discarded(&word) != 7 || early(0, 0) != 77 || early(&word, 1) != 44 ||
        word != 44)
        return 2;
    return 0;
}
#ifndef PAGOS_FREESTANDING
int main(int argc, char** argv) {
    pagos_test_disable_core_dumps();
    (void)argv;
    if (argc > 1)
        return (int)discarded(0);
    return abi_check();
}
#endif
