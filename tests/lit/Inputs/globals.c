#include <stdint.h>
extern uint32_t* counter_address(void);
extern uint32_t* configured_address(void);
extern const uint32_t* identity_address(void);
extern const uint32_t* twin_address(void);
extern uint32_t* peer_address(void);
extern uint32_t check_state(void);
extern uint32_t step(void);
extern uint32_t foreign_change(void);
extern uint32_t shadow(uint32_t);
extern uint32_t recurse(uint32_t);
uint32_t change(uint32_t* p) {
    *p = 20;
    return 0;
}
int main(void) {
    if (check_state() != 1 || *configured_address() != 41)
        return 1;
    if (*identity_address() != 17 || *twin_address() != 17 ||
        identity_address() == twin_address())
        return 2;
    if (step() != 1 || step() != 4 || *counter_address() != 2)
        return 3;
    *counter_address() = 8;
    if (shadow(2) != 10 || foreign_change() != 20 || recurse(3) != 23)
        return 4;
    if (peer_address() == counter_address() || *peer_address() != 99)
        return 5;
    return 0;
}
