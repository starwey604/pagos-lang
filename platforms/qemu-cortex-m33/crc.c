#include <stdint.h>
extern uint32_t generic_crc(const uint8_t*, uint32_t, uint32_t);
extern uint32_t frozen_crc(const uint8_t*, uint32_t);
static uint32_t reference(const uint8_t* data, uint32_t length,
                          uint32_t state) {
    for (uint32_t i = 0; i < length; ++i) {
        state ^= data[i];
        for (uint32_t bit = 0; bit < 8; ++bit)
            state = (state >> 1) ^ ((0U - (state & 1U)) & 0xedb88320U);
    }
    return state ^ 0xffffffffU;
}
int abi_check(void) {
    uint8_t data[1024];
    static const uint32_t lengths[] = {0, 1, 16, 64, 256, 1024};
    for (uint32_t pattern = 0; pattern < 3; ++pattern) {
        for (uint32_t i = 0; i < 1024; ++i)
            data[i] = pattern == 0 ? 0 : pattern == 1 ? 255 : (uint8_t)i;
        for (uint32_t n = 0; n < sizeof(lengths) / sizeof(lengths[0]); ++n) {
            uint32_t length = lengths[n];
            if (frozen_crc(data, length) !=
                    reference(data, length, 0xffffffffU) ||
                generic_crc(data, length, 0x12345678) !=
                    reference(data, length, 0x12345678))
                return 1;
        }
    }
    return frozen_crc((const uint8_t*)"123456789", 9) != 0xcbf43926U;
}
