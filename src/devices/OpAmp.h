#pragma once

// Op-amp models for LiveWDF.
//
// This is a port of PedalKernel's op-amp element
// (`pedalkernel-rt/src/elements/nonlinear/opamp.rs`). The physical model is a
// voltage-controlled voltage source with:
//
//   * finite open-loop DC gain A0,
//   * a dominant pole at f_p = GBW / A0 (so the closed-loop bandwidth is
//     GBW / closed-loop gain),
//   * output slew-rate limiting,
//   * asymmetric output saturation at the supply rails,
//   * optional tanh soft clipping from feedback diodes,
//   * finite output impedance.
//
// Two consumers share the same model:
//
//   * `OpAmpRoot` is the faithful one-port WDF root from PedalKernel: the
//     closed-loop gain is supplied by the caller and the GBW/slew/rail chain
//     is applied to the output wave. It is useful on its own and is what the
//     model-level tests drive.
//
//   * The engine uses `OpAmpModel` through a three-port VCVS root (output,
//     non-inverting input, inverting input referenced to the negative supply).
//     The real feedback network therefore stays in the WDF tree and sets the
//     closed-loop gain from Rf/Ri, instead of a precomputed scalar. The
//     dominant pole is integrated in the engine's state update, which is the
//     same f_p = GBW/A0 physics expressed in the time domain.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

#include "devices/PowerSupply.h"

namespace wdf {

inline constexpr double kOpAmpPi = 3.14159265358979323846;

enum class OpAmpType {
    Generic,
    Tl071,
    Tl072,
    Tl082,
    Jrc4558,
    Rc4558,
    Lm308,
    Lm741,
    Ne5532,
    Ca3080,  // OTA (transconductance amplifier); modelled as a slow VCVS here
    Op07,
};

inline OpAmpType opAmpTypeFromString(std::string_view name) {
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    if (lower == "generic") return OpAmpType::Generic;
    if (lower == "tl071") return OpAmpType::Tl071;
    if (lower == "tl072") return OpAmpType::Tl072;
    if (lower == "tl082") return OpAmpType::Tl082;
    if (lower == "jrc4558" || lower == "4558") return OpAmpType::Jrc4558;
    if (lower == "rc4558") return OpAmpType::Rc4558;
    if (lower == "lm308" || lower == "lm308n") return OpAmpType::Lm308;
    if (lower == "lm741" || lower == "ua741") return OpAmpType::Lm741;
    if (lower == "ne5532" || lower == "njm5532" || lower == "5532") return OpAmpType::Ne5532;
    if (lower == "ca3080" || lower == "lm3080") return OpAmpType::Ca3080;
    if (lower == "op07" || lower == "op07c") return OpAmpType::Op07;
    return OpAmpType::Generic;
}

inline const char* opAmpTypeName(OpAmpType type) {
    switch (type) {
        case OpAmpType::Generic: return "Generic";
        case OpAmpType::Tl071: return "TL071";
        case OpAmpType::Tl072: return "TL072";
        case OpAmpType::Tl082: return "TL082";
        case OpAmpType::Jrc4558: return "JRC4558";
        case OpAmpType::Rc4558: return "RC4558";
        case OpAmpType::Lm308: return "LM308";
        case OpAmpType::Lm741: return "LM741";
        case OpAmpType::Ne5532: return "NE5532";
        case OpAmpType::Ca3080: return "CA3080";
        case OpAmpType::Op07: return "OP07";
    }
    return "Generic";
}

/// Every canonical model name, in the order the editor offers them. One table,
/// so a picker entry cannot name a card `opAmpTypeFromString` does not resolve.
/// Spelled the way the parts are printed on a package: these are part numbers,
/// not identifiers, and lower-case "tl072" read like a typo beside "2N7000".
///
/// **`Generic` is deliberately absent.** It used to open this list, but the
/// editor's unnamed card is now the ideal op-amp -- a separate part type with
/// no supply pins, not a model row. The enum keeps `Generic` (and
/// `opAmpTypeFromString` still resolves "Generic") so documents written when
/// it was offered keep compiling; the picker simply no longer offers it.
inline constexpr const char* kOpAmpTypeNames[] = {
    "TL071", "TL072", "TL082", "JRC4558", "RC4558",
    "LM308", "LM741", "NE5532", "CA3080", "OP07"};
inline constexpr int kNumOpAmpTypes =
    static_cast<int>(sizeof(kOpAmpTypeNames) / sizeof(kOpAmpTypeNames[0]));

/// Absolute maximum supply voltage (V) for the package, not an operating point.
inline double opAmpSupplyMax(OpAmpType type) {
    switch (type) {
        case OpAmpType::Ne5532:
        case OpAmpType::Op07:
            return 44.0;
        default:
            return 36.0;
    }
}

inline bool opAmpIsOTA(OpAmpType type) { return type == OpAmpType::Ca3080; }

/// Static model card. All of these are the PedalKernel defaults
/// (`OpAmpModel::tl072`, `lm308`, ...), which in turn match the datasheets for
/// the parameters a guitar pedal can hear.
struct OpAmpModel {
    OpAmpType type = OpAmpType::Generic;

