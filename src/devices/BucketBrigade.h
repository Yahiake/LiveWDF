#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
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

    bool valid() const noexcept {
        return stages >= 2 && stages <= 32768 && stages % 2 == 0 &&
               std::isfinite(clockHz) && clockHz >= 100.0 && clockHz <= 500000.0 &&
               std::isfinite(periodSecondsPerVolt) &&
               std::isfinite(inputBias) && std::isfinite(outputBias) &&
               std::isfinite(inputSwing) && inputSwing > 0.0 &&
               std::isfinite(gain) && gain > 0.0 &&
               std::isfinite(outputOhms) && outputOhms > 0.0 &&
               transferLoss >= 0.0 && stages * transferLoss <= 1.0 &&
               kneeWidth >= 0.0 && kneeWidth < 1.0 && std::abs(evenOrder) < 0.25;
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
/// clock event samples an interpolated input; multiple events per host sample
/// are possible when the clock runs above the host rate. Combined output is
/// held until the next event. Clock feedthrough and noise are outside this
/// model.
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
        reset(model_.inputBias);
    }

    void reset(double input) noexcept {
        position_ = 0;
        phase_ = 0.0;
        previousInput_ = input;
        output_ = model_.outputVoltage(input);
        std::fill(ring_.begin(), ring_.end(), output_);
        history_.fill(output_);
    }

    double step(double input, double control, double samplePeriod) noexcept {
        if (!std::isfinite(input) || !std::isfinite(control) ||
            !std::isfinite(samplePeriod) || !(samplePeriod > 0.0))
            return output_;
        const double cycles = samplePeriod / model_.clockPeriod(control);
        const double nextPhase = phase_ + cycles;
        // Floating-point rounding must not move an exactly aligned clock edge
        // to the following host sample, e.g. 25 edges per 12 samples at 48 kHz.
        const double edgeTolerance = 8.0 * std::numeric_limits<double>::epsilon() *
                                     std::max(1.0, nextPhase);
        const double edges = nextPhase + edgeTolerance;
        if (!(edges >= 0.0 && edges < std::ldexp(1.0, std::numeric_limits<unsigned>::digits)))
            return output_;
        const auto clockEdges = static_cast<unsigned>(edges);
        for (unsigned i = 0; i < clockEdges; ++i) {
            const double at = (1.0 - phase_ + i) / cycles;
            const double sample = std::lerp(previousInput_, input, std::clamp(at, 0.0, 1.0));
            historyPosition_ = (historyPosition_ + 1) % history_.size();
            history_[historyPosition_] = ring_[position_];
            output_ = 0.0;
            for (std::size_t k = 0; k < taps_.size(); ++k)
                output_ += taps_[k] * history_[(historyPosition_ + history_.size() - k) % history_.size()];
            ring_[position_] = model_.outputVoltage(sample);
            if (++position_ == ring_.size())
                position_ = 0;
        }
        phase_ = std::max(0.0, nextPhase - clockEdges);
        previousInput_ = input;
        return output_;
    }

    double outputCurrent() const noexcept { return output_ / model_.outputOhms; }

private:
    BucketBrigadeModel model_;
    std::vector<double> ring_;
    std::array<double, 8> taps_{}, history_{};
    std::size_t position_ = 0, historyPosition_ = 0;
    double phase_ = 0.0;
    double previousInput_ = 0.0;
    double output_ = 0.0;
};

} // namespace wdf
