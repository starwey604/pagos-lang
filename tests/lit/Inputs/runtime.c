#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int next_input = 1;
static int input_count;
static char** inputs;

uint32_t pagos_external_input(void) {
    if (next_input >= input_count) {
        fputs("unexpected input read\n", stderr);
        exit(2);
    }
    uint32_t value = (uint32_t)strtoul(inputs[next_input++], NULL, 10);
    printf("input=%u\n", (unsigned)value);
    fflush(stdout);
    return value;
}

extern uint32_t pagos_main(void);

int main(int argc, char** argv) {
    input_count = argc;
    inputs = argv;
    uint32_t result = pagos_main();
    printf("result=%u\n", (unsigned)result);
    return 0;
}
