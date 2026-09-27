#include <stdint.h>
extern uint8_t* buffer_address(uint32_t);
extern const uint32_t* square_address(uint32_t);
extern const uint32_t* twin_address(void);
extern uint32_t* word_address(uint32_t);
extern int16_t* narrow_address(uint32_t);
extern uint64_t* wide_address(uint32_t);
extern uintptr_t* size_address(uint32_t);
extern uint32_t lookup(uint32_t);
extern uint32_t snapshot(void);
extern uint32_t foreign_change(void);
extern uint32_t repeat(void);
extern uint32_t fill(uint32_t);
extern uint32_t recurse(uint32_t);
extern uint32_t order(void);
extern uint32_t early(uint32_t);
extern uint32_t always_return(void);
extern uint32_t shadow(void);
extern uint32_t trap_read(uint32_t);
extern uint32_t trap_store(uint32_t);
extern uint32_t trap_discard(uint32_t);
static unsigned calls;
uint32_t next(void) {
    ++calls;
    return 1;
}
uint32_t payload(void) { return calls++ == 1 ? 77 : 0; }
uint32_t change(uint8_t* p) {
    *p = 20;
    return 0;
}

int abi_check(void) {
    uint8_t* data = buffer_address(0);
    if ((uintptr_t)data % 64 || (uintptr_t)square_address(0) % 32 ||
        twin_address() == square_address(0))
        return 1;
    for (unsigned i = 0; i < 16; ++i)
        if (data[i] != 0 || buffer_address(i) != data + i ||
            lookup(i) != i * i || square_address(i) != square_address(0) + i)
            return 2;
    for (unsigned i = 0; i < 4; ++i)
        if (*word_address(i) != i + 1 || word_address(i) != word_address(0) + i)
            return 3;
    if (*narrow_address(0) != -1234 || *narrow_address(1) != 32767 ||
        narrow_address(1) != narrow_address(0) + 1 ||
        *wide_address(0) != UINT64_MAX || *wide_address(1) != 1 ||
        wide_address(1) != wide_address(0) + 1 ||
        *size_address(0) != UINTPTR_MAX || *size_address(1) != 7 ||
        size_address(1) != size_address(0) + 1)
        return 4;
    data[0] = 3;
    if (snapshot() != 15 || data[0] != 9 || foreign_change() != 20)
        return 5;
    if (repeat() != 9 || data[0] != 7 || fill(16) != 15 || recurse(3) != 3)
        return 6;
    if (order() != 77 || calls != 2 || *word_address(1) != 77)
        return 7;
    if (early(0) != 77 || data[0] != 3 || early(1) != 5 || data[0] != 5 ||
        always_return() != 55 || shadow() != 2)
        return 8;
    return 0;
}

#ifndef PAGOS_FREESTANDING
int main(int argc, char** argv) {
    if (argc > 1) {
        switch (argv[1][0]) {
        case 'r':
            return (int)trap_read(16);
        case 's':
            return (int)trap_store(UINT32_MAX);
        case 'a':
            return buffer_address(16) != 0;
        default:
            return (int)trap_discard(UINT32_MAX);
        }
    }
    return abi_check();
}
#endif
