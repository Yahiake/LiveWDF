#pragma once

// Dynamic Power Supply Sag Model for LiveWDF.
//
// Cleanroom C++ port of PedalKernel's PowerSupply element
// (`pedalkernel-rt/src/elements/linear.rs`).
//
// In analog amplifiers and pedals, the power supply voltage is not an ideal,
// infinitely stiff source. The supply has an internal output impedance
// (transformer winding resistance, rectifier forward resistance, battery internal
// resistance, and filter capacitor ESR) and reservoir filter capacitance.
//
// Under heavy load (e.g. loud chords driving output stages or clipping diodes into
// saturation), the current drawn from the supply discharges the filter capacitor faster
// than the charging path can replenish it. The rail voltage drops ("sags"), which
// alters the clipping headroom, reduces dynamic punch, and creates a natural,
// musical compression and "bloom" as the supply recharges during sustain.
//
// Differential equation:
//   C * dVcap/dt = (V_nominal - Vcap) / Z_psu - I_load
//
// Discretized (forward Euler):
//   Vcap[n+1] = Vcap[n] + dt/C * ((V_nominal - Vcap[n]) / Z_psu - I_load)

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace wdf {

/// Rectifier character for the dynamic power supply.
enum class RectifierType {
    /// Solid-state rectifier (silicon diode bridge or battery) - low impedance, ~1.4V drop.
    SolidState,
    /// Tube rectifier (GZ34, 5U4, 5Y3) - higher impedance, ~10V standing drop, spongy response.
    Tube,
};

class PowerSupply {
public:
    PowerSupply(double nominalVoltage = 9.0,
                double impedance = 10.0,
                double filterCap = 100.0e-6,
                RectifierType rectifier = RectifierType::SolidState,
                double sampleRate = 48000.0)
        : nominalVoltage_(nominalVoltage),
          impedance_(std::max(1.0e-4, impedance)),
          filterCap_(std::max(1.0e-12, filterCap)),
          capVoltage_(nominalVoltage),
          sampleRate_(sampleRate > 0.0 ? sampleRate : 48000.0),
          rectifier_(rectifier) {
        updateDtOverC();
    }

    /// Advance one sample: given the instantaneous load current draw in Amperes,
    /// returns the sagged output voltage seen by the circuit.
    inline double tick(double currentDraw) noexcept {
        // Charging current flowing through Z_psu toward V_nominal
        const double chargeCurrent = (nominalVoltage_ - capVoltage_) / impedance_;
        // Net current entering reservoir capacitance
        const double netCurrent = chargeCurrent - currentDraw;
        capVoltage_ += netCurrent * dtOverC_;

        // Cap voltage cannot exceed nominal (rectifier does not push backwards)
        if (nominalVoltage_ >= 0.0) {
            capVoltage_ = std::clamp(capVoltage_, 0.0, nominalVoltage_);
        } else {
            capVoltage_ = std::clamp(capVoltage_, nominalVoltage_, 0.0);
        }

        const double drop = staticDrop();
        if (nominalVoltage_ >= 0.0) {
            return std::max(0.0, capVoltage_ - drop);
        } else {
            return std::min(0.0, capVoltage_ + drop);
        }
    }

    /// Steady-state output voltage under zero load (accounting for static rectifier drop).
    double steadyStateVoltage() const noexcept {
        const double drop = staticDrop();
        if (nominalVoltage_ >= 0.0) {
            return std::max(0.0, nominalVoltage_ - drop);
        } else {
            return std::min(0.0, nominalVoltage_ + drop);
        }
    }

    /// Reset reservoir capacitor to no-load nominal state.
    void reset() noexcept {
        capVoltage_ = nominalVoltage_;
    }

    /// Update sample rate and recompute dt/C coefficient.
    void setSampleRate(double fs) noexcept {
        if (fs > 0.0) {
            sampleRate_ = fs;
            updateDtOverC();
        }
    }

    /// Current capacitor voltage (V).
    double voltage() const noexcept { return capVoltage_; }

    /// Nominal open-circuit voltage (V).
    double nominalVoltage() const noexcept { return nominalVoltage_; }

    /// Total internal supply impedance (ohms).
    double impedance() const noexcept { return impedance_; }

    /// Filter capacitance (Farads).
    double filterCap() const noexcept { return filterCap_; }

    /// Rectifier type.
    RectifierType rectifier() const noexcept { return rectifier_; }

    void setNominalVoltage(double v) noexcept { nominalVoltage_ = v; }
    void setImpedance(double z) noexcept {
        impedance_ = std::max(1.0e-4, z);
    }
    void setFilterCap(double c) noexcept {
        filterCap_ = std::max(1.0e-12, c);
        updateDtOverC();
    }
    void setRectifier(RectifierType r) noexcept { rectifier_ = r; }

    /// Manually override static drop. Pass negative to revert to rectifier standard.
    void setStaticDropOverride(double d) noexcept { staticDropOverride_ = d; }

    double staticDrop() const noexcept {
        if (staticDropOverride_ >= 0.0)
            return staticDropOverride_;
        switch (rectifier_) {
            case RectifierType::Tube: return 10.0;
            case RectifierType::SolidState: return 1.4;
        }
        return 1.4;
    }

private:
    void updateDtOverC() noexcept {
        dtOverC_ = 1.0 / (sampleRate_ * filterCap_);
    }

    double nominalVoltage_;
    double impedance_;
    double filterCap_;
    double capVoltage_;
    double dtOverC_ = 0.0;
    double sampleRate_ = 48000.0;
    RectifierType rectifier_ = RectifierType::SolidState;
    double staticDropOverride_ = -1.0;
};

}  // namespace wdf
