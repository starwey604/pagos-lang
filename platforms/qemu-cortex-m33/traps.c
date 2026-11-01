#include <stdint.h>
extern uint32_t null_read(const uint32_t*);
extern uint32_t null_store(uint32_t*);
extern uint32_t unaligned_volatile(const uint32_t*);
extern uint32_t discarded(const uint32_t*);
extern uint32_t shift(uint32_t, uint32_t);
extern uint32_t divide(uint32_t, uint32_t);
extern uint32_t lookup(uint32_t);
int abi_check(void) {
#if TRAP_CASE == 1
    return (int)null_read(0);
#elif TRAP_CASE == 2
    return (int)null_store(0);
#elif TRAP_CASE == 3
    return (int)unaligned_volatile((const uint32_t*)(uintptr_t)1);
#elif TRAP_CASE == 4
    return (int)discarded(0);
#elif TRAP_CASE == 5
    return (int)shift(1, 32);
#elif TRAP_CASE == 6
    return (int)divide(1, 0);
#elif TRAP_CASE == 7
    return (int)lookup(3);
#endif
}
