#include <stddef.h>
#include <stdint.h>
#ifndef PAGOS_FREESTANDING
#include <stdlib.h>
#include <string.h>
#endif
struct State {
    uint8_t tag;
    uint32_t count;
    uint64_t wide;
    uintptr_t size;
};
extern struct State* state_address(void);
extern const struct State* config_address(void);
extern uint32_t* count_address(struct State*);
extern const uint8_t* tag_address(const struct State*);
extern uint32_t read(const struct State*);
extern uint32_t read_bytes(const uint8_t*);
extern uint32_t bump(struct State*);
extern uint32_t recurse(struct State*, uint32_t);
extern uint32_t snapshot(struct State*);
extern uint32_t foreign_read(void);
extern struct State* choose(struct State*, struct State*, uint32_t);
extern uint32_t conversions(struct State*);
extern uint32_t order(void);
extern uint32_t early(uint32_t);
extern uint32_t always_return(void);
extern uint32_t late_return(struct State*);
extern uint32_t discard(const struct State*);
extern uint32_t store(struct State*);
extern uint32_t volatile_read(const struct State*);
extern uint32_t volatile_bytes(const uint8_t*);
static unsigned calls;
static struct State shared = {5, 6, 7, 8};
uint32_t change(struct State* p) {
    p->count = 40;
    return 0;
}
struct State* foreign(void) { return &shared; }
struct State* next(void) {
    ++calls;
    return state_address();
}
uint32_t payload(void) {
#ifndef PAGOS_FREESTANDING
    if (calls == 99)
        _Exit(99); // A null field address must trap before the RHS.
#endif
    return ++calls == 2 ? 42 : 19;
}
int abi_check(void) {
    struct State local = {1, 10, UINT64_MAX, 16};
    struct State* p = state_address();
    if (p->tag != 3 || p->count != 12 || p->wide != UINT64_MAX ||
        p->size != 16 || config_address()->tag != 7 ||
        read(config_address()) != 99)
        return 1;
    if (count_address(&local) != &local.count ||
        tag_address(&local) != &local.tag || bump(&local) != 11 ||
        recurse(&local, 3) != 14 || snapshot(&local) != 68 ||
        local.count != 40 || foreign_read() != 6)
        return 2;
    if (choose(&local, p, 0) != &local || choose(&local, p, 1) != p ||
        conversions(&local) != 16 || conversions(0) != 0)
        return 3;
    if (order() != 42 || calls != 2 || early(0) != 77 || calls != 2 ||
        always_return() != 55 || late_return(p) != 88 || p->count != 42 ||
        volatile_read(p) != 42 || discard(p) != 0)
        return 4;
#ifndef PAGOS_FREESTANDING
    // Deliberately unaligned, accessed only by Pagos's align-1 memory path.
    _Alignas(struct State) unsigned char bytes[sizeof(struct State) + 1];
    memcpy(bytes + 1, &local, sizeof local);
    if (read_bytes(bytes + 1) != 40)
        return 5;
#endif
    return 0;
}
#ifndef PAGOS_FREESTANDING
int main(int argc, char** argv) {
    if (argc > 1) {
        switch (argv[1][0]) {
        case 'r':
            return (int)read(0);
        case 'f':
            return tag_address(0) != 0;
        case 'd':
            return (int)discard(0);
        case 's':
            calls = 99;
            return (int)store(0);
        case 'l':
            return (int)late_return(0);
        case 'a': {
            _Alignas(struct State)
                uint8_t bytes[sizeof(struct State) + 1] = {0};
            return (int)volatile_bytes(bytes + 1);
        }
        default:
            return (int)volatile_read(0);
        }
    }
    return abi_check();
}
#endif
