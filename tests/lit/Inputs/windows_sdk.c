#include "host_signals.h"
#include <stdint.h>

extern uint32_t windows_process_id(void);
int main(void) {
    return windows_process_id() != (uint32_t)GetCurrentProcessId();
}
