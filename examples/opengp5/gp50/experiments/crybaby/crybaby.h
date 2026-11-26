#ifndef LIVEWDF_GP50_CRYBABY_H
#define LIVEWDF_GP50_CRYBABY_H

#ifdef __arm__
#include "gp5_math.h"
#else
#include <math.h>
#endif

#include "crybaby_model.h"

struct CryBabyState {
    float wave[CRYBABY_STATES];
    float voltage[CRYBABY_JUNCTIONS];
    float voltage_delta[CRYBABY_JUNCTIONS];
    float last_output;
    unsigned position;
};

#define CRYBABY_MAX_ITERATIONS 18

static inline float crybaby_dot6(const float* a, const float* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] + a[4] * b[4] + a[5] * b[5];
}

static inline float crybaby_dot9(const float* a, const float* b)
{
    return crybaby_dot6(a, b) + a[6] * b[6] + a[7] * b[7] + a[8] * b[8];
}

static inline float crybaby_dot15(const float* a, const float* b)
{
    return crybaby_dot9(a, b) + crybaby_dot6(a + 9, b + 9);
}

static inline float crybaby_delta_current(float initial, float baseline, float delta, float* derivative)
{
    const float inv_vt = 1.0f / 0.02585f;
    float change = delta * inv_vt;
    const float argument = initial + change;
    if (argument <= -24.0f) { /* Less than 3.8e-25 A, below the network's FP32 precision. */
        *derivative = 0.0f;
        return -baseline;
    }
    if (argument >= 40.0f) {
        *derivative = 0.0f;
        return 1.0e-14f * expf(40.0f) - baseline;
    }
    if (fabsf(change) < 0.03f) {
        const float relative = change * (1.0f + change * (0.5f + change * (1.0f / 6.0f + change / 24.0f)));
        *derivative = baseline * (1.0f + relative) * inv_vt;
        return baseline * relative;
    }
    const float current = fabsf(change) < 40.0f ? baseline * expf(change) : 1.0e-14f * expf(argument);
    *derivative = current * inv_vt;
    return current - baseline;
}

static int crybaby_solve(float jacobian[6][6], float rhs[6], unsigned size)
{
    float inverse[6];
    for (unsigned pivot = 0; pivot < size; ++pivot) {
        unsigned row = pivot;
        for (unsigned candidate = pivot + 1; candidate < size; ++candidate)
            if (fabsf(jacobian[candidate][pivot]) > fabsf(jacobian[row][pivot])) row = candidate;
        if (!(fabsf(jacobian[row][pivot]) > 1.0e-8f)) return 0;
        if (row != pivot) {
            for (unsigned column = pivot; column < size; ++column) {
                const float value = jacobian[row][column];
                jacobian[row][column] = jacobian[pivot][column];
                jacobian[pivot][column] = value;
            }
            const float value = rhs[row];
            rhs[row] = rhs[pivot];
            rhs[pivot] = value;
        }
        inverse[pivot] = 1.0f / jacobian[pivot][pivot];
        for (row = pivot + 1; row < size; ++row) {
            const float scale = jacobian[row][pivot] * inverse[pivot];
            for (unsigned column = pivot + 1; column < size; ++column)
                jacobian[row][column] -= scale * jacobian[pivot][column];
            rhs[row] -= scale * rhs[pivot];
        }
    }
    for (int row = (int)size - 1; row >= 0; --row) {
        for (unsigned column = (unsigned)row + 1; column < size; ++column)
            rhs[row] -= jacobian[row][column] * rhs[column];
        rhs[row] *= inverse[row];
    }
    return 1;
}

static inline float crybaby_limit(float voltage, float bias, float delta)
{
    const float absolute = voltage + bias * 0.02585f;
    const float limit = absolute < 0.55f ? 0.64f - absolute : 0.09f;
    return voltage + (delta > limit ? limit : delta);
}