    /// Open-loop DC gain (dimensionless). TL072 ~ 200k (106 dB), LM308 ~ 300k.
    double openLoopGain = 100000.0;
    /// Gain-bandwidth product (Hz). TL072 ~ 3 MHz, LM308 ~ 1 MHz.
    double gbw = 1.0e6;
    /// Slew rate in V/us. TL072 ~ 13, LM308 ~ 0.3.
    double slewRate = 1.0;
    /// Maximum positive output swing above zero (V).
    double vRailPos = 12.0;
    /// Maximum negative output swing below zero (V).
    double vRailNeg = 12.0;
    /// Output impedance (ohm). 50-200 ohm for a voltage-feedback op-amp.
    double outputImpedance = 75.0;
    /// Output stage capacitance (F). Carried for model fidelity and future
    /// use in a full state-space path; the WDF root's Ro is the DC value.
    double outputCapacitance = 20e-12;
    /// Asymmetric soft rail saturation flag.
    /// When false (default), output rails use piecewise hard clipping std::clamp(v, lo, hi).
    /// When true, output rails use PedalKernel's smooth asymmetric tanh saturation:
    ///   v > 0: vRailPos * tanh(v / vRailPos)
    ///   v < 0: -vRailNeg * tanh(-v / vRailNeg)
    /// providing continuous derivatives everywhere and natural analog warmth.
    bool softRails = false;

    OpAmpModel() = default;
    explicit OpAmpModel(OpAmpType t) : type(t) { *this = fromType(t); }

    /// Build the canonical card for `t`.
    static OpAmpModel fromType(OpAmpType t) {
        OpAmpModel m;
        m.type = t;
        switch (t) {
            case OpAmpType::Generic:
                m.openLoopGain = 100000.0;
                m.gbw = 1.0e6;
                m.slewRate = 1.0;
                m.outputImpedance = 75.0;
                m.outputCapacitance = 20e-12;
                break;
            case OpAmpType::Tl071:
            case OpAmpType::Tl072:
                m.openLoopGain = 200000.0;
                m.gbw = 3.0e6;
                m.slewRate = 13.0;
                m.outputImpedance = 75.0;
                m.outputCapacitance = 20e-12;
                break;
            case OpAmpType::Tl082:
                m.openLoopGain = 200000.0;
                m.gbw = 4.0e6;
                m.slewRate = 13.0;
                m.outputImpedance = 75.0;
                m.outputCapacitance = 20e-12;
                break;
            case OpAmpType::Jrc4558:
                m.openLoopGain = 100000.0;
                m.gbw = 3.0e6;
                m.slewRate = 1.7;
                m.outputImpedance = 75.0;
                m.outputCapacitance = 20e-12;
                break;
            case OpAmpType::Rc4558:
                m.openLoopGain = 100000.0;
                m.gbw = 3.0e6;
                m.slewRate = 1.5;
                m.outputImpedance = 75.0;
                m.outputCapacitance = 20e-12;
                break;
            case OpAmpType::Lm308:
                m.openLoopGain = 300000.0;
                m.gbw = 1.0e6;
                m.slewRate = 0.3;
                m.outputImpedance = 200.0;
                m.outputCapacitance = 30e-12;
                break;
            case OpAmpType::Lm741:
                m.openLoopGain = 200000.0;
                m.gbw = 1.0e6;
                m.slewRate = 0.5;
                m.outputImpedance = 75.0;
                m.outputCapacitance = 20e-12;
                break;
            case OpAmpType::Ne5532:
                m.openLoopGain = 100000.0;
                m.gbw = 10.0e6;
                m.slewRate = 9.0;
                m.outputImpedance = 50.0;
                m.outputCapacitance = 15e-12;
                break;
            case OpAmpType::Ca3080:
                // OTA: the WDF model treats it as a VCVS with the published
                // small-signal bandwidth. Its transconductance pin is not
                // exposed, which is a known limitation rather than a silent
                // idealization.
                m.openLoopGain = 100000.0;
                m.gbw = 2.0e6;
                m.slewRate = 50.0;
                m.outputImpedance = 75.0;
                m.outputCapacitance = 20e-12;
                break;
            case OpAmpType::Op07:
                m.openLoopGain = 400000.0;
                m.gbw = 0.6e6;
                m.slewRate = 0.3;
                m.outputImpedance = 60.0;
                m.outputCapacitance = 25e-12;
                break;
        }
        return m;
    }

