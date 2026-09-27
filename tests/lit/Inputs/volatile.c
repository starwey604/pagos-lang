#include <stdint.h>
#include <string.h>

extern uint32_t sequence(uint32_t*);
extern uint32_t discarded(const uint32_t*);
extern uint32_t inline_effect(uint32_t*);
extern uint32_t loop(uint32_t*, uint32_t);
extern uint32_t ordered(uint32_t*);
extern uint32_t early(uint32_t*, uint32_t);
extern uint32_t load(const uint32_t*);
extern uint32_t store(uint32_t*);
extern uint32_t widths(uint8_t*, int16_t*, uint64_t*);

static uint32_t calls;
uint32_t* address(uint32_t* p) {
    ++calls;
    *p = 11;
    return p;
}
uint32_t value(uint32_t* p) {
    if (calls != 1 || *p != 11)
        return 999;
    ++calls;
    return 33;
}

int main(int argc, char** argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "null-store") == 0)
            return (int)store(0);
        if (strcmp(argv[1], "misaligned-read") == 0)
            return (int)load((const uint32_t*)(uintptr_t)1);
        if (strcmp(argv[1], "misaligned-store") == 0)
            return (int)store((uint32_t*)(uintptr_t)1);
        if (strcmp(argv[1], "discard-null") == 0)
            return (int)discarded(0);
        return (int)load(0);
    }
    uint32_t x = 0;
    uint8_t byte = 0;
    int16_t half = 0;
    uint64_t wide = 0;
    if (sequence(&x) != 7 || x != 3 || discarded(&x) != 7)
        return 1;
    if (inline_effect(&x) != 14 || x != 9 || loop(&x, 4) != 15)
        return 2;
    if (ordered(&x) != 33 || calls != 2 || x != 33)
        return 3;
    if (early(0, 0) != 77 || early(&x, 1) != 44 || x != 44)
        return 4;
    if (widths(&byte, &half, &wide) != 1 || byte != 255 || half != -1234 ||
        wide != UINT64_MAX)
        return 5;
    return 0;
}
