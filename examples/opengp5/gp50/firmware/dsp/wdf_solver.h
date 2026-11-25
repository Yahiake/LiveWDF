#if defined(__arm__) && !defined(GP5_HOST)
#define WDF_TABLE __attribute__((section(".data.wdf")))
#define WDF_INLINE __attribute__((always_inline))
#else
#define WDF_TABLE
#define WDF_INLINE
#endif
#include "circuit.h"
#undef WDF_TABLE

#if WDF_GAINS != DOD250_GAIN_MAX + 1
#error Gain parameter must match the exported coefficient bank
#endif

static float capacitor_state[WDF_STATES];
static float amplifier_pole;

static void wdf_reset(void)
{
    for (unsigned i = 0; i < WDF_STATES; ++i) capacitor_state[i] = 0.0f;
    amplifier_pole = initial_pole;
}

static inline float transfer(const float *row, float input)
{
    float value = row[0] + row[WDF_STATES + 1] * input;
#if defined(__GNUC__)
#pragma GCC unroll 6
#endif
    for (unsigned i = 0; i < WDF_STATES; ++i)
        value += row[i + 1] * capacitor_state[i];
    return value;
}

static inline WDF_INLINE float solve_diode_pair(float open, float impedance)
{
    /* Each table segment is linear. Search the loaded curve v + Z*I(v),
     * then invert the final segment in closed form. Extend the end segment
     * for magnitudes outside the table. */
    const float v_per_tick = 1.0f / WDF_TICKS_PER_VOLT;
    float target = open >= 0.0f ? open : -open;
    unsigned lo = 0;
    for (unsigned step = WDF_TICKS_PER_VOLT; step; step >>= 1) {
        unsigned index = lo + step;
        if ((float)index * v_per_tick + impedance * diode_lut[index] < target)
            lo = index;
    }
    float current_lo = diode_lut[lo];
    float current_step = diode_lut[lo + 1] - current_lo;
    float voltage_offset = target - (float)lo * v_per_tick - impedance * current_lo;
    float current = current_lo + current_step * (voltage_offset / (v_per_tick + impedance * current_step));
    return open >= 0.0f ? current : -current;
}

GP5_FAST static float wdf_step(float input, unsigned gain, float level_scale)
{
    const struct WdfModel *model = &models[gain];
    float amp_drive = transfer(model->amp_drive, input);
    float amp_source = transfer(model->amp_source, input) + model->load.pole_feedback * amplifier_pole;
    float diode_drive = transfer(model->diode_drive, input);
    float diode_current = solve_diode_pair(diode_drive + model->load.source_to_diode * amp_source,
                                            model->load.impedance_linear);
    float pole = amp_source + model->load.diode_to_pole * diode_current;
    float lo = amplifier_pole - model->load.slew;
    float hi = amplifier_pole + model->load.slew;
    if (lo < model->load.rail_low) lo = model->load.rail_low;
    if (hi > model->load.rail_high) hi = model->load.rail_high;
    if (pole < lo || pole > hi) {
        pole = pole < lo ? lo : hi;
        diode_current = solve_diode_pair(diode_drive + model->load.source_to_diode * pole,
                                           model->load.impedance_clamped);
    }
    float amp_current = (amp_drive - model->load.diode_to_amp * diode_current - pole) * model->load.inv_amp_load;
    const unsigned amp_col = WDF_STATES + 2;
    const unsigned diode_col = WDF_STATES + 3;
    float output = transfer(model->output, input) +
        model->output[amp_col] * amp_current + model->output[diode_col] * diode_current;
    float next[WDF_STATES];
    for (unsigned i = 0; i < WDF_STATES; ++i) {
        const float *row = model->next[i];
        next[i] = transfer(row, input) + row[amp_col] * amp_current + row[diode_col] * diode_current;
    }
    for (unsigned i = 0; i < WDF_STATES; ++i) capacitor_state[i] = next[i];
    amplifier_pole = pole;
    return output * level_scale;
}

#undef WDF_INLINE