    bool valid() const {
        return openLoopGain > 0.0 && gbw > 0.0 && slewRate >= 0.0 &&
               outputImpedance > 0.0;
    }

    /// Set symmetric signal-swing rails (V around zero).
    void setVMax(double vMax) {
        const double v = std::max(0.5, vMax);
        vRailPos = v;
        vRailNeg = v;
    }

    /// Set asymmetric signal-swing rails (V around zero). A zero swing is
    /// meaningful: an op-amp pinned at a rail can only move away from it.
    void setVRails(double pos, double neg) {
        vRailPos = std::max(0.0, pos);
        vRailNeg = std::max(0.0, neg);
    }

    /// Output limits in the model's port frame.
    double railHigh() const { return vRailPos; }
    double railLow() const { return -vRailNeg; }

    void enableSoftRails(bool enable = true) { softRails = enable; }

    /// Apply rail saturation (piecewise hard clamp or smooth asymmetric tanh).
    double applyRails(double v) const {
        if (softRails) {
            if (v > 0.0 && vRailPos < 1.0e9) {
                return vRailPos * std::tanh(v / vRailPos);
            } else if (v < 0.0 && vRailNeg < 1.0e9) {
                return -vRailNeg * std::tanh(-v / vRailNeg);
            }
            return v;
        }
        return std::clamp(v, railLow(), railHigh());
    }

    /// Exact derivative of rail saturation with respect to input voltage v.
    double railDerivative(double v) const {
        if (softRails) {
            if (v > 0.0 && vRailPos < 1.0e9) {
                const double t = std::tanh(v / vRailPos);
                return 1.0 - t * t;
            } else if (v < 0.0 && vRailNeg < 1.0e9) {
                const double t = std::tanh(-v / vRailNeg);
                return 1.0 - t * t;
            }
            return 1.0;
        }
        if (v > vRailPos || v < -vRailNeg)
            return 0.0;
        return 1.0;
    }

    /// GBW single-pole coefficient for the closed-loop gain `gain`:
    ///   f_cl = GBW / max(|gain|, 1)
    ///   g    = 1 - exp(-2*pi*f_cl/fs)
    /// A closed-loop bandwidth at or above Nyquist is transparent.
    double gbwCoeff(double gain, double sampleRate) const {
        const double g = std::max(std::abs(gain), 1.0);
        const double fcl = gbw / g;
        if (fcl >= sampleRate * 0.5)
            return 1.0;
        return 1.0 - std::exp(-2.0 * kOpAmpPi * fcl / sampleRate);
    }

    /// Static open-loop source value (used by the DC solve). `vDiff` is
    /// V(non-inverting) - V(inverting), both in the op-amp reference frame.
    double evalStatic(double vDiff) const {
        return applyRails(openLoopGain * vDiff);
    }
};

/// Runtime state for PedalKernel's GBW lowpass and dV/dt slew clamp.
/// This is the single implementation of those two primitives for the one-port
/// root; the engine's three-port path integrates the same pole implicitly.
struct OpAmpFxState {
    double gbwCoeff = 1.0;
    double gbwState = 0.0;
    double maxDv = std::numeric_limits<double>::infinity();
    double prevOut = 0.0;

    void configure(const OpAmpModel& model, double gain, double sampleRate) {
        gbwCoeff = model.gbwCoeff(gain, sampleRate);
        maxDv = model.slewRate * 1.0e6 / sampleRate;
    }

    void reset() {
        gbwState = 0.0;
        prevOut = 0.0;
    }

    /// Single-pole GBW lowpass. Advances the filter state.
    double gbwStep(double sample) {
        const double out = gbwCoeff * sample + (1.0 - gbwCoeff) * gbwState;
        gbwState = out;
        return out;
    }

