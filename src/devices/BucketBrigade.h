#pragma once

#include <algorithm>
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

    bool valid() const noexcept {
        return stages >= 2 && stages <= 32768 && stages % 2 == 0 &&
               std::isfinite(clockHz) && clockHz >= 100.0 && clockHz <= 500000.0 &&
               std::isfinite(periodSecondsPerVolt) &&
               std::isfinite(inputBias) && std::isfinite(outputBias) &&
               std::isfinite(inputSwing) && inputSwing > 0.0 &&
               std::isfinite(gain) && gain > 0.0 &&
               std::isfinite(outputOhms) && outputOhms > 0.0;
    }

    double clockPeriod(double control) const noexcept {
        return std::clamp(1.0 / clockHz + periodSecondsPerVolt * control,
                          2.0e-6, 1.0e-2);
    }

    double outputVoltage(double input) const noexcept {
        return outputBias + gain * std::clamp(input - inputBias, -inputSwing, inputSwing);
    }

    double dcOutputCurrent(double input) const noexcept {
        return outputVoltage(input) / outputOhms;
    }
};

/// Each full clock period advances charge by two stages. The ring therefore
/// stores stages/2 input samples, independently of host sample rate. Each
/// clock event samples an interpolated input; multiple events per host sample
/// are possible when the clock runs above the host rate. Combined output is
/// held until the next event. Clock feedthrough and loss/noise per stage are
/// outside this model.
class BucketBrigadeState {
public:
    explicit BucketBrigadeState(const BucketBrigadeModel& model)
        : model_(model), ring_(static_cast<std::size_t>(model.stages / 2)) {
        reset(model_.inputBias);
    }

    void reset(double input) noexcept {
        position_ = 0;
        phase_ = 0.0;
        previousInput_ = input;
        output_ = model_.outputVoltage(input);
        std::fill(ring_.begin(), ring_.end(), output_);
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
            output_ = ring_[position_];
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
    std::size_t position_ = 0;
    double phase_ = 0.0;
    double previousInput_ = 0.0;
    double output_ = 0.0;
};

} // namespace wdf
