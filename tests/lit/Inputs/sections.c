#include <stdint.h>

extern const uint32_t* rom_address(void);
extern const uint32_t* twin_address(void);
extern uint32_t* seed_address(void);
extern uint32_t* count_address(void);
extern uint32_t* zero_address(void);
extern uint64_t* natural_address(void);
extern uint8_t* page_address(void);
extern uint32_t step(void);

int main(void) {
    if ((uintptr_t)rom_address() % 32 || (uintptr_t)twin_address() % 64 ||
        (uintptr_t)seed_address() % 64 || (uintptr_t)count_address() % 128 ||
        (uintptr_t)zero_address() % 16 || (uintptr_t)natural_address() % 8 ||
        (uintptr_t)page_address() % 4096)
        return 1;
    if (*rom_address() != 17 || *twin_address() != 17 ||
        rom_address() == twin_address() || *seed_address() != 37 ||
        *count_address() != 0 || *zero_address() != 0 ||
        *natural_address() != UINT64_MAX || *page_address() != 0)
        return 2;
    if (step() != 38 || step() != 39)
        return 3;
    *seed_address() = 90;
    return step() == 93 ? 0 : 4;
}
