#ifndef LIVEWDF_GP50_MICROAMP_H
#define LIVEWDF_GP50_MICROAMP_H

#if defined(__arm__) && !defined(GP5_HOST)
#define MICROAMP_TABLE __attribute__((section(".data.microamp")))
#else
#define MICROAMP_TABLE
#endif
#include "microamp_coefficients.h"
#undef MICROAMP_TABLE

static float microamp_capacitors[MICROAMP_STATES];
static float microamp_pole;

static void microamp_reset(void)
{
    for (unsigned i = 0; i < MICROAMP_STATES; ++i) microamp_capacitors[i] = 0.0f;
    microamp_pole = 0.0f;
}

static inline float microamp_transfer(const float *row, float input, float pole)
{
    float result = row[0] + row[MICROAMP_STATES + 1] * input + row[MICROAMP_STATES + 2] * pole;
#if defined(__GNUC__)
#pragma GCC unroll 5
#endif
    for (unsigned i = 0; i < MICROAMP_STATES; ++i)
        result += row[i + 1] * microamp_capacitors[i];
    return result;
}

/* Input and output are volts. Gain is a slot in [0, MICROAMP_GAIN_COUNT).
 * The level multiplier is applied after the circuit, with 1.0 for unity. */
GP5_FAST static float microamp_step(float input, unsigned gain, float level_scale)
{
    const struct MicroampModel *model = &microamp_models[gain];
    float pole = microamp_transfer(model->pole, input, microamp_pole);
    float lo = microamp_pole - MICROAMP_SLEW, hi = microamp_pole + MICROAMP_SLEW;
    if (lo < MICROAMP_RAIL_LOW) lo = MICROAMP_RAIL_LOW;
    if (hi > MICROAMP_RAIL_HIGH) hi = MICROAMP_RAIL_HIGH;
    if (pole < lo) pole = lo;
    if (pole > hi) pole = hi;

    float output = microamp_transfer(model->output, input, pole);
    float next[MICROAMP_STATES];
    for (unsigned i = 0; i < MICROAMP_STATES; ++i)
        next[i] = microamp_transfer(model->next[i], input, pole);
    for (unsigned i = 0; i < MICROAMP_STATES; ++i)
        microamp_capacitors[i] = next[i];
    microamp_pole = pole;
    return output * level_scale;
}

#endif
