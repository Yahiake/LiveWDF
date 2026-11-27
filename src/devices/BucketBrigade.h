#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

namespace wdf {

/// A combined-output BBD and its clock. Transfer defaults use the MN3005
/// approximation in MiSTer-Discrete's circuits/parts/mn3005.lib, including
/// the open-circuit gain, clipping window and output impedance. Chip voltage
/// and clock control are relative to the common terminal.
struct BucketBrigadeModel {
    int stages = 4096;
    double clockHz = 40000.0;
    double periodSecondsPerVolt = 0.0;
    double inputBias = -8.25;
    double outputBias = -9.479;
    double inputSwing = 2.75;
    double gain = 1.052;
    double outputOhms = 5200.0;
    // Charge left behind per transfer, fitted to the MN3005 data sheet's Gi-fi
    // curve at fcp 40 kHz: -1.9 dB at 10 kHz, of which the held output is -0.9 dB.
    double transferLoss = 2.8e-5;
    // The window's knee width and second-order term, both relative to the
    // swing, fitted to the MN3005 data sheet's THD against input at 40 kHz.
    double kneeWidth = 0.1;
    double evenOrder = 0.014;
    // Insertion loss at a 1 kHz clock, falling with the square root of the
    // clock rate, read off the MN3005 data sheet's Gi-fcp curve.
    double slowClockLossDb = 0.5;

    bool valid() const noexcept {
        return stages >= 2 && stages <= 32768 && stages % 2 == 0 &&
               std::isfinite(clockHz) && clockHz >= 100.0 && clockHz <= 500000.0 &&
               std::isfinite(periodSecondsPerVolt) &&
               std::isfinite(inputBias) && std::isfinite(outputBias) &&
               std::isfinite(inputSwing) && inputSwing > 0.0 &&
               std::isfinite(gain) && gain > 0.0 &&
               std::isfinite(outputOhms) && outputOhms > 0.0 &&
               transferLoss >= 0.0 && stages * transferLoss <= 1.0 &&
               kneeWidth >= 0.0 && kneeWidth < 1.0 && std::abs(evenOrder) < 0.25 &&
               slowClockLossDb >= 0.0 && slowClockLossDb < 20.0;
    }

    double clockPeriod(double control) const noexcept {
        return std::clamp(1.0 / clockHz + periodSecondsPerVolt * control,
                          2.0e-6, 1.0e-2);
    }

    double outputVoltage(double input) const noexcept {
        const double x = std::clamp((input - inputBias) / inputSwing, -3.0, 3.0);
        const double v = x + evenOrder * x * x;
        return outputBias + gain * inputSwing * (v - beyond(v - 1.0) + beyond(-v - 1.0));
    }

    double beyond(double v) const noexcept {
        return std::max(v, 0.0) + (kneeWidth > 0.0 ? kneeWidth * std::log1p(std::exp(-std::abs(v) / kneeWidth)) : 0.0);
    }

