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

#include "devices/Diode.h"
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
    M5218Al,
    Tl022,
    Mc33174,
    Mc33179,
    Ba728N,
    Rc4558ClassAB,
    Njm4558ClassAB,
    Tl082ClassAB,
    Lm741ClassAB,
    Tl061,
    Op275,
    Api2520,
    Lm324,
    Mci2001,
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
    if (lower == "op275") return OpAmpType::Op275;
    if (lower == "mci2001" || lower == "mci-2001") return OpAmpType::Mci2001;
    if (lower == "api2520") return OpAmpType::Api2520;
    if (lower == "lm324" || lower == "lm324-n") return OpAmpType::Lm324;
    if (lower == "m5218al" || lower == "m5218a" || lower == "m5218ap" || lower == "m5218afp")
        return OpAmpType::M5218Al;
    if (lower == "tl022" || lower == "tl022c") return OpAmpType::Tl022;
    if (lower == "mc33174") return OpAmpType::Mc33174;
    if (lower == "mc33179") return OpAmpType::Mc33179;
    if (lower == "ba728n" || lower == "ba728" || lower == "ba728f") return OpAmpType::Ba728N;
    if (lower == "rc4558-class-ab") return OpAmpType::Rc4558ClassAB;
    if (lower == "njm4558-class-ab") return OpAmpType::Njm4558ClassAB;
    if (lower == "tl082-class-ab") return OpAmpType::Tl082ClassAB;
    if (lower == "lm741-class-ab") return OpAmpType::Lm741ClassAB;
    if (lower == "tl061" || lower == "tl061c") return OpAmpType::Tl061;
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
        case OpAmpType::Op275: return "OP275";
        case OpAmpType::Mci2001: return "MCI2001";
        case OpAmpType::Api2520: return "API2520";
        case OpAmpType::Lm324: return "LM324";
        case OpAmpType::M5218Al: return "M5218AL";
        case OpAmpType::Tl022: return "TL022";
        case OpAmpType::Mc33174: return "MC33174";
        case OpAmpType::Mc33179: return "MC33179";
        case OpAmpType::Ba728N: return "BA728N";
        case OpAmpType::Rc4558ClassAB: return "RC4558-Class-AB";
        case OpAmpType::Njm4558ClassAB: return "NJM4558-Class-AB";
        case OpAmpType::Tl082ClassAB: return "TL082-Class-AB";
        case OpAmpType::Lm741ClassAB: return "LM741-Class-AB";
        case OpAmpType::Tl061: return "TL061";
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
    "LM308", "LM741", "NE5532", "CA3080", "OP07",
    "M5218AL", "TL022", "MC33174", "MC33179", "BA728N",
    "RC4558-Class-AB", "NJM4558-Class-AB", "TL082-Class-AB", "LM741-Class-AB", "TL061", "OP275", "API2520",
    "LM324", "MCI2001"};
inline constexpr int kNumOpAmpTypes =
    static_cast<int>(sizeof(kOpAmpTypeNames) / sizeof(kOpAmpTypeNames[0]));

/// Absolute maximum supply voltage (V) for the package, not an operating point.
inline double opAmpSupplyMax(OpAmpType type) {
    switch (type) {
        case OpAmpType::Ba728N:
            return 18.0;
        case OpAmpType::Mci2001:
            return 50.0;  // Harris HA-911 V+ to V− absolute maximum.
        case OpAmpType::Api2520:
            return 40.0;
        case OpAmpType::Lm324:
            return 32.0;
        case OpAmpType::Ne5532:
        case OpAmpType::Op07:
        case OpAmpType::Op275:
        case OpAmpType::Mc33174:
            return 44.0;
        default:
            return 36.0;
    }
}

inline bool opAmpIsOTA(OpAmpType type) { return type == OpAmpType::Ca3080; }

