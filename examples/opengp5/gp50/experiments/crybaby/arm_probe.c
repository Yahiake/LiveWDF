#include <stdint.h>
#define GP5_FAST __attribute__((section(".data_fast"), noinline))
#include "crybaby.h"
static struct CryBabyState state;
int wah_block(const int32_t* in, int32_t* out, unsigned position)
{
    unsigned rejected = 0;
    for (unsigned i = 0; i < 64; ++i) {
        float y;
        rejected += crybaby_step(&state, (float)in[i] * (CRYBABY_INPUT_VOLTS / 2147483648.0f), position, &y) == 0;
        float output = y / CRYBABY_OUTPUT_VOLTS;
        if (output >= 1.0f) out[i] = INT32_MAX;
        else if (output <= -1.0f) out[i] = INT32_MIN;
        else out[i] = (int32_t)(output * 2147483648.0f);
    }
    return rejected;
}
