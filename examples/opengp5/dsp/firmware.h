#include "circuit.h"

static LiveWdfCircuit circuit;
static float mix;

static void fw_init(void)
{
    livewdf_reset(&circuit);
    mix = 0.0f;
}

static void fw_param(uint32_t id, int32_t value)
{
    (void)id;
    (void)value;
}

static void fw_process(const int32_t *in, int32_t *left, int32_t *right)
{
    const float target = gp5_param(GP5_P_ON) ? 0.01f * gp5_param(GP5_P_MIX) : 0.0f;
    for (unsigned i = 0; i < GP5_BLOCK; ++i) {
        const float dry = (float)in[i] / GP5_FULL_SCALE;
        const float wet = livewdf_step(&circuit, dry);
        const float delta = target - mix;
        mix = delta > -0.00001f && delta < 0.00001f ? target : mix + 0.005f * delta;
        const float output = (dry + mix * (wet - dry)) * GP5_FULL_SCALE;
        int32_t sample = in[i];
        if (mix != 0.0f) {
            if (output >= GP5_FULL_SCALE) sample = INT32_MAX;
            else if (output <= -GP5_FULL_SCALE) sample = INT32_MIN;
            else sample = (int32_t)output;
        }
        left[i] = right[i] = sample;
    }
}

static int fw_command(uint32_t cmd, const uint8_t *payload, uint32_t size)
{
    (void)cmd;
    (void)payload;
    (void)size;
    return 0;
}
