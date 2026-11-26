#include "wdf_solver.h"
#include "microamp.h"
#include "effects.h"

#define VOLTS_PER_FULL_SCALE 1.2f

static unsigned active_model;
static float previous_output;

static void fw_init(void)
{
    wdf_reset();
    microamp_reset();
    effects_reset();
}

static void fw_param(uint32_t id, int32_t value)
{
    (void)id;
    (void)value;
}

static void fw_process(const int32_t *in, int32_t *out_left, int32_t *out_right)
{
    unsigned model = (unsigned)gp5_param(GP5_P_MODEL);
    unsigned gain = (unsigned)gp5_param(GP5_P_GAIN);
    unsigned level = (unsigned)gp5_param(GP5_P_LEVEL);
    int changed = model != active_model;
    float previous = previous_output;
    if (changed) {
        if (model == MODEL_MICROAMP) microamp_reset();
        else wdf_reset();
        active_model = model;
    }

    unsigned circuit_level = level > DOD250_LEVEL_MAX ? DOD250_LEVEL_MAX : level;
    float level_scale = model == MODEL_MICROAMP ? circuit_level / 100.0f : level_scales[circuit_level];
    if (level > DOD250_LEVEL_MAX) level_scale *= level / 100.0f;
    float input_volts = model == MODEL_MICROAMP ? MICROAMP_INPUT_VOLTS : VOLTS_PER_FULL_SCALE;
    float normalize = model == MODEL_MICROAMP ? 1.0f / MICROAMP_OUTPUT_VOLTS : 1.0f / VOLTS_PER_FULL_SCALE;
    for (unsigned i = 0; i < GP5_BLOCK; ++i) {
        float x = (float)in[i] * (input_volts / GP5_FULL_SCALE);
        float y = model == MODEL_MICROAMP ? microamp_step(x, gain, level_scale) : wdf_step(x, gain, level_scale);
        y *= normalize;
        if (changed) y = previous + (y - previous) * ((float)(i + 1) / GP5_BLOCK);
        if (y > 0.99999994f) y = 0.99999994f;
        if (y < -1.0f) y = -1.0f;
        previous_output = y;
        out_left[i] = (int32_t)(y * GP5_FULL_SCALE);
    }
    effects_block(out_left, out_left, out_right, (unsigned)gp5_param(GP5_P_CAB),
                  (unsigned)gp5_param(GP5_P_DELAY), (unsigned)gp5_param(GP5_P_REVERB));
    if (!gp5_param(GP5_P_ON))
        for (unsigned i = 0; i < GP5_BLOCK; ++i) out_left[i] = out_right[i] = in[i];
}

static int fw_command(uint32_t cmd, const uint8_t *payload, uint32_t n)
{
    (void)cmd;
    (void)payload;
    (void)n;
    return 0;
}