// These are the dominant slopes when all three BJTs are forward-active.
// The physical residual still checks both junctions of each transistor.
static int crybaby_forward_direction(const struct CryBabyTransfer* model, const float slopes[6], float delta[6])
{
    const float (*z)[6] = model->impedance;
    const float s0 = slopes[0], s1 = slopes[2], s2 = slopes[4];
    const float a = 1.0f + z[0][0] * s0, b = z[0][2] * s1, c = z[0][4] * s2;
    const float d = z[2][0] * s0, e = 1.0f + z[2][2] * s1, f = z[2][4] * s2;
    const float g = z[4][0] * s0, h = z[4][2] * s1, i = 1.0f + z[4][4] * s2;
    const float aa = e * i - f * h, ba = f * g - d * i, ca = d * h - e * g;
    const float det = a * aa + b * ba + c * ca;
    if (!(fabsf(det) > 1.0e-8f)) return 0;
    const float inv = 1.0f / det;
    const float j = delta[0], k = delta[2], l = delta[4];
    delta[0] = inv * (aa * j + (c * h - b * i) * k + (b * f - c * e) * l);
    delta[2] = inv * (ba * j + (a * i - c * g) * k + (c * d - a * f) * l);
    delta[4] = inv * (ca * j + (b * g - a * h) * k + (a * e - b * d) * l);
    const float c0 = delta[0] * s0, c1 = delta[2] * s1, c2 = delta[4] * s2;
    for (unsigned row = 1; row < 6; row += 2)
        delta[row] -= z[row][0] * c0 + z[row][2] * c1 + z[row][4] * c2;
    return 1;
}

// The caller keeps the physical capacitor/inductor state when changing treadle.
// An unaccepted nonlinear step leaves the previous accepted state untouched.
GP5_FAST static int crybaby_step(struct CryBabyState* state, float input, unsigned position, float* output)
{
    const struct CryBabyTransfer* model = &crybaby_transfer[position];
    float signal[15], voltage[6], derivative[6], drive[6];
    for (unsigned i = 0; i < 8; ++i) signal[i] = state->wave[i];
    signal[8] = input;
    for (unsigned i = 0; i < 6; ++i) {
        drive[i] = crybaby_dot9(model->drive[i], signal);
        voltage[i] = state->voltage[i];
        if (state->position == position)
            voltage[i] = crybaby_limit(voltage[i], model->bias_argument[i], state->voltage_delta[i]);
    }
    for (unsigned iteration = 0; iteration < CRYBABY_MAX_ITERATIONS; ++iteration) {
        float residual[6], norm = 0.0f;
        for (unsigned i = 0; i < 6; ++i)
            signal[9 + i] = crybaby_delta_current(model->bias_argument[i], model->bias_current[i],
                                                   voltage[i], &derivative[i]);
        for (unsigned i = 0; i < 6; ++i) {
            residual[i] = drive[i] - voltage[i] - crybaby_dot6(model->impedance[i], signal + 9);
            if (fabsf(residual[i]) > norm) norm = fabsf(residual[i]);
        }
        if (norm < 2.0e-6f) {
            for (unsigned i = 0; i < 8; ++i)
                state->wave[i] = crybaby_dot15(model->advance[i], signal);
            for (unsigned i = 0; i < 6; ++i) {
                state->voltage_delta[i] = voltage[i] - state->voltage[i];
                state->voltage[i] = voltage[i];
            }
            state->position = position;
            *output = state->last_output = crybaby_dot15(model->advance[8], signal);
            return (int)iteration + 1;
        }
        // Ignore negligible slopes in the Newton direction only. The full
        // physical residual above still decides if a sample can commit.
        unsigned active[6], count = 0;
        for (unsigned i = 0; i < 6; ++i)
            if (derivative[i] > 1.0e-9f) active[count++] = i;
        if (count == 3 && active[0] == 0 && active[1] == 2 && active[2] == 4
            && crybaby_forward_direction(model, derivative, residual)) {
            for (unsigned i = 0; i < 6; ++i)
                voltage[i] = crybaby_limit(voltage[i], model->bias_argument[i], residual[i]);
            continue;
        }
        float jacobian[6][6], direction[6], current_step[6];
        for (unsigned row = 0; row < count; ++row) {
            direction[row] = residual[active[row]];
            for (unsigned column = 0; column < count; ++column)
                jacobian[row][column] = model->impedance[active[row]][active[column]] * derivative[active[column]];
            jacobian[row][row] += 1.0f;
        }
        if (!crybaby_solve(jacobian, direction, count)) break;
        for (unsigned i = 0; i < count; ++i) current_step[i] = derivative[active[i]] * direction[i];
        for (unsigned i = 0; i < 6; ++i) {
            float delta = residual[i];
            for (unsigned j = 0; j < count; ++j) delta -= model->impedance[i][active[j]] * current_step[j];
            voltage[i] = crybaby_limit(voltage[i], model->bias_argument[i], delta);
        }
    }
    *output = state->last_output;
    return 0;
}

#endif