    /// dV/dt clamp toward `target` from the previous real output.
    double slewStep(double target) {
        const double dv = target - prevOut;
        double out;
        if (dv > maxDv)
            out = prevOut + maxDv;
        else if (dv < -maxDv)
            out = prevOut - maxDv;
        else
            out = target;
        prevOut = out;
        return out;
    }
};

/// Topology of the one-port `OpAmpRoot`.
enum class OpAmpMode {
    Inverting,    // Vout = -gain * Vin, input is the incident wave
    NonInverting  // Vout = +gain * Vp, input set with setVp()
};

/// Faithful port of PedalKernel's one-port WDF op-amp root.
///
/// `process(a)` returns the reflected wave `b = 2*Vout - a`. The output is
/// calculated from the mode, passed through the GBW lowpass, optional diode
/// soft clip, the asymmetric supply clamp and finally the slew clamp -- the
/// exact ordering in PedalKernel's `OpAmpRoot::process`.
///
/// The voltage rails are absolute, zero-referenced clamps applied as
/// `[-vRailNeg, +vRailPos]`; they are not offsets around an output bias.
struct OpAmpRoot {
    OpAmpModel model;
    OpAmpMode mode = OpAmpMode::NonInverting;
    double gain = 1.0;
    double vp = 0.0;
    double sampleRate = 48000.0;
    bool softClip = false;
    double softClipV = 0.5;
    OpAmpFxState fx;

    OpAmpRoot() { configure(); }
    explicit OpAmpRoot(const OpAmpModel& m) : model(m) { configure(); }

    static OpAmpRoot unityGain(const OpAmpModel& m) {
        OpAmpRoot r(m);
        r.mode = OpAmpMode::NonInverting;
        r.gain = 1.0;
        return r;
    }
    static OpAmpRoot inverting(const OpAmpModel& m, double g) {
        OpAmpRoot r(m);
        r.mode = OpAmpMode::Inverting;
        r.gain = std::abs(g);
        r.configure();
        return r;
    }
    static OpAmpRoot nonInverting(const OpAmpModel& m, double g) {
        OpAmpRoot r(m);
        r.mode = OpAmpMode::NonInverting;
        r.gain = std::abs(g);
        r.configure();
        return r;
    }

    void configure() { fx.configure(model, gain, sampleRate); }

    void setVp(double v) { vp = v; }
    double vpValue() const { return vp; }

    void setGain(double g) {
        gain = std::abs(g);
        configure();
    }

    void setSampleRate(double fs) {
        if (fs > 0.0) {
            sampleRate = fs;
            configure();
        }
    }

    void setVRails(double pos, double neg) { model.setVRails(pos, neg); }
    void setVMax(double vMax) { model.setVMax(vMax); }
    double vMax() const { return std::min(model.vRailPos, model.vRailNeg); }

    void enableSoftRails(bool enable = true) { model.enableSoftRails(enable); }

    bool sagEnabled = false;
    PowerSupply psuPos;
    PowerSupply psuNeg;

    void enablePowerSag(double impedance = 100.0, double filterCap = 47.0e-6,
                        RectifierType rectifier = RectifierType::SolidState) {
        sagEnabled = true;
        psuPos = PowerSupply(model.vRailPos, impedance, filterCap, rectifier, sampleRate);
        psuNeg = PowerSupply(model.vRailNeg, impedance, filterCap, rectifier, sampleRate);
    }

    void disablePowerSag() {
        sagEnabled = false;
    }

    void tickPowerSag(double iPos, double iNeg) {
        if (sagEnabled) {
            model.setVRails(psuPos.tick(iPos), psuNeg.tick(iNeg));
        }
    }

    void setSoftClip(double diodeVf) {
        softClip = true;
        softClipV = std::max(0.1, diodeVf);
    }
    void clearSoftClip() { softClip = false; }

    bool isNonInverting() const { return mode == OpAmpMode::NonInverting; }

    /// One WDF root evaluation. `a` is the incident wave, `rp` is ignored
    /// (the root's port resistance is carried by the surrounding tree).
    double process(double a, double rp = 0.0) {
        double vOut = (mode == OpAmpMode::Inverting) ? -gain * (a * 0.5)
                                                     : gain * vp;

        vOut = fx.gbwStep(vOut);
        if (softClip)
            vOut = softClipV * std::tanh(vOut / softClipV);

        if (sagEnabled) {
            const double rLoad = (rp > 0.0) ? rp : model.outputImpedance;
            const double iOut = (vOut - (a * 0.5)) / std::max(1.0, rLoad);
            tickPowerSag(std::max(0.0, iOut), std::max(0.0, -iOut));
        }

        vOut = model.applyRails(vOut);
        vOut = fx.slewStep(vOut);
        return 2.0 * vOut - a;
    }

