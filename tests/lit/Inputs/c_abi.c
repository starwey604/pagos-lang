#include <stdint.h>

extern uint32_t api_sum(uint32_t);
extern int32_t api_neg(int32_t);
extern uint32_t api_mix(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                        uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
extern uint32_t api_effects(void);
extern uint32_t api_constant(void);
extern uint32_t api_even(uint32_t);
extern uint32_t api_odd(uint32_t);
extern uint32_t api_discard(void);
extern uint32_t api_arg_return(uint32_t);
extern uint32_t api_trap(uint32_t);

static uint32_t calls;
uint32_t c_next(void) { return ++calls; }
int32_t c_neg(int32_t value) { return -value; }
uint32_t c_identity(uint32_t value) { return value; }
uint32_t c_mix(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e,
               uint32_t f, uint32_t g, uint32_t h, uint32_t i, uint32_t j) {
    return a + 3 * b + 5 * c + 7 * d + 11 * e + 13 * f + 17 * g + 19 * h +
           23 * i + 29 * j;
}

int abi_check(void) {
    uint32_t expected = c_mix(0xffffffffU, 2, 3, 4, 5, 6, 7, 8, 9, 0x80000000U);
    return api_sum(20) == 210 && api_neg(-1234567) == 1234566 &&
                   api_neg(17) == -18 && api_constant() == 42 &&
                   api_mix(0xffffffffU, 2, 3, 4, 5, 6, 7, 8, 9, 0x80000000U) ==
                       (expected ^ 0xa5a5a5a5U) &&
                   api_effects() == 4 && calls == 2 && api_even(12) == 1 &&
                   api_odd(9) == 1 && api_discard() == 7 && calls == 3 &&
                   api_arg_return(1) == 99 && calls == 3 &&
                   api_arg_return(0) == 4 && calls == 4
               ? 0
               : 1;
}

#ifndef PAGOS_FREESTANDING
int main(int argc, char** argv) {
    (void)argv;
    if (argc > 1)
        return (int)api_trap(0);
    return abi_check();
}
#endif
