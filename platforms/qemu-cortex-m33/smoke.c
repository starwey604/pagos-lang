#include <stddef.h>
#include <stdint.h>

_Static_assert(sizeof(void*) == 4 && sizeof(uint32_t) == 4,
               "AAPCS32 ILP32 required");
static volatile uint32_t initialized = 0x12345678;
static volatile uint32_t zeroed;
static volatile uint64_t numerator = 0x123456789abcdef0ULL;
static volatile uint64_t denominator = 37;

void platform_finish(uint32_t status) {
    const uint32_t arguments[2] = {0x20026, status};
    register uint32_t operation __asm__("r0") = 0x20;
    register const uint32_t* parameter __asm__("r1") = arguments;
    __asm__ volatile("bkpt 0xab" : "+r"(operation) : "r"(parameter) : "memory");
    for (;;) {
    }
}

static void uart_puts(const char* text) {
    volatile uint32_t* const uart = (volatile uint32_t*)0x40200000;
    // CMSDK APB UART BAUDDIV; emulator baud is not a physical timing test.
    uart[4] = 16;
    uart[2] = 1; // TX enable, polling only.
    while (*text) {
        uint32_t attempts = 100000;
        while (uart[1] & 1) {
            if (!--attempts)
                platform_finish(4);
        }
        uart[0] = (uint8_t)*text++;
    }
}

void fault_main(void) {
    uart_puts("PAGOS ARM FAULT\n");
    platform_finish(3);
}
// The ARM libgcc EHABI personality references these freestanding hooks even
// though Pagos has no exception surface. Unwinder aborts are unexpected faults.
void abort(void) {
    fault_main();
    for (;;) {
    }
}
void* memcpy(void* destination, const void* source, size_t count) {
    uint8_t* out = destination;
    const uint8_t* in = source;
    for (size_t i = 0; i < count; ++i)
        out[i] = in[i];
    return destination;
}
void trap_main(uint32_t pc) {
#ifdef EXPECT_TRAP
    const uint32_t cfsr = *(volatile uint32_t*)0xe000ed28;
    if ((cfsr & 0x10000) && *(const uint16_t*)pc == 0xdefe) {
        uart_puts("PAGOS ARM TRAP\n");
        platform_finish(2);
    }
#else
    (void)pc;
#endif
    fault_main();
}

#ifdef WITH_PAGOS_C_ABI
extern int abi_check(void);
#endif
void smoke_main(void) {
#ifdef FORCE_TIMEOUT
    uart_puts("PAGOS ARM TIMEOUT\n");
    for (;;) {
    }
#endif
#ifdef FORCE_UNDEFINED
    // Must be reported as a fault, not a Pagos trap.
    __asm__ volatile("udf #1");
#endif
    _Alignas(8) volatile uint32_t stack_probe[4] = {1, 2, 3, 4};
    uint32_t passed = initialized == 0x12345678 && zeroed == 0 &&
                      ((uintptr_t)&stack_probe & 7) == 0 &&
                      stack_probe[3] == 4 &&
                      numerator / denominator == 0x7df47fccd4ac14ULL;
#ifdef WITH_PAGOS_C_ABI
    passed = passed && abi_check() == 0;
#endif
#ifdef FORCE_FAILURE
    passed = 0;
#endif
    uart_puts(passed ? "PAGOS ARM PASS\n" : "PAGOS ARM FAIL\n");
    platform_finish(passed ? 0 : 1);
}
