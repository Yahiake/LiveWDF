#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace wdf::preview {

// Fixed-snapshot approximation of an analog circuit. Each stage filters its input into
// complementary frequency bands, mixes those bands, and applies a memoryless nonlinearity.
class NonlinearPreview {
    static constexpr std::size_t width = 14;
    static constexpr std::size_t inputs = 9;
    static constexpr std::size_t stageBands = 6 * width;
    static constexpr std::size_t outputBands = 7 * width;

    template<std::size_t Cutoffs, std::size_t Channels>
    struct Bank {
        std::array<std::array<float, Channels>, Cutoffs> state{};

        std::array<float, (Cutoffs + 1) * Channels>
        step(const float* x, const std::array<float, Cutoffs>& alpha) noexcept {
            std::array<float, (Cutoffs + 1) * Channels> bands;
            for (std::size_t f = 0; f < Cutoffs; ++f) {
                for (std::size_t c = 0; c < Channels; ++c) {
                    float& s = state[f][c];
                    s += alpha[f] * (x[c] - s);
                    bands[f * Channels + c] = s - (f > 0 ? state[f - 1][c] : 0.0f);
                }
            }
            for (std::size_t c = 0; c < Channels; ++c)
                bands[Cutoffs * Channels + c] = x[c] - state.back()[c];
            return bands;
        }

        void finishBlock() noexcept {
            for (auto& band : state)
                for (auto& s : band)
                    if (std::abs(s) < 1e-30f) s = 0.0f;
        }
    };

public:
    struct Coefficients {
        std::array<float, 8> alpha0{};
        std::array<float, 5> alpha1{};
        std::array<float, 6> alpha2{};
        std::array<float, inputs> input_scale{};
        std::array<float, inputs * width> first_weights{};
        std::array<float, width> first_bias{};
        std::array<float, (stageBands + inputs) * width> second_weights{};
        std::array<float, width> second_bias{};
        std::array<float, outputBands + stageBands + inputs> output_weights{};
        float output_scale{};
    };

    // Coefficient validation and transcendental evaluation of the zero-input
    // operating point happen once, off the audio thread.
    bool prepare(const Coefficients& next) noexcept {
        const auto between = [](const auto& xs, float lo, float hi) {
            return std::all_of(xs.begin(), xs.end(), [&](float x) {
                return std::isfinite(x) && x >= lo && x <= hi;
            });
        };
        const auto poles = [&](const auto& xs) {
            return between(xs, 1e-7f, 1.0f) && std::is_sorted(xs.begin(), xs.end());
        };
        if (!poles(next.alpha0) || !poles(next.alpha1) || !poles(next.alpha2)
                || !between(next.input_scale, 1e-8f, 1e5f)
                || !between(next.first_weights, -1e4f, 1e4f)
                || !between(next.second_weights, -1e4f, 1e4f)
                || !between(next.first_bias, -256.f, 256.f)
                || !between(next.second_bias, -256.f, 256.f)
                || !between(next.output_weights, -1e4f, 1e4f)
                || !(next.output_scale > 0.f && next.output_scale <= 1e3f))
            return false;
        coefficients_ = next;
        for (std::size_t h = 0; h < width; ++h) {
            zero1_[h] = std::tanh(next.first_bias[h]);
            zero2_[h] = std::tanh(next.second_bias[h]);
        }
        reset();
        return true;
    }

    void reset() noexcept { input_ = {}; stage_ = {}; output_ = {}; }
    void finishBlock() noexcept {
        input_.finishBlock();
        stage_.finishBlock();
        output_.finishBlock();
    }

    // Input and output are volts relative to the DC point. A fresh model
    // driven at zero returns exactly zero including across long silent blocks.
    double step(double in) noexcept {
        const float dry = static_cast<float>(in);
        const auto& w = coefficients_;
        auto x = input_.step(&dry, w.alpha0);
        auto a = w.first_bias;
        auto b = w.second_bias;
        for (std::size_t i = 0; i < inputs; ++i) {
            x[i] *= w.input_scale[i];
            addRow(a, &w.first_weights[i * width], x[i]);
        }
        for (std::size_t h = 0; h < width; ++h)
            a[h] = std::tanh(a[h]) - zero1_[h];
        auto band = stage_.step(a.data(), w.alpha1);
        for (std::size_t i = 0; i < stageBands; ++i)
            addRow(b, &w.second_weights[i * width], band[i]);
        for (std::size_t i = 0; i < inputs; ++i)
            addRow(b, &w.second_weights[(stageBands + i) * width], x[i]);
        for (std::size_t h = 0; h < width; ++h)
            b[h] = std::tanh(b[h]) - zero2_[h];
        auto wet = output_.step(b.data(), w.alpha2);
        float mix = 0.f;
        for (std::size_t i = 0; i < outputBands; ++i)
            mix += w.output_weights[i] * wet[i];
        for (std::size_t i = 0; i < stageBands; ++i)
            mix += w.output_weights[outputBands + i] * band[i];
        for (std::size_t i = 0; i < inputs; ++i)
            mix += w.output_weights[outputBands + stageBands + i] * x[i];
        return mix * w.output_scale;
    }

private:
    static void addRow(std::array<float, width>& sums, const float* weights, float x) noexcept {
        for (std::size_t h = 0; h < width; ++h)
            sums[h] += weights[h] * x;
    }
    Coefficients coefficients_{};
    std::array<float, width> zero1_{}, zero2_{};
    Bank<8, 1> input_;
    Bank<5, width> stage_;
    Bank<6, width> output_;
};
} // namespace wdf::preview
