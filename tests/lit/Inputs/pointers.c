#include <stdint.h>

extern uint32_t api_alias(uint32_t*, const uint32_t*);
extern uint32_t api_foreign(void);
extern uint32_t api_inline(uint32_t*);
extern uint32_t api_loop(uint32_t*, uint32_t);
extern const uint32_t* api_roundtrip(uint32_t*);
extern uint32_t* api_choose(uint32_t*, uint32_t*, uint32_t);
extern uint32_t api_small(uint8_t*, int16_t*);
extern uint32_t api_wide(uint64_t*, int64_t*, uintptr_t*);
extern uint32_t api_unaligned(uint8_t*);
extern uint32_t api_return(uint32_t*, uint32_t);
extern uint32_t api_null_read(const uint32_t*);
extern uint32_t api_null_store(uint32_t*);
extern uint32_t api_discard_read(const uint32_t*);

static uint32_t backing;
static uint32_t calls;
uint32_t* c_pointer(void) {
    ++calls;
    return &backing;
}
uint32_t c_change(uint32_t* p) {
    *p = 21;
    return 0;
}

int abi_check(void) {
    uint32_t x = 2, y = 5;
    uint8_t byte = 0;
    int16_t small = 0;
    uint64_t wide = 0;
    int64_t signed_wide = 0;
    uintptr_t size = 0;
    unsigned char bytes[9] = {0};
    if (api_alias(&x, &x) != 13 || x != 9)
        return 1;
    if (api_alias(&x, &y) != 15 || x != 9 || y != 5)
        return 2;
    if (api_foreign() != 24 || backing != 21 || calls != 1)
        return 3;
    if (api_loop(&x, 4) != 18 || x != 18)
        return 4;
    if (api_roundtrip(&x) != &x || api_choose(&x, &y, 0) != &x ||
        api_choose(&x, &y, 1) != &y)
        return 5;
    if (api_small(&byte, &small) != 1489 || byte != 255 || small != -1234)
        return 6;
    if (api_wide(&wide, &signed_wide, &size) != 1 || wide != UINT64_MAX ||
        signed_wide != INT64_MIN || size != 17)
        return 7;
    // Only Pagos constructs the unaligned u32 pointer; C passes a byte buffer.
    // Both validated targets are little-endian.
    if (api_unaligned(bytes) != 0x12345678 || bytes[0] != 0 ||
        bytes[1] != 0x78 || bytes[4] != 0x12 || bytes[5] != 0)
        return 8;
    if (api_return(&x, 0) != 77 || x != 18 || api_return(&x, 1) != 44 ||
        x != 44)
        return 9;
    // The RHS returns before the store; no null access actually occurs.
    if (api_return(0, 0) != 77)
        return 10;
    if (api_inline(&x) != 15 || x != 1)
        return 11;
    return 0;
}

#ifndef PAGOS_FREESTANDING
int main(int argc, char** argv) {
    if (argc > 1) {
        if (argv[1][0] == 's')
            return (int)api_null_store(0);
        if (argv[1][0] == 'd')
            return (int)api_discard_read(0);
        return (int)api_null_read(0);
    }
    return abi_check();
}
#endif
