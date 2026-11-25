#ifndef LIVEWDF_GP50_EFFECTS_H
#define LIVEWDF_GP50_EFFECTS_H

#include <stdint.h>

/* Optional post-model effects at 44.1 kHz. Cabinet is a switch, echo and hall
 * are wet percentages. A zero setting clears that stage's history. */
#define EFFECTS_CAB_TAPS 48u
#define EFFECTS_ECHO_SAMPLES 11025u
#define EFFECTS_HALL_LINES (751u + 907u + 1069u + 1277u + 1487u + 1709u + 1889u + 2113u)

typedef struct {
    float cab[EFFECTS_CAB_TAPS * 2u];
    unsigned cab_next, cab_enabled, echo_next, echo_ready;
    unsigned hall_age, line_next[8], diffuser_next[4];
    float absorption[8], last_input, last_left, last_right;
} effects_state_t;

static effects_state_t effects_state;
static int16_t effects_echo[EFFECTS_ECHO_SAMPLES] GP5_LARGE;
static float effects_lines[EFFECTS_HALL_LINES] GP5_LARGE;
static float effects_diffusers[71 + 53 + 191 + 139] GP5_LARGE;

/* First half of a symmetric synthetic cabinet IR. Hann-windowed 4.4 kHz
 * low-pass sinc with a low-frequency shelf, normalized to 0.88 peak gain. */
static const float effects_cab_ir[EFFECTS_CAB_TAPS / 2u] = {
     0.00000000f,  0.00000535f, -0.00001630f, -0.00028957f, -0.00110638f, -0.00252006f,
    -0.00409955f, -0.00496814f, -0.00421586f, -0.00154458f,  0.00219604f,  0.00496724f,
     0.00420514f, -0.00193926f, -0.01326570f, -0.02675501f, -0.03690152f, -0.03727234f,
    -0.02287111f,  0.00755562f,  0.05024275f,  0.09683055f,  0.13652890f,  0.15936278f,
};

static void effects_reset(void)
{
    /* The positions and age gate reads until each buffer has fresh samples. */
    effects_state = (effects_state_t){0};
}

static GP5_FAST float effects_cab_step(float input)
{
    unsigned pos = effects_state.cab_next;
    float *window = effects_state.cab + pos;
    window[0] = window[EFFECTS_CAB_TAPS] = input;
    effects_state.cab_next = pos == 0 ? EFFECTS_CAB_TAPS - 1u : pos - 1u;
    float output = 0.0f;
    for (unsigned i = 0; i < EFFECTS_CAB_TAPS / 2u; ++i)
        output += effects_cab_ir[i] * (window[i] + window[EFFECTS_CAB_TAPS - 1u - i]);
    return output;
}

static inline float effects_echo_step(float input, float mix)
{
    unsigned pos = effects_state.echo_next;
    float repeat = effects_state.echo_ready ? (float)effects_echo[pos] * (1.0f / 32768.0f) : 0.0f;
    float feedback = (input + 0.42f * repeat) * 32768.0f;
    if (feedback > 32767.0f) feedback = 32767.0f;
    if (feedback < -32768.0f) feedback = -32768.0f;
    effects_echo[pos++] = (int16_t)feedback;
    if (pos == EFFECTS_ECHO_SAMPLES) {
        pos = 0;
        effects_state.echo_ready = 1;
    }
    effects_state.echo_next = pos;
    return input + mix * (repeat - input);
}