    double dcOutputCurrent(double input) const noexcept {
        return outputVoltage(input) / outputOhms;
    }
};

/// Each full clock period advances charge by two stages. The ring therefore
/// stores stages/2 input samples, independently of host sample rate. Each
/// clock event samples the input band-limited from the host samples, seven
/// host samples late; multiple events per host sample are possible when the
/// clock runs above the host rate. The combined output
/// is a staircase that changes at clock events. Clock feedthrough and noise
/// are outside this model.
///
/// The host grid cannot sample that staircase directly: its images at
/// k fcp +- f lie above the host Nyquist rate and would fold into the band.
/// The staircase therefore passes through an 8-pole Butterworth low-pass at
/// 0.35 of the host rate, integrated exactly between clock events since it is
/// piecewise constant. The filter runs ahead of the clock by its own DC group
/// delay plus the input's seven samples, read from the samples already in the
/// line, so the delay is kept.
class BucketBrigadeState {
public:
    explicit BucketBrigadeState(const BucketBrigadeModel& model)
        : model_(model), ring_(static_cast<std::size_t>(model.stages / 2)) {
        // Each of the N transfers passes (1 - e) / (1 - e z^-1) at the clock
        // rate, so the line's impulse response is the negative binomial series
        // (1 - e)^N C(N + k - 1, k) e^k, normalized here to unit DC gain.
        double tap = std::pow(1.0 - model_.transferLoss, model_.stages), sum = 0.0;
        for (std::size_t k = 0; k < taps_.size(); ++k) {
            taps_[k] = tap;
            sum += tap;
            tap *= (model_.stages + k) * model_.transferLoss / (k + 1.0);
        }
        for (auto& t : taps_)
            t /= sum;
        std::array<std::complex<double>, 2 * kPoles> all;
        for (std::size_t k = 0; k < all.size(); ++k)
            all[k] = std::polar(1.0, std::numbers::pi * (2.0 * k + all.size() + 1.0) / (2.0 * all.size()));
        for (std::size_t k = 0; k < kPoles; ++k) {
            std::complex<double> residue = 1.0;
            for (std::size_t j = 0; j < all.size(); ++j)
                if (j != k)
                    residue /= all[k] - all[j];
            poles_[k] = all[k];
            weights_[k] = residue / all[k];
            groupDelay_ -= 2.0 * std::real(1.0 / all[k]);
        }
        reset(model_.inputBias);
    }

    void reset(double input) noexcept {
        // Counting from the history length keeps the first taps on the reset level.
        written_ = filtered_ = history_.size();
        phase_ = filterTime_ = 0.0;
        inputs_.fill(input);
        output_ = level_ = restingOutput_ = model_.outputVoltage(input);
        std::fill(ring_.begin(), ring_.end(), output_);
        history_.fill(output_);
        for (std::size_t k = 0; k < kPoles; ++k)
            state_[k] = -weights_[k] * output_;
    }

    double step(double input, double control, double samplePeriod) noexcept {
        if (!std::isfinite(input) || !std::isfinite(control) ||
            !std::isfinite(samplePeriod) || !(samplePeriod > 0.0))
            return output_;
        const double period = model_.clockPeriod(control);
        const double cycles = samplePeriod / period;
        const double nextPhase = phase_ + cycles;
        // Floating-point rounding must not move an exactly aligned clock edge
        // to the following host sample, e.g. 25 edges per 12 samples at 48 kHz.
        const double edgeTolerance = 8.0 * std::numeric_limits<double>::epsilon() *
                                     std::max(1.0, nextPhase);
        const double edges = nextPhase + edgeTolerance;
        if (!(edges >= 0.0 && edges < std::ldexp(1.0, std::numeric_limits<unsigned>::digits)))
            return output_;
        const auto clockEdges = static_cast<unsigned>(edges);
        inputs_[++newest_ % inputs_.size()] = input;
        for (unsigned i = 0; i < clockEdges; ++i) {
            const double at = (1.0 - phase_ + i) / cycles;
            const double sample = interpolate(std::clamp(at, 0.0, 1.0));
            auto& slot = ring_[written_ % ring_.size()];
            history_[written_ % history_.size()] = slot;
            slot = model_.outputVoltage(sample);
            ++written_;
        }
        phase_ = std::max(0.0, nextPhase - clockEdges);

        const double cutoff = 2.0 * std::numbers::pi * 0.35 / samplePeriod;
        const double retained = std::pow(10.0, -0.05 * model_.slowClockLossDb * std::sqrt(1.0e3 * period));
        // Edges already in the line are known up to stages/2 periods ahead.
        const double ahead = std::min(groupDelay_ / cutoff + kInputDelay * samplePeriod,
                                      (ring_.size() - 1.0) * period);
        filterTime_ -= samplePeriod;
        const double until = std::max(filterTime_, ahead);
        std::array<std::complex<double>, kPoles> spacing;
        for (std::size_t k = 0; k < kPoles; ++k)
            spacing[k] = std::exp(poles_[k] * (cutoff * period));
        for (;; ++filtered_) {
            const double at = (1.0 - phase_ + (double(filtered_) - double(written_))) * period;
            if (at > until || filtered_ >= written_ + ring_.size())
                break;
            advance(std::max(at, filterTime_), cutoff, spacing, period);
            double charge = 0.0;
            for (std::size_t k = 0; k < taps_.size(); ++k)
                charge += taps_[k] * stored(filtered_ - k);
            level_ = restingOutput_ + retained * (charge - restingOutput_);
        }
        advance(until, cutoff, spacing, period);
        output_ = 0.0;
        for (const auto& s : state_)
            output_ += 2.0 * std::real(s);
        return output_;
    }

