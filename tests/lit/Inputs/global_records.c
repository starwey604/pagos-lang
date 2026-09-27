#include <stddef.h>
#include <stdint.h>

struct State {
    uint8_t tag;
    uint32_t count;
    uint64_t wide;
    uintptr_t size;
    int16_t tail;
};
extern uint8_t* tag_address(void);
extern uint32_t* count_address(void);
extern uint64_t* wide_address(void);
extern uintptr_t* size_address(void);
extern int16_t* tail_address(void);
extern uint8_t* zero_address(void);
extern const uint8_t* config_address(void);
extern const uint8_t* twin_address(void);
extern uint32_t startup(void);
extern uint32_t advance(void);
extern uint32_t snapshot(void);
extern uint32_t aliases(void);
extern uint32_t early(uint32_t);
extern uint32_t shadow(void);

uint32_t change(uint32_t* p) {
    *p = 40;
    return 0;
}

int abi_check(void) {
    const uintptr_t base = (uintptr_t)tag_address();
    if (startup() != 1 || base % 64 || (uintptr_t)zero_address() % 32 ||
        (uintptr_t)config_address() % _Alignof(struct State))
        return 1;
    if ((uintptr_t)count_address() - base != offsetof(struct State, count) ||
        (uintptr_t)wide_address() - base != offsetof(struct State, wide) ||
        (uintptr_t)size_address() - base != offsetof(struct State, size) ||
        (uintptr_t)tail_address() - base != offsetof(struct State, tail))
        return 2;
    if (sizeof(struct State) != (sizeof(uintptr_t) == 8 ? 32 : 24) ||
        *tag_address() != 3 || *count_address() != 12 ||
        *wide_address() != UINT64_MAX || *size_address() != UINTPTR_MAX ||
        *tail_address() != -7 || *config_address() != 3 ||
        config_address() == twin_address())
        return 3;
    if (advance() != 13 || snapshot() != 66 || *count_address() != 40 ||
        aliases() != 21 || early(0) != 77 || *count_address() != 21 ||
        early(1) != 30 || shadow() != 99)
        return 4;
    *size_address() = 7;
    *tail_address() = -123;
    return *size_address() != 7 || *tail_address() != -123;
}

#ifndef PAGOS_FREESTANDING
int main(void) { return abi_check(); }
#endif
