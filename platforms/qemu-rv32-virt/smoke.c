// Platform-only environment probe, not a Pagos runtime or language feature.
typedef unsigned int u32;
typedef unsigned long long u64;
_Static_assert(sizeof(u32) == 4 && sizeof(void*) == 4, "RV32 ILP32 required");

static volatile u32 initialized = 0x12345678;
static volatile u32 zeroed;
static volatile u64 numerator = 0x123456789abcdef0ULL;
static volatile u64 denominator = 37;

static void uart_puts(const char* text) {
    volatile unsigned char* const uart = (volatile unsigned char*)0x10000000;
    while (*text) {
        while (!(uart[5] & 0x20)) {
        }
        uart[0] = (unsigned char)*text++;
    }
}

void smoke_main(void) {
#ifdef FORCE_TIMEOUT
    for (;;) {
    }
#endif
    volatile u32 stack_probe[4] = {1, 2, 3, 4};
    // Both compiler-generated division and the target libgcc implementation
    // must execute correctly, not merely link successfully.
    u32 passed = initialized == 0x12345678 && zeroed == 0 &&
                 ((u32)&stack_probe & 3) == 0 && stack_probe[3] == 4 &&
                 numerator / denominator == 0x7df47fccd4ac14ULL;
#ifdef FORCE_FAILURE
    passed = 0;
#endif
    uart_puts(passed ? "PAGOS RV32 PASS\n" : "PAGOS RV32 FAIL\n");
    *(volatile u32*)0x100000 = passed ? 0x5555 : 0x13333;
    for (;;) {
    }
}