/// Static model card. The original constants are from PedalKernel. Added
/// pedal-specific cards use typical manufacturer data for gain, bandwidth and
/// slew rate. Output impedance and capacitance retain the generic assumptions
/// unless the card specifies them.
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
    /// Solver-only state on an Engine's private model copy: the linear
    /// output conductance is already present in the passive network.
    bool outputResistanceEmbedded = false;

    double outputVoltageWeight() const noexcept {
        return outputResistanceEmbedded ? 0.0 : 1.0;
    }

    double sourceVoltageFromSolverCurrent(double voltage, double current) const noexcept {
        return outputVoltageWeight() * voltage - outputImpedance * current;
    }

    double physicalOutputCurrent(double voltage, double current) const noexcept {
        return outputResistanceEmbedded ? current + voltage / outputImpedance : current;
    }
    /// The optional symmetric class-AB stage limits the sourced and sunk
    /// current. A zero limit selects the original resistive output law.
    double outputCurrentLimit = 0.0;
    double outputJunctionIs = 0.0;

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
            case OpAmpType::Mci2001:
                // MCI-2001 data sheet: 90 dB open-loop, 50 dB closed-loop
                // through 20 kHz on ±24 V supplies, +24 dBm into 600 Ω.
                // Use the corresponding single-pole gain-bandwidth product.
                // The Harris HA-911 selection guide gives a 2.3 V/µs slew rate.
                // https://funkwerkes.com/web/wp-content/techdocs/MixedProAudio/MCI-2001-Op-Amp-Replacement.pdf
                m.openLoopGain = 31622.7766;
                m.gbw = 6.324555e6;
                m.slewRate = 2.3;
                // Correct the measured loaded swing for the model's estimated Ro.
                m.vRailPos = m.vRailNeg = 17.36 * (1.0 + m.outputImpedance / 600.0);
                break;
            case OpAmpType::Api2520:
                // API's 2520 data sheet gives >110 dB, 50 MHz and a 40 kHz
                // full-power bandwidth at a rated 7.75 Vrms (about 2.75 V/us).
                // The output resistance is a compact-model approximation.
                // https://www.steampoweredradio.com/pdf/api/manuals/api%201972%20schematics.pdf
                m.openLoopGain = 316227.766;
                m.gbw = 50.0e6;
                m.slewRate = 2.75;
                m.outputImpedance = 5.0;
                m.vRailPos = m.vRailNeg = 11.0;
                break;
            case OpAmpType::Op275:
                // Analog Devices OP275 Rev. C, ±15 V and 25 °C typical.
                // https://www.analog.com/media/en/technical-documentation/data-sheets/OP275.pdf
                m.openLoopGain = 250000.0;
                m.gbw = 9.0e6;
                m.slewRate = 22.0;
                m.outputImpedance = 30.0;
                // Compensate the 13.9 V swing specified into 2 kΩ for Ro.
                m.vRailPos = m.vRailNeg = 13.9 * (1.0 + m.outputImpedance / 2000.0);
                break;
            case OpAmpType::Lm324:
                // TI LM324-N, SNOSC16D, nominal gain at 15 V with a >=2 kΩ
                // load. TI's LM324-N product table gives 1 MHz and 0.5 V/µs.
                // https://www.ti.com/lit/ds/symlink/lm324-n.pdf
                // https://www.ti.com/product/LM324-N
                m.openLoopGain = 100000.0;
                m.gbw = 1.0e6;
                m.slewRate = 0.5;
                // Typical light-load headroom on nominal ±15 V rails.
                // Single-supply documents set their own output bounds.
                m.setVRailBounds(-14.995, 13.0);
                break;
            case OpAmpType::Op07:
                m.openLoopGain = 400000.0;
                m.gbw = 0.6e6;
                m.slewRate = 0.3;
                m.outputImpedance = 60.0;
                m.outputCapacitance = 25e-12;
                break;
            case OpAmpType::M5218Al:
                // Mitsubishi M5218AL/P/FP datasheet: 110 dB typical (10^(110/20)).
                // https://ardent-tool.com/datasheets/Mitsubishi_M5218.pdf
                m.openLoopGain = 316227.766;
                m.gbw = 7.0e6;
                m.slewRate = 3.0;
                break;
            case OpAmpType::Tl022:
                // TI TL022C datasheet, typical values at ±15 V and 25 °C.
                // https://www.ti.com/lit/ds/symlink/tl022.pdf
                m.openLoopGain = 10000.0;
                m.gbw = 0.5e6;
                m.slewRate = 0.5;
                break;
            case OpAmpType::Mc33174:
                // onsemi MC33171/72/74 datasheet: 500 V/mV large-signal gain.
                // https://www.onsemi.com/pdf/datasheet/mc33171-d.pdf
                m.openLoopGain = 500000.0;
                m.gbw = 1.8e6;
                m.slewRate = 2.1;
                break;
            case OpAmpType::Mc33179:
                // onsemi MC33178/79 datasheet, typical values at ±15 V.
                // https://www.onsemi.com/pdf/datasheet/mc33178-d.pdf
                m.openLoopGain = 200000.0;
                m.gbw = 5.0e6;
                m.slewRate = 2.0;
                break;
            case OpAmpType::Ba728N:
                // ROHM BA728/BA728F/BA728N datasheet, typical at 12 V.
                // https://www.alldatasheet.com/datasheet-pdf/pdf/35975/ROHM/BA728N.html
                m.openLoopGain = 100000.0;
                m.gbw = 0.7e6;
                m.slewRate = 0.7;
                break;
            // MiSTer-Discrete circuit-compiler@692e777, circuits/parts/
            // rc4558.lib, tl082.lib and ua741.lib. The rail bounds below
            // assume ±15 V; the schematic can set different bounds.
            // https://github.com/jopdorp/MiSTer-Discrete/tree/692e777112e4a3c05a17578d9cfda1ecbc9b98d6/circuits/parts
            case OpAmpType::Rc4558ClassAB:
                m.openLoopGain = 300000.0;
                m.gbw = 3.0e6;
                m.slewRate = 1.7;
                m.setClassABOutput(25e-3, 6e-8, 1.9);
                break;
            case OpAmpType::Njm4558ClassAB:
                m.openLoopGain = 100000.0;
                m.gbw = 3.0e6;
                m.slewRate = 1.0;
                m.setClassABOutput(25e-3, 6e-8, 1.9);
                break;
            case OpAmpType::Tl082ClassAB:
                m.openLoopGain = 200000.0;
                m.gbw = 3.0e6;
                m.slewRate = 13.0;
                m.setClassABOutput(26e-3, 5.8e-8, 1.44);
                break;
            case OpAmpType::Lm741ClassAB:
                m.openLoopGain = 200000.0;
                m.gbw = 1.0e6;
                m.slewRate = 0.5;
                m.setClassABOutput(25e-3, 6e-8, 1.9);
                break;
            case OpAmpType::Tl061:
                // TI SLOS078N, TL061C: 6 V/mV large-signal gain.
                // https://www.ti.com/lit/ds/symlink/tl061.pdf
                m.openLoopGain = 6000.0;
                m.gbw = 1.0e6;
                m.slewRate = 3.5;
                break;
        }
        return m;
    }

    static constexpr double kSteeringDiodeIs = 1e-5;

    void setClassABOutput(double limit, double junctionIs, double headroom) noexcept {
        outputCurrentLimit = limit;
        outputJunctionIs = junctionIs;
        vRailPos = 15.0 - headroom;
        vRailNeg = 15.0 - headroom;
        double conductance = 0.0;
        outputCurrent(0.0, conductance);
        outputImpedance = 1.0 / conductance;
    }

    bool hasClassABOutput() const noexcept { return outputCurrentLimit > 0.0; }

    /// Current into the modeled output pin for V(out) - V(pole). The symmetric
    /// class-AB cards have the same ideality on both steering paths and no
    /// series resistor. This inverts the two diode branches sampled by
    /// MiSTer-Discrete circuitsim/mna.py::_class_b directly, avoiding tables.
    double outputCurrent(double voltageError, double& conductance) const noexcept {
        if (!hasClassABOutput()) {
            conductance = 1.0 / outputImpedance;
            return voltageError * conductance;
        }

        const double totalCurrent = outputCurrentLimit + outputJunctionIs + kSteeringDiodeIs;
        const double ratio = outputJunctionIs / kSteeringDiodeIs;
        const auto branch = [&](double v, double& slope) noexcept {
            const double scaled = ratio * std::exp(std::clamp(v / kThermalVoltage, -700.0, 700.0));
            const double fraction = scaled / (1.0 + scaled);
            const double current = fraction * totalCurrent - outputJunctionIs;
            slope = current > 0.0 && current < outputCurrentLimit
                        ? fraction * (1.0 - fraction) * totalCurrent / kThermalVoltage
                        : 0.0;
            return std::clamp(current, 0.0, outputCurrentLimit);
        };
        double inSlope = 0.0, outSlope = 0.0;
        const double entering = branch(voltageError, inSlope);
        const double leaving = branch(-voltageError, outSlope);
        conductance = inSlope + outSlope;
        return entering - leaving;
    }

    bool valid() const {
        return openLoopGain > 0.0 && gbw > 0.0 && slewRate >= 0.0 &&
               outputImpedance > 0.0 && (!hasClassABOutput() || outputJunctionIs > 0.0);
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

    /// Signed output swing bounds. For example, a single 9 V supply can
    /// have both its lower and upper output limit above circuit ground.
    void setVRailBounds(double low, double high) noexcept {
        vRailPos = high;
        vRailNeg = -low;
        softRails = false;
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

    /// ADI OP275 Rev. C, TPC 4: 60.115 dB open-loop gain at 15.309 kHz
    /// gives a 15.51 MHz audio-band gain product. The same plot crosses
    /// unity near 9 MHz after the high-frequency pole/zero pair.
    double audioGainBandwidth() const noexcept {
        return type == OpAmpType::Op275 ? gbw * (15.51 / 9.0) : gbw;
    }

    /// GBW single-pole coefficient for the closed-loop gain `gain`:
    ///   f_cl = GBW / max(|gain|, 1)
    ///   g    = 1 - exp(-2*pi*f_cl/fs)
    /// A closed-loop bandwidth at or above Nyquist is transparent.
    double gbwCoeff(double gain, double sampleRate) const {
        const double g = std::max(std::abs(gain), 1.0);
        const double fcl = audioGainBandwidth() / g;
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
    /// the audio-band gain product divided by the open-loop DC gain.
    static double alpha(const OpAmpModel& model, double sampleRate) {
        const double fp = model.audioGainBandwidth() / std::max(model.openLoopGain, 1.0);
        return 2.0 * kOpAmpPi * fp / sampleRate;
    }

    /// Maximum source-voltage change per sample from the slew rate (V/us).
    static double maxDv(const OpAmpModel& model, double sampleRate) {
        return model.slewRate * 1.0e6 / sampleRate;
    }
};

}  // namespace wdf
