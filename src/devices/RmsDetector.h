#pragma once

#include <algorithm>
#include <cmath>

namespace wdf {

/// Log-domain RMS detection. Rectification, doubling
/// in the log domain, filtering, and square root reduce to a one-pole filter
/// on normalized input power followed by a logarithm. It includes no noise,
/// input overload recovery, or temperature drift.
/// https://www.thatcorp.com/datashts/THAT_2252_Datasheet.pdf (theory of operation)
/// https://thatcorp.com/datashts/dn05.pdf (16 ms example and reference level)
struct RmsDetectorModel {
    double referenceVolts = 1.0;       // RMS input voltage for 0 V output
    double timeConstantSeconds = 0.016;
    double voltsPerDb = 0.0061;
    double inputOhms = 1.0e6;
    double outputOhms = 10.0;
    bool negativeOutput = false;

    // dbx 160/161, M2: calibrated current-input equivalent at 25 C.
    // C15 is the externally selected 22 uF timing capacitor. An exponential
    // decay of mean power at tau gives 10 / (ln(10) * tau) dB/s in silence.
    static RmsDetectorModel dbx209(double referenceAmps, double timingFarads) {
        RmsDetectorModel model;
        model.inputOhms = 10.0; // virtual-ground input sensing impedance
        model.referenceVolts = referenceAmps * model.inputOhms;
        model.timeConstantSeconds = (10.0 / (std::log(10.0) * 120.0)) * (timingFarads / 22e-6);
        model.voltsPerDb = 0.006;
        model.negativeOutput = true;
        return model;
    }

    double signedVoltsPerDb() const noexcept {
        return negativeOutput ? -voltsPerDb : voltsPerDb;
    }

    static constexpr double minimumPower = 1.0e-10; // −100 dB relative to reference
    static constexpr double maximumPower = 1.0e4;   // +40 dB clipping at the output
    static constexpr double maximumNormalizedInput = 1.0e6;

    bool valid() const noexcept {
        return std::isfinite(referenceVolts) && referenceVolts > 0.0 &&
               std::isfinite(timeConstantSeconds) && timeConstantSeconds > 0.0 &&
               std::isfinite(voltsPerDb) && voltsPerDb > 0.0 &&
               std::isfinite(100.0 * voltsPerDb) &&
               std::isfinite(inputOhms) && inputOhms > 0.0 &&
               std::isfinite(outputOhms) && outputOhms > 0.0 &&
               std::isfinite(100.0 * voltsPerDb / outputOhms);
    }

    double power(double input) const noexcept {
        const double relative = std::clamp(input / referenceVolts,
                                           -maximumNormalizedInput, maximumNormalizedInput);
        return relative * relative;
    }

    double current(double meanPower) const noexcept {
        const double db = 10.0 * std::log10(std::clamp(meanPower, minimumPower, maximumPower));
        return (signedVoltsPerDb() * db) / outputOhms;
    }

    double dcOutputCurrent(double input) const noexcept {
        return current(power(input));
    }
};

class RmsDetectorState {
public:
    RmsDetectorState(const RmsDetectorModel& model, double samplePeriod) noexcept
        : model_(model), mix_(-std::expm1(-samplePeriod / model.timeConstantSeconds)) {}

    void reset(double input) noexcept { meanPower_ = model_.power(input); }

    void step(double input) noexcept {
        // Exact one-pole integration for a sample held over one interval.
        meanPower_ = std::lerp(meanPower_, model_.power(input), mix_);
    }

    double outputCurrent() const noexcept { return model_.current(meanPower_); }

private:
    RmsDetectorModel model_;
    double mix_;
    double meanPower_ = 0.0;
};

} // namespace wdf
