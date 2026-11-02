#include <stddef.h>
#include <stdint.h>

struct State {
    uint8_t tag;
    uint32_t count;
    uint64_t wide;
    uintptr_t size;
    int16_t tail;
};
_Static_assert(sizeof(uintptr_t) == 8, "x64 pointer width");
_Static_assert(sizeof(struct State) == 32, "target record size");
_Static_assert(offsetof(struct State, count) == 4, "count offset");
_Static_assert(offsetof(struct State, wide) == 8, "wide offset");
_Static_assert(offsetof(struct State, size) == 16,
               "pointer-sized field offset");
_Static_assert(offsetof(struct State, tail) == 24, "tail offset");

extern struct State* storage_state(void);
extern const struct State* storage_zero(void);
extern uint32_t* storage_word(uint32_t);
extern uint32_t storage_write(uint32_t, uint32_t);
extern struct State* storage_roundtrip(struct State*, uint32_t*);

static uint32_t calls;
struct State* c_touch(uint32_t a, struct State* p, uint32_t b, uint32_t* q,
                      uint32_t c, struct State* r, uint32_t d) {
    if (a != 11 || b != 22 || c != 33 || d != 44 || r != storage_state())
        return 0;
    ++calls;
    *q = a + b;
    r->count = c + d;
    p->wide = UINT64_MAX;
    p->size = (uintptr_t)p;
    return p;
}

int main(void) {
    struct State* state = storage_state();
    const struct State* zero = storage_zero();
    struct State local = {1, 7, 0, 0, -3};
    uint32_t value = 0;
    if ((uintptr_t)state % 64 || state->tag != 3 || state->count != 12 ||
        state->wide != UINT64_MAX || state->size != UINTPTR_MAX ||
        state->tail != -7)
        return 1;
    if (zero->tag || zero->count || zero->wide || zero->size || zero->tail)
        return 2;
    if (*storage_word(7) || storage_write(7, 100) != 149 ||
        *storage_word(7) != 100)
        return 3;
    if (storage_roundtrip(&local, &value) != &local || calls != 1 ||
        value != 33 || state->count != 77 || local.count != 8 ||
        local.wide != UINT64_MAX || local.size != (uintptr_t)&local ||
        local.tag != 1 || local.tail != -3)
        return 4;
    return 0;
}