    double outputCurrent() const noexcept { return output_ / model_.outputOhms; }

private:
    static constexpr std::size_t kPoles = 4, kTaps = 16, kPhases = 512;
    static constexpr int kInputDelay = kTaps / 2 - 1;

    // Kaiser-windowed sinc at the host Nyquist rate, beta 7: about -70 dB
    // from 0.65 of the host rate, where the images of the host samples start.
    static const std::array<std::array<double, kTaps>, kPhases + 1>& kernel() {
        static const auto table = [] {
            std::array<std::array<double, kTaps>, kPhases + 1> t{};
            for (std::size_t p = 0; p <= kPhases; ++p) {
                double sum = 0.0;
                for (std::size_t i = 0; i < kTaps; ++i) {
                    const double x = double(p) / kPhases - kTaps / 2.0 + double(i);
                    const double r = x / (kTaps / 2.0);
                    const double sinc = x == 0.0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
                    t[p][i] = std::abs(r) < 1.0 ? sinc * std::cyl_bessel_i(0.0, 7.0 * std::sqrt(1.0 - r * r)) : 0.0;
                    sum += t[p][i];
                }
                for (auto& v : t[p])
                    v /= sum;
            }
            return t;
        }();
        return table;
    }

    // The input at fraction `at` of the last host interval, kInputDelay
    // samples earlier.
    double interpolate(double at) const noexcept {
        const double position = at * kPhases;
        const auto p = std::min(static_cast<std::size_t>(position), kPhases - 1);
        const double blend = position - p;
        const auto& lo = kernel()[p];
        const auto& hi = kernel()[p + 1];
        double sum = 0.0;
        for (std::size_t i = 0; i < kTaps; ++i)
            sum += std::lerp(lo[i], hi[i], blend) * inputs_[(newest_ - i) % inputs_.size()];
        return sum;
    }

    double stored(std::size_t edge) const noexcept {
        return edge < written_ ? history_[edge % history_.size()] : ring_[edge % ring_.size()];
    }

    void advance(double to, double cutoff, const std::array<std::complex<double>, kPoles>& spacing,
                 double period) noexcept {
        const double span = to - filterTime_;
        if (!(span > 0.0))
            return;
        const bool onePeriod = std::abs(span - period) <= 1e-9 * period;
        for (std::size_t k = 0; k < kPoles; ++k) {
            const auto decay = onePeriod ? spacing[k] : std::exp(poles_[k] * (cutoff * span));
            state_[k] = decay * (state_[k] + weights_[k] * level_) - weights_[k] * level_;
        }
        filterTime_ = to;
    }

    BucketBrigadeModel model_;
    std::vector<double> ring_;
    std::array<double, 8> taps_{}, history_{};
    std::array<double, 32> inputs_{};
    std::size_t newest_ = 0;
    std::array<std::complex<double>, kPoles> poles_{}, weights_{}, state_{};
    double groupDelay_ = 0.0;
    std::size_t written_ = 0, filtered_ = 0;
    double phase_ = 0.0, filterTime_ = 0.0;
    double output_ = 0.0, level_ = 0.0;
    double restingOutput_ = 0.0;
};

} // namespace wdf