    /// Voltage-source value for a diode-pair stage driven by this op-amp:
    /// gain, GBW rolloff, rail clamp and slew, but no soft clipping because
    /// the diode NR solver performs the clipping. Mirrors PedalKernel's
    /// `compute_vs_voltage`.
    double computeVsVoltage(double input) {
        double vOut = gain * input;
        vOut = fx.gbwStep(vOut);
        vOut = model.applyRails(vOut);
        vOut = fx.slewStep(vOut);
        return vOut;
    }

    void reset() {
        fx.reset();
        vp = 0.0;
        if (sagEnabled) {
            psuPos.reset();
            psuNeg.reset();
        }
    }
};

/// Engine-facing op-amp source law.
///
/// The engine represents the op-amp as a Thevenin source behind
/// `outputImpedance`:
///
///   i_out = (v_out - vSource) / Ro
///   vSource = A0 * (v_pos - v_neg)                 (DC solve)
///   vSource = (vPrev + alpha*A0*vDiff) / (1+alpha) (transient)
///
/// The transient form is backward Euler on the dominant pole
/// `dV/dt = 2*pi*f_p*(A0*vDiff - V)` with `f_p = gbw/A0`, hence
/// `alpha = 2*pi*(gbw/A0)/fs`. In steady state it gives exactly `A0*vDiff`;
/// at audio frequencies it gives the datasheet's open-loop rolloff, and with
/// the feedback network in the WDF tree it produces the familiar
/// `GBW/closedLoopGain` closed-loop bandwidth.
///
/// Slew and supply rails are piecewise clamps on the source voltage. When a
/// clamp is active the derivative with respect to `vDiff` is zero, so the
/// Newton Jacobian remains exact for the active piece.
struct OpAmpEngineLaw {
    /// Which piece of the static output clamp a differential input selects.
    /// A zero derivative on a rail is correct for the active piece, but a
    /// Newton step is only meaningful on that piece; callers use this to keep
    /// a line search from accepting a trial that silently switches pieces.
    /// Returns -1 on the low rail, 0 in the linear region, +1 on the high rail.
    static int staticRailRegion(const OpAmpModel& model, double vDiff) noexcept {
        const double raw = model.openLoopGain * vDiff;
        if (raw > model.railHigh())
            return 1;
        if (raw < model.railLow())
            return -1;
        return 0;
    }

    static double sourceVoltage(const OpAmpModel& model, double vDiff,
                                double vPrev, double alpha, double maxDv,
                                bool staticMode, double* dSourceDvDiff) {
        double vSource;
        double derivative;
        if (staticMode) {
            vSource = model.openLoopGain * vDiff;
            derivative = model.openLoopGain;
        } else {
            const double a = std::clamp(alpha, 0.0, 1.0);
            const double denom = 1.0 + a;
            vSource = (vPrev + a * model.openLoopGain * vDiff) / denom;
            derivative = a * model.openLoopGain / denom;

            if (std::isfinite(maxDv)) {
                if (vSource > vPrev + maxDv) {
                    vSource = vPrev + maxDv;
                    derivative = 0.0;
                } else if (vSource < vPrev - maxDv) {
                    vSource = vPrev - maxDv;
                    derivative = 0.0;
                }
            }
        }

        if (model.softRails) {
            const double dRail = model.railDerivative(vSource);
            vSource = model.applyRails(vSource);
            derivative *= dRail;
        } else {
            const double hi = model.railHigh();
            const double lo = model.railLow();
            if (vSource > hi) {
                vSource = hi;
                derivative = 0.0;
            } else if (vSource < lo) {
                vSource = lo;
                derivative = 0.0;
            }
        }

        if (dSourceDvDiff)
            *dSourceDvDiff = derivative;
        return vSource;
    }

    /// Backward-Euler pole coefficient for the dominant pole at
    /// `f_p = gbw / openLoopGain`.
    static double alpha(const OpAmpModel& model, double sampleRate) {
        const double fp = model.gbw / std::max(model.openLoopGain, 1.0);
        return 2.0 * kOpAmpPi * fp / sampleRate;
    }

    /// Maximum source-voltage change per sample from the slew rate (V/us).
    static double maxDv(const OpAmpModel& model, double sampleRate) {
        return model.slewRate * 1.0e6 / sampleRate;
    }
};

}  // namespace wdf
