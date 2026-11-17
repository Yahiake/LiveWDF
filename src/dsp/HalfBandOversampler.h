#pragma once

// Halfband IIR Antialiasing Oversampler for LiveWDF.
//
// Cleanroom C++ port of PedalKernel's minimum-latency oversampler
// (`pedalkernel-rt/src/oversampling.rs`).
//
// Nonlinear circuit stages (diode clippers, saturated op-amp rails, BJT/JFET/MOSFET
// overdrive, tube clipping) generate high-frequency harmonic content that folds
// back across the Nyquist boundary at base sample rates (44.1 kHz / 48 kHz), causing
// inharmonic digital aliasing.
//
// This class provides a zero-latency / minimum-phase oversampler that:
//   1. Upsamples the input signal by inserting zeros (zero-stuffing) and anti-imaging filtering
//   2. Evaluates the nonlinear stage or engine at 2x or 4x oversampled rate
//   3. Anti-alias filters and decimates back to the base rate
//
// The filters use cascaded Butterworth biquads designed via the bilinear transform
// at cutoff fc = 0.5 / factor of the oversampled rate (= base Nyquist frequency).
// 2x oversampling uses a 6th-order cascade (3 biquads, ~80 dB alias rejection).
// 4x oversampling uses an 8th-order cascade (4 biquads, ~80 dB alias rejection).
// Both preserve exact unity DC gain.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace wdf {

inline constexpr double kHalfBandPi = 3.14159265358979323846;

/// Oversampling factor options.
enum class OversamplingFactor {
    X1 = 1,
    X2 = 2,
    X4 = 4,
};

/// Cascaded Butterworth biquad half-band lowpass filter.
class HalfBandFilter {
public:
    HalfBandFilter() = default;
    explicit HalfBandFilter(OversamplingFactor factor) {
        configure(factor);
    }

    void configure(OversamplingFactor factor) {
        sections_.clear();
        state_.clear();
        switch (factor) {
            case OversamplingFactor::X1:
                break;
            case OversamplingFactor::X2:
                // 6th-order Butterworth, fc = 0.25 * fs_oversampled
                sections_ = designButterworth(6, 0.25);
                break;
            case OversamplingFactor::X4:
                // 8th-order Butterworth, fc = 0.125 * fs_oversampled
                sections_ = designButterworth(8, 0.125);
                break;
        }
        state_.assign(sections_.size(), {0.0, 0.0, 0.0, 0.0});
    }

    /// Process one sample through the cascaded biquads with denormal flushing.
    inline double process(double input) noexcept {
        double x = input;
        for (size_t i = 0; i < sections_.size(); ++i) {
            const auto& coef = sections_[i];
            auto& s = state_[i];
            // y[n] = b0*x + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
            const double y = coef[0] * x + coef[1] * s[0] + coef[2] * s[1]
                             - coef[3] * s[2] - coef[4] * s[3];
            s[1] = s[0];
            s[0] = x;
            s[3] = s[2];
            // Flush subnormals to prevent x86 CPU performance degradation
            s[2] = (std::abs(y) < 1.0e-15) ? 0.0 : y;
            x = s[2];
        }
        return x;
    }

    void reset() noexcept {
        for (auto& s : state_) {
            s = {0.0, 0.0, 0.0, 0.0};
        }
    }

    size_t sectionCount() const noexcept { return sections_.size(); }

private:
    /// Design Butterworth lowpass filter as cascaded second-order sections.
    /// order must be even. fcNormalized is fc/fs (0 to 0.5).
    /// Returns vector of [b0, b1, b2, a1, a2] per biquad section.
    static std::vector<std::array<double, 5>> designButterworth(size_t order, double fcNormalized) {
        const size_t numSections = order / 2;
        const double wc = std::tan(kHalfBandPi * fcNormalized);
        const double wc2 = wc * wc;

        std::vector<std::array<double, 5>> sections;
        sections.reserve(numSections);

        for (size_t k = 0; k < numSections; ++k) {
            // Butterworth pole angle: theta = (2k + 1) * pi / (2 * order)
            const double theta = (2.0 * static_cast<double>(k) + 1.0) * kHalfBandPi /
                                 (2.0 * static_cast<double>(order));
            const double bw = 2.0 * std::cos(theta);

            // Bilinear transform: s = 2 * (z-1) / (z+1)
            // H(s) = wc^2 / (s^2 + bw*wc*s + wc^2)
            const double a0 = 4.0 + 2.0 * bw * wc + wc2;
            const double a1Coef = (2.0 * wc2 - 8.0) / a0;
            const double a2Coef = (4.0 - 2.0 * bw * wc + wc2) / a0;
            const double b0 = wc2 / a0;
            const double b1 = 2.0 * wc2 / a0;
            const double b2 = wc2 / a0;

            sections.push_back({b0, b1, b2, a1Coef, a2Coef});
        }
        return sections;
    }

    // [b0, b1, b2, a1, a2] per biquad section
    std::vector<std::array<double, 5>> sections_;
    // [x1, x2, y1, y2] per biquad section
    std::vector<std::array<double, 4>> state_;
};

/// High-performance minimum-latency oversampler wrapping an arbitrary nonlinear callable.
class HalfBandOversampler {
public:
    explicit HalfBandOversampler(OversamplingFactor factor = OversamplingFactor::X2)
        : factor_(factor),
          upsampleFilter_(factor),
          downsampleFilter_(factor) {}

    void setFactor(OversamplingFactor factor) {
        if (factor != factor_) {
            factor_ = factor;
            upsampleFilter_.configure(factor);
            downsampleFilter_.configure(factor);
        }
    }

    OversamplingFactor factor() const noexcept { return factor_; }

    size_t ratio() const noexcept {
        return static_cast<size_t>(factor_);
    }

    /// Process a single base-rate sample through the oversampled processing stage.
    /// `func` is called `ratio()` times with the oversampled input sample.
    template <typename Func>
    inline double process(double input, Func&& func) {
        const size_t r = ratio();
        if (r == 1) {
            return func(input);
        }

        // Upsample: insert zeros between samples.
        // Scale by factor r to compensate for zero-stuffing energy loss and maintain unity gain.
        const double gain = static_cast<double>(r);
        double lastOut = 0.0;

        for (size_t i = 0; i < r; ++i) {
            const double up = (i == 0) ? (input * gain) : 0.0;
            const double filtered = upsampleFilter_.process(up);
            const double processed = func(filtered);
            lastOut = downsampleFilter_.process(processed);
        }

        // Decimate: the last filtered sample is the downsampled output
        return lastOut;
    }

    /// Process a contiguous buffer of samples.
    template <typename Func>
    void processBlock(const double* input, double* output, size_t numSamples, Func&& func) {
        for (size_t i = 0; i < numSamples; ++i) {
            output[i] = process(input[i], func);
        }
    }

    void reset() noexcept {
        upsampleFilter_.reset();
        downsampleFilter_.reset();
    }

private:
    OversamplingFactor factor_ = OversamplingFactor::X2;
    HalfBandFilter upsampleFilter_;
    HalfBandFilter downsampleFilter_;
};

}  // namespace wdf