/* The half-rate hall is adapted from Dreamcatcher dsp/hall.h
 * https://github.com/OpenGP5/Dreamcatcher/blob/6c1f2728c715671ecb1148d090fe6df1cb4f6e8f/dsp/hall.h
 * Fixed delays and absorption give a two-second low-frequency decay.
 *
 * MIT License
 * Copyright (c) 2026 the Dreamcatcher contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
static GP5_FAST void effects_hall_step(float input, float *left, float *right)
{
    static const unsigned delay[] = {751, 907, 1069, 1277, 1487, 1709, 1889, 2113};
    static const unsigned diffuser[] = {71, 53, 191, 139};
    static const float gain[] = {0.286657f, 0.274169f, 0.261684f, 0.246356f,
                                 0.231653f, 0.216924f, 0.205573f, 0.192160f};
    static const float pole[] = {0.087998f, 0.106152f, 0.124929f, 0.148905f,
                                 0.172935f, 0.198116f, 0.218345f, 0.243257f};
    unsigned age = effects_state.hall_age;
    float *line = effects_diffusers;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned pos = effects_state.diffuser_next[i];
        float stored = age >= diffuser[i] ? line[pos] : 0.0f;
        float sum = input + 0.6f * stored;
        line[pos++] = sum;
        effects_state.diffuser_next[i] = pos == diffuser[i] ? 0 : pos;
        line += diffuser[i];
        input = stored - 0.6f * sum;
    }

    float mix[8];
    line = effects_lines;
    for (unsigned i = 0; i < 8; ++i) {
        mix[i] = age >= delay[i] ? line[effects_state.line_next[i]] : 0.0f;
        line += delay[i];
    }
    *left = 0.35f * (mix[0] + mix[2] + mix[4] + mix[6]);
    *right = 0.35f * (mix[1] + mix[3] + mix[5] + mix[7]);

    /* Normalized Hadamard feedback. The line gains include 1/sqrt(8). */
    for (unsigned span = 1; span < 8; span *= 2) {
        for (unsigned i = 0; i < 8; i += span * 2) {
            for (unsigned j = i; j < i + span; ++j) {
                float a = mix[j], b = mix[j + span];
                mix[j] = a + b;
                mix[j + span] = a - b;
            }
        }
    }

    line = effects_lines;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned pos = effects_state.line_next[i];
        float filtered = gain[i] * mix[i] + pole[i] * effects_state.absorption[i];
        effects_state.absorption[i] = filtered;
        line[pos++] = filtered + ((i & 1u) ? -0.5f : 0.5f) * input;
        effects_state.line_next[i] = pos == delay[i] ? 0 : pos;
        line += delay[i];
    }
    if (age < delay[7]) effects_state.hall_age = age + 1u;
}

static inline int32_t effects_to_q31(float x)
{
    if (x >= 1.0f) return INT32_MAX;
    if (x <= -1.0f) return INT32_MIN;
    return (int32_t)(x * GP5_FULL_SCALE);
}

static GP5_FAST void effects_block(const int32_t *mono, int32_t *left, int32_t *right,
                                  unsigned cab, unsigned delay, unsigned reverb)
{
    if (!cab && effects_state.cab_enabled) {
        for (unsigned i = 0; i < EFFECTS_CAB_TAPS * 2u; ++i) effects_state.cab[i] = 0.0f;
    }
    effects_state.cab_enabled = cab;
    if (!delay) effects_state.echo_next = effects_state.echo_ready = 0;
    if (!reverb && effects_state.hall_age) {
        effects_state.hall_age = 0;
        effects_state.last_input = effects_state.last_left = effects_state.last_right = 0.0f;
        for (unsigned i = 0; i < 8; ++i) effects_state.absorption[i] = 0.0f;
    }
    if (!cab && !delay && !reverb) {
        for (unsigned i = 0; i < GP5_BLOCK; ++i) left[i] = right[i] = mono[i];
        return;
    }

    float echo_mix = (float)delay * 0.01f, hall_mix = (float)reverb * 0.01f;
    for (unsigned i = 0; i < GP5_BLOCK; i += 2u) {
        float first = (float)mono[i] / GP5_FULL_SCALE;
        float second = (float)mono[i + 1u] / GP5_FULL_SCALE;
        if (cab) {
            first = effects_cab_step(first);
            second = effects_cab_step(second);
        }
        if (delay) {
            first = effects_echo_step(first, echo_mix);
            second = effects_echo_step(second, echo_mix);
        }
        if (!reverb) {
            left[i] = right[i] = effects_to_q31(first);
            left[i + 1u] = right[i + 1u] = effects_to_q31(second);
            continue;
        }

        float wet_left, wet_right;
        float half_rate = 0.25f * effects_state.last_input + 0.5f * first + 0.25f * second;
        effects_hall_step(half_rate, &wet_left, &wet_right);
        left[i] = effects_to_q31(first + hall_mix * (0.5f * (effects_state.last_left + wet_left) - first));
        right[i] = effects_to_q31(first + hall_mix * (0.5f * (effects_state.last_right + wet_right) - first));
        left[i + 1u] = effects_to_q31(second + hall_mix * (wet_left - second));
        right[i + 1u] = effects_to_q31(second + hall_mix * (wet_right - second));
        effects_state.last_input = second;
        effects_state.last_left = wet_left;
        effects_state.last_right = wet_right;
    }
}

#endif
