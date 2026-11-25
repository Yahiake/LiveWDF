#include "wdf_solver.h"

#define VOLTS_PER_FULL_SCALE 1.2f

static void fw_init(void)
{
    wdf_reset();
}

static void fw_param(uint32_t id, int32_t value)
{
    (void)id;
    (void)value;
}

static void fw_process(const int32_t *in, int32_t *out_left, int32_t *out_right)
{
    unsigned gain = (unsigned)gp5_param(GP5_P_GAIN);
    float level_scale = level_scales[gp5_param(GP5_P_LEVEL)];
    int on = gp5_param(GP5_P_ON);
    for (unsigned i = 0; i < GP5_BLOCK; ++i) {
        float x = (float)in[i] * (VOLTS_PER_FULL_SCALE / GP5_FULL_SCALE);
        float y = wdf_step(x, gain, level_scale) / VOLTS_PER_FULL_SCALE;
        if (y > 0.99999994f) y = 0.99999994f;
        if (y < -1.0f) y = -1.0f;
        out_left[i] = out_right[i] = on ? (int32_t)(y * GP5_FULL_SCALE) : in[i];
    }
}

static int fw_command(uint32_t cmd, const uint8_t *payload, uint32_t n)
{
    (void)cmd;
    (void)payload;
    (void)n;
    return 0;
}
