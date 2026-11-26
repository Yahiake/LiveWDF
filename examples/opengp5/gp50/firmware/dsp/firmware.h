#include "effects.h"

#ifdef LIVEWDF_GP50_CATALOG
void livewdf_catalog_init(unsigned rate);
void livewdf_catalog_block(unsigned model, unsigned level, unsigned frames, const int32_t *controls,
                           const int32_t *input, int32_t *output);
#else
#include "wdf_solver.h"
#include "microamp.h"

#define VOLTS_PER_FULL_SCALE 1.2f

static unsigned active_model;
static float previous_output;
#endif

static void fw_init(void)
{
#ifdef LIVEWDF_GP50_CATALOG
    livewdf_catalog_init(GP5_SAMPLE_RATE);
#else
    wdf_reset();
    microamp_reset();
#endif
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
    unsigned level = (unsigned)gp5_param(GP5_P_LEVEL);
#ifdef LIVEWDF_GP50_CATALOG
    livewdf_catalog_block(model, level, GP5_BLOCK, &gp5_param_values[GP5_P_POT_0], in, out_left);
#else
    unsigned gain = (unsigned)gp5_param(GP5_P_GAIN);
    int changed = model != active_model;
    float previous = previous_output;
    if (changed) {
        if (model == MODEL_MICROAMP) microamp_reset();
        else wdf_reset();
        active_model = model;
    }

    float level_scale = model == MODEL_MICROAMP ? level / 100.0f : level_scales[level];
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
#endif
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
