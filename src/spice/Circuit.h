#pragma once

// Circuit description: the flat element list and the named-node netlist.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "devices/Jfet.h"
#include "devices/Mosfet.h"
#include "devices/Triode.h"
#include "devices/Pentode.h"
#include "devices/Bjt.h"
#include "devices/BucketBrigade.h"
#include "devices/OpAmp.h"
#include "devices/RmsDetector.h"

namespace wdf {

inline constexpr const char* kGroundName = "0";

/// Pi, so the headers do not depend on a platform `M_PI`.
inline constexpr double kPi = 3.14159265358979323846;

// --------------------------------------------------------------------------- //
// Circuit description
// --------------------------------------------------------------------------- //

/// Potentiometer taper curves. `Log` and `RevLog` are the decade audio pair as
/// specified in PedalKernel's `pot_taper.rs`: r = (10^pos - 1)/9 for `Log` (0.24
/// of the wiper's fraction at half rotation, so a slow start and a fast finish)
/// and its exact inverse for `RevLog`. They are *not* Cathodyne's cubic curves,
/// which read 0.875 at half rotation; the same word names a different curve in
/// that engine, so a Mark-style fixture only agrees with Cathodyne on the names.
enum class PotTaper {
    Linear,
    Log,
    RevLog,
};

/// A maker's audio taper: two straight segments meeting at `mid` of the track at half rotation.
inline double twoSegmentTaper(double p, double mid) noexcept {
    return p < 0.5 ? 2.0 * mid * p : mid + (1.0 - mid) * (2.0 * p - 1.0);
}

/// 3-terminal potentiometer (LiveWDF Phase 1e addition).
///
/// In the netlist and engine, a pot expands into two series resistors meeting at
/// `wiper`: `name_1` between `n1` and `wiper`, and `name_2` between `wiper` and
/// `n2`. The position is a parameter in [0, 1]; changing it updates the two
/// resistances according to the taper and re-derives the R-type transfer in ~25 us
/// without reallocating buffers or changing the network topology.
struct Pot {
    static constexpr double kEps = 1e-4;
    std::string n1, wiper, n2;
    double totalResistance = 100e3;
    double position = 0.5;
    PotTaper taper = PotTaper::Linear;
    std::string name;
    double taperMid = 0.0;  // fraction at half rotation of a Log/RevLog track; 0 keeps (10^p - 1)/9

    /// Calculate the two resistance values (r1 between n1 and wiper, r2 between wiper and n2)
    /// for a given position and taper, with epsilon clamping to keep conductances finite.
    static std::pair<double, double> resistances(double totalR, double pos, PotTaper t,
                                                 double mid = 0.0) noexcept {
        double p = std::clamp(pos, 0.0, 1.0);
        double r = p;
        switch (t) {
            case PotTaper::Linear:
                r = p;
                break;
            case PotTaper::Log:
                r = mid > 0.0 ? twoSegmentTaper(p, mid) : (std::pow(10.0, p) - 1.0) / 9.0;
                break;
            case PotTaper::RevLog:
                r = 1.0 - (mid > 0.0 ? twoSegmentTaper(1.0 - p, mid) : (std::pow(10.0, 1.0 - p) - 1.0) / 9.0);
                break;
        }
        r = std::clamp(r, kEps, 1.0 - kEps);
        return {(1.0 - r) * totalR, r * totalR};
    }
};

/// A 2-terminal variable resistor (rheostat) with a taper curve, as in guitar amp
/// tone stacks (e.g. Cathodyne's `VarResElement`). Expands at parse time into a named
/// resistor, adjusted at runtime via `EngineCompile::setVarResPosition`.
struct VarRes {
    static constexpr double kEps = 1e-4;
    std::string a, b;
    double maxResistance = 0.0;
    double position = 0.5;
    PotTaper taper = PotTaper::Linear;
    std::string name;
    double taperMid = 0.0;

    static double resistance(double maxR, double pos, PotTaper taper, double mid = 0.0) {
        double p = std::clamp(pos, 0.0, 1.0);
        double r = p;
        switch (taper) {
            case PotTaper::Linear:
                r = p;
                break;
            case PotTaper::Log:
                r = mid > 0.0 ? twoSegmentTaper(p, mid) : (std::pow(10.0, p) - 1.0) / 9.0;
                break;
            case PotTaper::RevLog:
                r = 1.0 - (mid > 0.0 ? twoSegmentTaper(1.0 - p, mid) : (std::pow(10.0, 1.0 - p) - 1.0) / 9.0);
                break;
        }
        r = std::clamp(r, kEps, 1.0);
        return r * maxR;
    }
};

/// Light-dependent resistor driven by the voltage across a lamp/LED's current
/// sense resistor. The sense inputs impose no load on the light source.
struct PhotoresistorControl {
    std::string sensePos, senseNeg;
    double brightOhms = 4000.0;
    double referenceVolts = 1.0;
    double gamma = 1.0;
    double attackSeconds = 0.002;
    double releaseSeconds = 0.020;
    // A gas lamp strikes at one voltage and goes out at a lower one; zero leaves the cell analog.
    double strikeVolts = 0.0, extinguishVolts = 0.0;

    double illumination(double volts) const {
        return std::pow(std::clamp(volts / referenceVolts, 0.0, 1.0), gamma);
    }

    double targetConductance(double volts, double darkOhms) const {
        const double dark = 1.0 / darkOhms;
        return dark + (1.0 / brightOhms - dark) * illumination(volts);
    }
};

/// Differential control of an inverting current-output VCA.
struct VcaControl {
    std::string control, reference;
    double fullGainVolts = 1.0;
    bool blackmer = false;

    static constexpr double minimumGain = 1e-5;
    static constexpr double maximumGain = 1.6;

    // THAT2180 datasheet equation 2 at 25 °C. Bound the exponential outside
    // the characterized control range to keep out-of-range patches finite.
    static constexpr double blackmerVoltsPerDb = 0.0061;
    static constexpr double blackmerMinimumDb = -110.0;
    static constexpr double blackmerMaximumDb = 40.0;

    // A dbx 200 steers the bases of a single complementary log/antilog
    // pair directly. The 2180 has a different control-port scale.
    double logVoltsPerDb = blackmerVoltsPerDb;

    double initialGain() const noexcept { return blackmer ? 1.0 : minimumGain; }

    double normalizedGain(double volts) const noexcept {
        if (blackmer) {
            const double db = std::clamp(volts / logVoltsPerDb,
                                         blackmerMinimumDb, blackmerMaximumDb);
            return std::exp2(db * 0.16609640474436812); // log2(10) / 20
        }
        return std::clamp(volts / fullGainVolts, minimumGain, maximumGain);
    }
};

/// A CA3080's matched-pair differential law. Sense the current in series with
/// its bias diode, and mirror the collector difference to the output port.
struct OtaControl {
    std::string bias, biasDiode;
    static constexpr double senseOhms = 100.0;
    static constexpr double pairVolts = 2.0 * 0.02585;
    static constexpr double transportGain = 125.0 / 126.0;
    static constexpr double minimumCurrent = 1e-12;

    static double slopeResistance(double senseVolts) noexcept {
        const double biasAmps = std::max(senseVolts / senseOhms, minimumCurrent);
        return pairVolts / (transportGain * biasAmps);
    }
};

/// A dynamic audio-rate voltage/control-modulated resistor sitting at the R-type
/// multiport root. Its resistance can be modulated every audio sample with zero
/// matrix rebuild overhead via closed-form root division: `i = vOc / (R + Z_rr)`.
struct ControlledRes {
    std::string a, b;
    double initialOhms = 1e3;
    std::string name;
    std::optional<PhotoresistorControl> photo;
    std::optional<VcaControl> vca;
    std::optional<OtaControl> ota = std::nullopt;
    // Signed 2*Iq*Rsense for either half of the dbx 200 class-AB cell.
    // Zero selects the ordinary full-wave controlled resistance.
    double dbx200Crossover = 0.0;
};

struct Res {
    std::string a, b;
    double value = 0.0;
    /// Netlist designator (`R275`), empty for the synthetic ones `addTube`
    /// creates. Only used to *address* a component -- nothing in the solve reads
    /// it -- but it is what lets a control (a pot) name the resistor it drives.
    std::string name;
};

struct Cap {
    std::string a, b;
    double value = 0.0;
    std::string name;
    /// Optional reset seed for self-starting oscillators and other charged
    /// capacitors. Voltage is measured from a to b.
    std::optional<double> initialVolts = std::nullopt;
};

/// An inductor (LiveWDF phase-1 addition). In the time domain it becomes a
/// companion conductance `T/(2L)` with a sign-flipped state (`b = -state`,
/// the wave-dual of the capacitor); at DC it is a short (see `kIndDcR`).
struct Ind {
    std::string a, b;
    double value = 0.0;
    std::string name;
};

/// An ideal current source (LiveWDF phase-1 addition): `value` amperes flow
/// from `a` through the source to `b`, so the element draws `+value` at node
/// `a`. Handled as a *fixed-current root port* -- an exact unit-current
/// injection column, not a stiff conductance, so there is no parasitic load.
struct Isrc {
    std::string a, b;
    double dc = 0.0;
    double ac = 0.0;
    std::vector<double> sin;  // raw SIN(io ia freq td theta phase) args, if any
    std::string name;
};

/// DC resistance used to model an inductor at an operating point. An ideal
/// inductor is a short; a stiff resistor keeps the DC solve linear and keeps
/// the inductor current recoverable (numerically it is exact to well below the
/// 5e-4 V verification threshold for bias currents up to ~1 A). Replacing this
/// with an exact node-merge constraint is a later increment (see PROGRESS.md).
inline constexpr double kIndDcR = 1e-4;

/// The conductance leaked from a node to ground when the DC network has no
/// path to ground for it.
///
/// **Why a node can need this.** At DC a capacitor is an open circuit, so a
/// node that reaches the rest of the circuit only through a capacitor has no
/// conductance at all -- and a nodal matrix row of zeros is a singular
/// matrix. A preamp's output node is the usual case: it hangs off the plate
/// through the output coupling capacitor, and nothing else touches it.
///
/// **Why 1e-12.** This is SPICE's `gmin`, and it is what ngspice does with the
/// same default. It is far too small to change any circuit that has a real
/// path to ground, and it is the whole difference between "no DC answer" and
/// "a DC answer whose floating node sits at 0 V" -- which is the right answer,
/// since a node with no DC path has no DC voltage to find.
///
/// It is applied **only as a fallback**, when the plain DC network is
/// singular, so a circuit that solves today solves exactly as it did.
inline constexpr double kGminDc = 1e-12;

/// An ideal source: v(a) - v(b) = dc at DC and = ac in an AC analysis.
struct Src {
    std::string a, b;
    double dc = 0.0;
    double ac = 0.0;
    std::vector<double> sin;  // raw SIN(vo va freq td theta phase) args, if any
    std::string name;
};

/// One tube: the nonlinear part of the Tubes.lib subckt.
/// `g`/`k` are the external grid and cathode (the grid diode sits between them),
/// `gi` the internal grid behind rgi + vct, `p` the plate.
struct Tri {
    std::string g, p, k, gi;
    TriodeModel model;
    std::string name;       // device name (from LiveWDFDocument component ID)
};

/// A diode, as an element (`D1 anode cathode <model>`). LiveWDF step G.
///
/// Unlike `Res`/`Cap`/`Ind` this is **not** an element port: it is nonlinear, so
/// it does not reduce to a conductance. It becomes a *root port* instead --
/// exactly like each of a tube's three -- and the Newton solve finds its
/// current. That is the whole point of the element: it is the test of whether
/// the root abstraction is genuinely device-independent, or whether it only ever
/// worked because every root happened to be a triode.
///
/// `model` is SPICE's junction law with its own `RS` inside it; the rest of the
/// series resistance a diode sees is the network's, and stays in `Z`.
struct Diode {
    std::string a, b;  // anode, cathode
    DiodeModel model;
    std::string name;
};

/// An ideal or finite-output-resistance Voltage-Controlled Voltage Source (SPICE `E` element).
/// In linear MNA / WDF scattering, it acts as a Thevenin/Norton source with output conductance
/// Go = 1/ro and transconductance gm = gain * Go from input (inA, inB) to output (outA, outB).
struct VCVS {
    std::string outA, outB;  // positive, negative output terminals
    std::string inA, inB;    // positive, negative controlling input terminals
    double gain = 1.0;
    double ro = 1e-4;        // output resistance, ohms (default 0.1 mOhm)
    std::string name;
};

/// One power pentode tube (e.g. EL34, 6L6GC, EL84, 6V6S, KT66, KT77).
/// Terminals: plate p, screen grid s, control grid g, cathode k.
/// Root ports: (p, k), (s, k), (g, k).
struct Pent {
    std::string p, s, g, k;
    Pentode model;
    std::string name;
};

/// One n-channel JFET (`J1 drain gate source model`). The intrinsic device has
/// three terminals and two nonlinear root ports: drain-source and gate-source.
/// `d`/`s` are the intrinsic nodes behind the model's `rd`/`rs`; `dExt`/`sExt`
/// are the nodes the schematic connects. `addJfet` creates the series
/// resistors, the gate resistor and the intrinsic capacitances, so the
/// converter only has to name the three visible pins.
struct Jfet {
    std::string d, g, s;          // intrinsic drain, gate, source
    std::string dExt, gExt, sExt; // visible terminals
    JfetModel model;
    std::string name;
};

/// One n-channel MOSFET (`M1 drain gate source model`). The paper's device is
/// three-terminal and the gate carries no current, so this is the intrinsic
/// two-port root device: (drain, source) and (gate, source). No model-card
/// parasitics are invented or added here. A SPICE bulk terminal may be supplied
/// only when it is tied to the source, because the body effect is outside this
/// model.
struct Mosfet {
    std::string d, g, s;
    MosfetModel model;
    std::string name;
};

/// Operational Amplifier (Op-Amp) model.
/// Pins: out, neg (inverting input), pos (non-inverting input), vcc, vee (supply rails)
struct OpAmp {
    std::string out, neg, pos, vcc, vee;
    OpAmpModel model;
    std::string name;
};

/// The small, user-facing transformer shape: turns plus one of three behaviour
/// cards. `turnsPrimary` and `turnsSecondary` are total end-to-end turns, so
/// the centre-tap half is half a primary turn and no `L1`/`L2`/`k` vocabulary
/// leaks into the UI. `model` is `ideal`, `auto` or `manual`; `auto` derives a
/// real model card from the turns ratio, while `manual` lets the saved document
/// state the inductance and leakage directly.
struct TransformerSpec {
    std::string name;
    double turnsPrimary = 1.0;
    double turnsSecondary = 1.0;
    std::string model = "auto";
    double Lm = 0.0;          // primary-referred magnetizing inductance (H)
    double leakage = 0.0;     // primary-referred leakage inductance (H)
    double dcrPrimary = 0.0;  // primary winding resistance (ohm)
    double dcrSecondary = 0.0;// secondary winding resistance (ohm)
    double cp = 0.0;          // optional primary cross-coupling capacitor (F)
    double rc = 0.0;          // optional primary cross-coupling resistor (ohm)
};

inline std::string lowerCase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// Fill the numerical transformer fields from the user model.
///
/// `Ideal` is a numerically faithful limit: a large but finite Lm plus a small
/// leakage floor. A rank-one transformer is singular, so a strict ideal core
/// cannot be inverted; this keeps the matrix stiff but well-conditioned while
/// making the inductive divider invisible over the audio band.
inline TransformerSpec resolveTransformerSpec(TransformerSpec spec) {
    if (!(spec.turnsPrimary > 0.0) || !(spec.turnsSecondary > 0.0) ||
        !std::isfinite(spec.turnsPrimary) || !std::isfinite(spec.turnsSecondary))
        throw std::invalid_argument("transformer turns ratio must be finite and positive");

    const double ratio = spec.turnsPrimary / spec.turnsSecondary;
    const double referenceOhms = 8.0;
    const double bassCorner = 30.0;
    // Guitar output transformer sheets (Hammond 1750 series) put leakage at
    // 1/7700 to 1/1000 of the primary inductance.
    const double leakageDenominator = 2400.0;
    const double dcrDenominator = 40.0;
    const double autoLm = (ratio * ratio * referenceOhms) /
                          (2.0 * kPi * bassCorner);
    const double autoLeakage = autoLm / leakageDenominator;
    const double autoDcrPrimary = (ratio * ratio * referenceOhms) / dcrDenominator;
    const double autoDcrSecondary = referenceOhms / dcrDenominator;

    // The user-facing picker says `Real — Auto` and `Real — Manual`, while
    // older saved documents may say just `Auto` or `Manual`. Canonicalize both
    // to the numerical card names the resolver understands.
    const std::string normalizedModel = lowerCase(spec.model);
    const bool idealModel = normalizedModel.find("ideal") != std::string::npos;
    const bool manualModel =
        !idealModel && normalizedModel.find("manual") != std::string::npos;

    if (idealModel) {
        spec.Lm = 1.0e3;
        spec.leakage = 1.0e-1;
    } else if (manualModel) {
        if (!(spec.Lm > 0.0) || !std::isfinite(spec.Lm))
            spec.Lm = autoLm;
        if (!(spec.leakage > 0.0) || !std::isfinite(spec.leakage))
            spec.leakage = autoLeakage;
        // A manual card can describe a near-rank-one transformer, but the
        // solver needs a finite inverse. The floor is purely numerical, far
        // below any wound transformer's leakage.
        const double minimumLeakage =
            std::max(1.0e-9 * spec.Lm, std::numeric_limits<double>::min());
        if (!(spec.leakage >= minimumLeakage))
            spec.leakage = minimumLeakage;
    } else {
        spec.Lm = autoLm;
        spec.leakage = autoLeakage;
    }

    if (!std::isfinite(spec.dcrPrimary) || spec.dcrPrimary < 0.0)
        spec.dcrPrimary = 0.0;
    if (!std::isfinite(spec.dcrSecondary) || spec.dcrSecondary < 0.0)
        spec.dcrSecondary = 0.0;
    if (!std::isfinite(spec.cp) || spec.cp < 0.0)
        spec.cp = 0.0;
    if (!std::isfinite(spec.rc) || spec.rc < 0.0)
        spec.rc = 0.0;

    if (idealModel) {
        spec.dcrPrimary = 0.0;
        spec.dcrSecondary = 0.0;
    } else if (!manualModel) {
        spec.dcrPrimary = autoDcrPrimary;
        spec.dcrSecondary = autoDcrSecondary;
    }

    if (!(spec.Lm + spec.leakage > 0.0) || !std::isfinite(spec.Lm + spec.leakage))
        throw std::invalid_argument("transformer inductance must be finite and positive");
    return spec;
}

/// A group of magnetically coupled inductors (SPICE `K` directive), or one
/// transformed device carrying an explicit inductance matrix. Empty `L` uses
/// the old SPICE-K model `L_ij = k sqrt(L_i L_j)`; a real transformer supplies
/// the full matrix instead.
struct MutualInd {
    std::string name;
    std::vector<std::string> indNames;
    double k = 0.0;
    std::vector<std::vector<double>> L;
};

/// A three-terminal BjtDevice with Ebers-Moll model (Ebers-Moll + Early effect).
/// Three terminals: collector (c), base (b), emitter (e).
/// Root ports: (collector, emitter), (base, emitter).
struct BjtDevice {
    std::string c, b, e;    // collector, base, emitter
    BjtModel model;
    std::string name;
};

/// A clocked audio delay whose output drives a Norton equivalent.
struct BucketBrigade {
    std::string input, control, common, name;
    BucketBrigadeModel model;
    std::size_t outputCurrentIndex = 0;
};

/// A log-RMS level detector drives a Norton voltage-output equivalent.
struct RmsDetector {
    std::string input, common, name;
    RmsDetectorModel model;
    std::size_t outputCurrentIndex;
};

// In a matched input pair the geometric-mean collector current changes by
// exp((Ni - Pi)/(2*VT)) when the input-side base voltages are displaced.
struct Dbx200Bias {
    static constexpr double twiceThermalVolts = 2.0 * 0.025693;
    size_t pnp, npn;
    std::string ni, pi;
};

struct Circuit {
    std::vector<Res> res;
    std::vector<Cap> cap;
    std::vector<Ind> ind;
    std::vector<Isrc> isrc;
    std::vector<Src> src;
    std::vector<Tri> tri;
    std::vector<Diode> diode;
    std::vector<Pot> pots;
    std::vector<VCVS> vcvs;
    std::vector<Pent> pent;
    std::vector<Jfet> jfet;
    std::vector<Mosfet> mosfet;
    std::vector<BjtDevice> bjt;
    std::vector<OpAmp> opamp;
    std::vector<BucketBrigade> bucketBrigades;
    std::vector<RmsDetector> rmsDetectors;
    std::vector<Dbx200Bias> dbx200Biases;
    // Names of equivalent voltages eliminated by an exact reduction.
    std::vector<std::pair<std::string, std::string>> nodeAliases;

    void addRes(const std::string& a, const std::string& b, double v,
                const std::string& name = {}) {
        res.push_back({a, b, v, name});
    }
    void addCap(const std::string& a, const std::string& b, double v,
                const std::string& name = {}, std::optional<double> initialVolts = std::nullopt) {
        cap.push_back({a, b, v, name, initialVolts});
    }
    void addInd(const std::string& a, const std::string& b, double v,
                const std::string& name = {}) {
        ind.push_back({a, b, v, name});
    }
    void addIsrc(const std::string& a, const std::string& b, double dc = 0.0, double ac = 0.0,
                 std::vector<double> sin = {}, const std::string& name = {}) {
        isrc.push_back({a, b, dc, ac, std::move(sin), name});
    }
    void addBucketBrigade(const std::string& input, const std::string& output,
                          const std::string& control, const std::string& common,
                          const BucketBrigadeModel& model, const std::string& name = {}) {
        if (!model.valid())
            throw std::invalid_argument("Invalid bucket-brigade parameters: " + name);
        const auto id = name.empty() ? ("BBD" + std::to_string(bucketBrigades.size() + 1)) : name;
        addRes(input, common, 1e6, id + "_in");
        addRes(control, common, 1e8, id + "_clock");
        addRes(output, common, model.outputOhms, id + "_out");
        bucketBrigades.push_back({input, control, common, id, model, isrc.size()});
        addIsrc(common, output, model.outputBias / model.outputOhms, 0.0, {}, id + "_drive");
    }
    void addRmsDetector(const std::string& input, const std::string& output,
                        const std::string& common, const RmsDetectorModel& model,
                        const std::string& name = {}) {
        if (!model.valid())
            throw std::invalid_argument("Invalid RMS detector parameters: " + name);
        const auto id = name.empty() ? ("RMS" + std::to_string(rmsDetectors.size() + 1)) : name;
        addRes(input, common, model.inputOhms, id + "_in");
        addRes(output, common, model.outputOhms, id + "_out");
        rmsDetectors.push_back({input, common, id, model, isrc.size()});
        addIsrc(common, output, model.dcOutputCurrent(0.0), 0.0, {}, id + "_drive");
    }
    void addSrc(const std::string& a, const std::string& b, double dc = 0.0, double ac = 0.0,
                std::vector<double> sin = {}, const std::string& name = {}) {
        src.push_back({a, b, dc, ac, std::move(sin), name});
    }
    void addTri(const std::string& g, const std::string& p, const std::string& k,
                const std::string& gi, const TriodeModel& m) {
        tri.push_back({g, p, k, gi, m});
    }
    void addDiode(const std::string& a, const std::string& b, const DiodeModel& m,
                  const std::string& name = {}) {
        diode.push_back({a, b, m, name});
    }
    void addPot(const std::string& n1, const std::string& wiper, const std::string& n2,
                double totalR, double pos = 0.5, PotTaper taper = PotTaper::Linear,
                const std::string& name = {}, double taperMid = 0.0) {
        std::string potName = name.empty() ? ("P" + std::to_string(pots.size() + 1)) : name;
        pots.push_back({n1, wiper, n2, totalR, pos, taper, potName, taperMid});
        auto [r1, r2] = Pot::resistances(totalR, pos, taper, taperMid);
        addRes(n1, wiper, r1, potName + "_1");
        addRes(wiper, n2, r2, potName + "_2");
    }

    std::vector<VarRes> varRes;
    std::vector<ControlledRes> ctrlRes;

    void addVarRes(const std::string& a, const std::string& b, double maxR,
                   double pos = 0.5, PotTaper taper = PotTaper::Linear,
                   const std::string& name = {}, double taperMid = 0.0) {
        std::string vrName = name.empty() ? ("VR" + std::to_string(varRes.size() + 1)) : name;
        varRes.push_back({a, b, maxR, pos, taper, vrName, taperMid});
        addRes(a, b, VarRes::resistance(maxR, pos, taper, taperMid), vrName);
    }

    void addControlledRes(const std::string& a, const std::string& b, double initialOhms = 1e3,
                          const std::string& name = {}) {
        std::string crName = name.empty() ? ("CR" + std::to_string(ctrlRes.size() + 1)) : name;
        ctrlRes.push_back({a, b, initialOhms, crName, std::nullopt, std::nullopt});
    }

    void addPhotoresistor(const std::string& a, const std::string& b, double darkOhms,
                         const PhotoresistorControl& photo, const std::string& name = {}) {
        const auto positive = [](double v) { return std::isfinite(v) && v > 0.0; };
        if (!positive(darkOhms) || !positive(photo.brightOhms) ||
            photo.brightOhms > darkOhms || !positive(photo.referenceVolts) ||
            !positive(photo.gamma) || !positive(photo.attackSeconds) ||
            !positive(photo.releaseSeconds))
            throw std::invalid_argument("Invalid photoresistor parameters: " + name);
        addControlledRes(a, b, darkOhms, name);
        ctrlRes.back().photo = photo;
    }

    std::vector<MutualInd> mutual;

    void addMutualInd(const std::string& name, const std::vector<std::string>& indNames, double k) {
        mutual.push_back({name, indNames, k});
    }
    void addMutualIndExplicit(const std::string& name,
                              const std::vector<std::string>& indNames,
                              const std::vector<std::vector<double>>& L) {
        mutual.push_back({name, indNames, 0.0, L});
    }

    // SPICE's `K` directive: two named inductors and one pair coefficient. This
    // is a netlist primitive, not the user-facing transformer model.
    void addTransformer(const std::string& l1, const std::string& l2, double k,
                        const std::string& name = {}) {
        mutual.push_back({name.empty() ? ("K_" + l1 + "_" + l2) : name, {l1, l2}, k});
    }

    /// User-facing four-terminal transformer. `turnsPrimary` and `turnsSecondary`
    /// are the physical end-to-end turns, so the turns ratio fixes the impedance
    /// behaviour; `spec.model` chooses the ideal, automatic, or explicit model
    /// card. The winding resistances sit outside the coupled inductance matrix.
    void addTransformerElement(const std::string& p1, const std::string& p2,
                               const std::string& s1, const std::string& s2,
                               const TransformerSpec& inSpec,
                               const std::string& name = {}) {
        TransformerSpec spec = resolveTransformerSpec(inSpec);
        const std::string base =
            name.empty() ? ("T" + std::to_string(mutual.size() + 1)) : name;

        auto seriesEnds = [this, &base](const std::string& a, const std::string& b,
                                        double ohms, const std::string& tag)
            -> std::pair<std::string, std::string> {
            if (!(ohms > 0.0))
                return {a, b};
            const std::string ai = base + "$" + tag + "a";
            const std::string bi = base + "$" + tag + "b";
            addRes(a, ai, 0.5 * ohms, base + "$" + tag + "1");
            addRes(b, bi, 0.5 * ohms, base + "$" + tag + "2");
            return {ai, bi};
        };

        const auto [pa, pb] = seriesEnds(p1, p2, spec.dcrPrimary, "dp");
        const auto [se1, se2] = seriesEnds(s1, s2, spec.dcrSecondary, "ds");

        const double n = spec.turnsSecondary / spec.turnsPrimary;
        const double n2 = n * n;
        const double lpp = spec.Lm + spec.leakage;
        const double lss = lpp * n2;
        const double lps = spec.Lm * n;

        const std::string pri = base + "$pri";
        const std::string sec = base + "$sec";
        addInd(pa, pb, lpp, pri);
        addInd(se1, se2, lss, sec);
        addMutualIndExplicit(base, {pri, sec}, {{lpp, lps}, {lps, lss}});

        if (spec.cp > 0.0)
            addCap(p1, p2, spec.cp, base + "$cp");
        if (spec.rc > 0.0)
            addRes(p1, p2, spec.rc, base + "$rc");
    }

    /// Five-terminal centre-tapped transformer: three terminals belong to the
    /// primary winding, with the middle one at the tap, and two belong to the
    /// secondary. The model is still one coupled device, not two transformers
    /// and not two independent halves. `turnsPrimary` is the full primary turn
    /// count, so each half carries a quarter of the magnetizing inductance and
    /// half of its leakage allowance; the cross-coupling between the halves and
    /// the secondary is what keeps the tap electrically centred.
    void addCenterTapTransformerElement(const std::string& p1, const std::string& pt,
                                        const std::string& p2, const std::string& s1,
                                        const std::string& s2, const TransformerSpec& inSpec,
                                        const std::string& name = {}) {
        TransformerSpec spec = resolveTransformerSpec(inSpec);
        const std::string base =
            name.empty() ? ("T" + std::to_string(mutual.size() + 1)) : name;

        auto seriesEnds = [this, &base](const std::string& a, const std::string& b,
                                        double ohms, const std::string& tag)
            -> std::pair<std::string, std::string> {
            if (!(ohms > 0.0))
                return {a, b};
            const std::string ai = base + "$" + tag + "a";
            const std::string bi = base + "$" + tag + "b";
            addRes(a, ai, 0.5 * ohms, base + "$" + tag + "1");
            addRes(b, bi, 0.5 * ohms, base + "$" + tag + "2");
            return {ai, bi};
        };

        const auto [up1, upt] = seriesEnds(p1, pt, 0.5 * spec.dcrPrimary, "dup");
        const auto [lpt, lp2] = seriesEnds(pt, p2, 0.5 * spec.dcrPrimary, "dlp");
        const auto [se1, se2] = seriesEnds(s1, s2, spec.dcrSecondary, "ds");

        const double n = spec.turnsSecondary / spec.turnsPrimary;
        const double lpp = spec.Lm + spec.leakage;
        const double lhh = 0.25 * spec.Lm + 0.5 * spec.leakage;
        const double lss = lpp * n * n;
        const double lhl = 0.25 * spec.Lm;
        const double lhs = 0.5 * spec.Lm * n;

        const std::string up = base + "$sup";
        const std::string low = base + "$slw";
        const std::string sec = base + "$sec";
        addInd(up1, upt, lhh, up);
        addInd(lpt, lp2, lhh, low);
        addInd(se1, se2, lss, sec);
        addMutualIndExplicit(base,
                             {up, low, sec},
                             {
                                 {lhh, lhl, lhs},
                                 {lhl, lhh, lhs},
                                 {lhs, lhs, lss},
                             });

        if (spec.cp > 0.0)
            addCap(p1, p2, spec.cp, base + "$cp");
        if (spec.rc > 0.0)
            addRes(p1, p2, spec.rc, base + "$rc");
    }

    void addVCVS(const std::string& outA, const std::string& outB,
                 const std::string& inA, const std::string& inB,
                 double gain = 1.0, double ro = 1e-4, const std::string& name = {}) {
        std::string vName = name.empty() ? ("E" + std::to_string(vcvs.size() + 1)) : name;
        vcvs.push_back({outA, outB, inA, inB, gain, ro, vName});
    }

    /// Bootstrapping the root port makes the output current proportional to
    /// the input current independently of the output load.
    void addCurrentVca(const std::string& input, const std::string& output,
                       const std::string& signalReference, double inputOhms,
                       double currentOhms, const VcaControl& control,
                       const std::string& name) {
        const double initialOhms = currentOhms / control.initialGain();
        if (!std::isfinite(inputOhms) || inputOhms <= 0.0 ||
            !std::isfinite(initialOhms) || initialOhms <= 0.0 ||
            !std::isfinite(control.fullGainVolts) || control.fullGainVolts <= 0.0)
            throw std::invalid_argument("Invalid VCA parameters: " + name);
        const std::string copy = name + "$copy";
        const std::string drive = name + "$drive";
        addRes(input, signalReference, inputOhms, name + "$ri");
        addVCVS(copy, signalReference, output, signalReference, 1.0, 1e-4, name + "$bootstrap");
        addVCVS(drive, copy, input, signalReference, -1.0, 1e-4, name + "$input");
        addControlledRes(drive, output, initialOhms, name);
        ctrlRes.back().vca = control;
    }

    /// The linear cell mirrors half the input current at unity gain.
    void addLinearVca(const std::string& input, const std::string& output,
                      double inputOhms, const VcaControl& control,
                      const std::string& name) {
        addCurrentVca(input, output, control.reference, inputOhms, 2.0 * inputOhms,
                      control, name);
    }

    /// Room-temperature THAT2180 macromodel.
    /// https://thatcorp.com/datashts/THAT_2180-Series_Datasheet.pdf
    /// A small finite input resistance
    /// approximates the summing input's virtual ground. This models gain
    /// within the normal bias range and omits chip noise and distortion.
    void addThat2180(const std::string& input, const std::string& output,
                     const std::string& ecPlus, const std::string& ecMinus,
                     const std::string& signalReference, double inputOhms,
                     const std::string& name) {
        addCurrentVca(input, output, signalReference, inputOhms, inputOhms,
                      {ecPlus, ecMinus, 1.0, true}, name);
    }

    /// Calibrated analog terminals of dbx drawing 110557 (1975). The two
    /// matched transistor pairs share emitter nodes, so each collector-current
    /// ratio is exp(deltaVbase / VT). The input pair's current difference is
    /// Iin and its current product at centered symmetry is Iq^2.
    void addDbx200(const std::string& input, const std::string& output,
                   const std::string& pi, const std::string& ni,
                   const std::string& po, const std::string& no,
                   const std::string& common, const std::string& name) {
        constexpr double senseOhms = 10.0;
        constexpr double idleAmps = 5.17e-6; // matched reference circuit, neutral controls
        constexpr double voltsPerDb = 0.002958; // kT/q * ln(10)/20, 25 C
        const auto copy = name + "$copy";
        const auto drive = name + "$drive";
        addRes(input, common, senseOhms, name + "$sense");
        addVCVS(copy, common, output, common, 1.0, 1e-4, name + "$bootstrap");
        addVCVS(drive, copy, input, common, -1.0, 1e-4, name + "$input");
        const auto leg = [&](const std::string& high, const std::string& low,
                             double sign, const std::string& suffix) {
            addControlledRes(drive, output, senseOhms, name + suffix);
            ctrlRes.back().vca = VcaControl{high, low, 1.0, true, voltsPerDb};
            ctrlRes.back().dbx200Crossover = sign * 2.0 * idleAmps * senseOhms;
        };
        if (pi == no && po == ni) {
            // With the two controls linked, the standing currents cancel at
            // every audio level. The remaining whole-wave law is affine.
            leg(pi, po, 0.0, "$cell");
        } else {
            const size_t pnp = ctrlRes.size();
            leg(pi, po, 1.0, "$pnp");
            const size_t npn = ctrlRes.size();
            leg(no, ni, -1.0, "$npn");
            dbx200Biases.push_back({pnp, npn, ni, pi});
        }
        // Forward-biased collector/base junctions carry excess current if the
        // external transimpedance amplifier can no longer hold virtual ground.
        addDiode(output, po, DiodeModel(336.7e-15 / 3.009, 1.0, 1.8, 1e12), name + "$cbP");
        addDiode(no, output, DiodeModel(7.049e-15 / 2.209, 1.0, 1.43, 1e12), name + "$cbN");
    }

    /// Signal law of the matched NPN differential pair in MiSTer-Discrete's
    /// circuits/parts/ca3080.lib, with a real bias diode and current sensing.
    /// One channel of the NE570/NE571/SA571 compandor, from the onsemi NE570/D
    /// block diagram and circuit description: rectifier and gain-cell inputs
    /// through R1 10k and R2 20k into summing nodes at the 1.8 V bandgap, the
    /// full-wave rectifier current 2|Vin|/R1 averaged on CRECT through 10k, the
    /// gain cell Iout = Iin IG / 140 uA, and a 741-class op-amp with R3 20k to
    /// its pin and R4 30k to ground. Omits noise, the cell's offset (THD trim)
    /// and temperature.
    void addNe570(const std::string& rectIn, const std::string& rectCap,
                  const std::string& cellIn, const std::string& invIn,
                  const std::string& r3, const std::string& out,
                  const std::string& vcc, const std::string& gnd, const std::string& name) {
        const auto local = [&](const char* suffix) { return name + "$" + suffix; };
        const auto bandgap = local("bandgap"), vref = local("vref");
        addIsrc(gnd, bandgap, 1.8e-3, 0.0, {}, local("bandgap_i"));
        addRes(bandgap, gnd, 1e3, local("bandgap_r"));
        addVCVS(vref, gnd, bandgap, gnd, 1.0, 1e-4, local("vref_buffer"));
        addRes(rectIn, vref, 10e3, local("R1"));
        // Each half mirrors 2|Vin|/R1 into CRECT: a fixed 0.4 mA scaled by
        // its side of the rectifier input over 2 V.
        addCurrentVca(gnd, rectCap, vref, 1e6, 4.5e3, {rectIn, vref, 2.0}, local("rect_pos"));
        addCurrentVca(gnd, rectCap, vref, 1e6, 4.5e3, {vref, rectIn, 2.0}, local("rect_neg"));
        addRes(rectCap, gnd, 10e3, local("rect_r"));
        // Scaled by four to keep IG / I1 below the VCA's gain bound.
        addCurrentVca(vref, invIn, cellIn, 20e3, 5e3, {rectCap, gnd, 5.6}, local("cell"));
        addRes(invIn, r3, 20e3, local("R3"));
        addRes(invIn, gnd, 30e3, local("R4"));
        addOpAmp(out, invIn, vref, vcc, gnd, OpAmpModel::fromType(OpAmpType::Lm741), name);
    }

    void addCa3080(const std::string& out, const std::string& neg, const std::string& pos,
                   const std::string& vcc, const std::string& vee, const std::string& bias,
                   const std::string& name) {
        constexpr double mirrorRo = 1.0;
        const auto local = [&](const char* suffix) { return name + "$" + suffix; };
        const auto biasDiode = local("bias_diode");
        addRes(bias, biasDiode, OtaControl::senseOhms, local("bias_sense"));
        addDiode(biasDiode, vee, DiodeModel(5.95e-16, 1.0, 0.0, 1e12), local("bias"));
        addRes(biasDiode, vee, 10e6, local("bias_leak"));
        addRes(pos, neg, 26e3, local("input_res"));
        // Use a buffered copy of the output as the offset for the root
        // port. Its voltage is the input difference, independently of load.
        const auto outCopy = local("out_copy");
        const auto drive = local("drive");
        addVCVS(outCopy, vee, out, vee, 1.0, mirrorRo, local("copy_out"));
        addVCVS(drive, outCopy, pos, neg, 1.0, mirrorRo, local("drive_signal"));
        addControlledRes(drive, out, OtaControl::slopeResistance(0.0), name);
        ctrlRes.back().ota = OtaControl{bias, biasDiode};
        addRes(out, kGroundName, 15e6, local("out_res"));
        // Approximate 9 V output compliance with divided rail references.
        // Keeping both references within the rails also lets source stepping
        // initialize the circuit from zero supply.
        const auto upper = local("upper_clamp");
        const auto lower = local("lower_clamp");
        addVCVS(upper, vee, vcc, vee, 0.77, mirrorRo, local("upper_ref"));
        addVCVS(lower, vee, vcc, vee, 0.133, mirrorRo, local("lower_ref"));
        const DiodeModel clamp(1e-14, 1.0, 0.0, 1e12);
        addDiode(out, upper, clamp, local("clamp_pos"));
        addDiode(lower, out, clamp, local("clamp_neg"));
    }

    void addPentode(const std::string& p, const std::string& s,
                    const std::string& g, const std::string& k,
                    const Pentode& model = kEL34, const std::string& name = {}) {
        std::string ptName = name.empty() ? ("XP" + std::to_string(pent.size() + 1)) : name;
        pent.push_back({p, s, g, k, model, ptName});
    }

    /// Add an n-channel JFET with the model's linear parasitics.
    ///
    /// The nonlinear device is intrinsic. `rd`/`rs` become ordinary series
    /// resistors to internal `$d`/`$s` nodes, `rgate` becomes a resistor from
    /// the gate to `ground`, and `cgs`/`cgd`/`cds` become ordinary capacitors.
    /// A zero `rd`/`rs`/capacitance means the terminal is connected directly;
    /// a zero `rgate` means no external gate resistor (open gate), not a short.
    /// This keeps every linear part in the linear network and the root device
    /// exactly the two-port nonlinear branch of the paper.
    void addJfet(const std::string& drain, const std::string& gate, const std::string& source,
                 const JfetModel& model, const std::string& name = {},
                 const std::string& ground = "0") {
        if (!model.valid())
            throw std::invalid_argument("invalid JFET model parameters");
        const std::string jName = name.empty() ? ("J" + std::to_string(jfet.size() + 1)) : name;
        std::string dInt = drain;
        std::string sInt = source;
        if (model.p.rd > 0.0) {
            dInt = jName + "$d";
            addRes(drain, dInt, model.p.rd, jName + "$rd");
        }
        if (model.p.rs > 0.0) {
            sInt = jName + "$s";
            addRes(source, sInt, model.p.rs, jName + "$rs");
        }
        if (model.p.rgate > 0.0)
            addRes(gate, ground, model.p.rgate, jName + "$rg");
        if (model.p.cgs > 0.0)
            addCap(gate, sInt, model.p.cgs, jName + "$cgs");
        if (model.p.cgd > 0.0)
            addCap(gate, dInt, model.p.cgd, jName + "$cgd");
        if (model.p.cds > 0.0)
            addCap(dInt, sInt, model.p.cds, jName + "$cds");
        jfet.push_back({dInt, gate, sInt, drain, gate, source, model, jName});
    }

    /// Add a three-terminal n-channel MOSFET. The model's `K`, `Vth` and
    /// `lambda` are the whole device; no series resistances or capacitances are
    /// synthesized. The caller supplies a valid model explicitly; there are no
    /// named MOSFET presets.
    void addMosfet(const std::string& drain, const std::string& gate, const std::string& source,
                   const MosfetModel& model, const std::string& name = {}) {
        if (!model.valid())
            throw std::invalid_argument("invalid MOSFET model parameters");
        const std::string mName = name.empty() ? ("M" + std::to_string(mosfet.size() + 1)) : name;
        mosfet.push_back({drain, gate, source, model, mName});
    }

    /// Add a three-terminal BjtDevice with Ebers-Moll model.
    /// Terminals: collector (c), base (b), emitter (e).
    /// Constant junction capacitances stamp into the linear network.
    void addBjt(const std::string& collector, const std::string& base, const std::string& emitter,
                const BjtModel& model, const std::string& name = {}) {
        if (!model.valid())
            throw std::invalid_argument("invalid BjtDevice model parameters");
        const std::string bName = name.empty() ? ("Q" + std::to_string(bjt.size() + 1)) : name;
        bjt.push_back({collector, base, emitter, model, bName});
        if (model.cbe > 0.0)
            addCap(base, emitter, model.cbe, bName + "$cbe");
        if (model.cbc > 0.0)
            addCap(base, collector, model.cbc, bName + "$cbc");
    }

    /// Add an operational amplifier.
    /// Terminals: out, neg (inverting input), pos (non-inverting input), vcc, vee (supply rails).
    void addOpAmp(const std::string& out, const std::string& neg, const std::string& pos,
                  const std::string& vcc, const std::string& vee,
                  const OpAmpModel& model, const std::string& name = {}) {
        const std::string oName = name.empty() ? ("U" + std::to_string(opamp.size() + 1)) : name;
        if (model.type != OpAmpType::Lm358) {
            opamp.push_back({out, neg, pos, vcc, vee, model, oName});
            return;
        }
        // MiSTer-Discrete 692e777, circuits/parts/lm358.lib. Each driver
        // returns its unused steering current to the same supply that fed it.
        // The two buffered copies of the pole implement these current paths
        // without a branch-current probe or another nonlinear equation.
        const auto pole = oName + "$pole";
        opamp.push_back({pole, neg, pos, vcc, vee, model, oName});
        const auto high = oName + "$high", low = oName + "$low";
        const auto bh = oName + "$sourceBase", bl = oName + "$sinkBase";
        addVCVS(high, vcc, pole, vcc, 1.0, 0.01, oName + "$sourceDrive");
        addVCVS(low, vee, pole, vee, 1.0, 0.01, oName + "$sinkDrive");
        addIsrc(vcc, bh, 40e-3, 0.0, {}, oName + "$sourceBias");
        addIsrc(bl, vee, 20e-3, 0.0, {}, oName + "$sinkBias");
        addRes(bh, high, 100e3, oName + "$sourceReturn");
        addRes(low, bl, 100e3, oName + "$sinkReturn");
        const DiodeModel steering(1e-5, 1.0, 0.0, 1e12);
        addDiode(bh, high, steering, oName + "$sourceSteer");
        addDiode(low, bl, steering, oName + "$sinkSteer");
        addDiode(bh, out, DiodeModel(1.2e-13, 2.0, 0.0, 1e12), oName + "$darlington");
        addDiode(out, bl, DiodeModel(1.2e-14, 1.0, 54.0, 1e12), oName + "$pnp");
        // The upstream floor diode and 29 µA pulldown combine into one
        // Shockley branch: I(out→vee) = Ipd * (1 - exp(-Vout,vee / VT)).
        addDiode(vee, out, DiodeModel(29e-6, 1.0, 0.0, 1e12), oName + "$floor");
        addRes(out, vcc, 20e6, oName + "$loadPlus");
        addRes(out, vee, 20e6, oName + "$loadMinus");
        addIsrc(vcc, vee, 0.321e-3, 0.0, {}, oName + "$quiescent");
    }

    template<class Visitor>
    void visitStampedNodes(Visitor&& push) const {
        for (const auto& e : res) {
            push(e.a);
            push(e.b);
        }
        for (const auto& e : cap) {
            push(e.a);
            push(e.b);
        }
        for (const auto& e : ind) {
            push(e.a);
            push(e.b);
        }
        for (const auto& e : isrc) {
            push(e.a);
            push(e.b);
        }
        for (const auto& s : src) {
            push(s.a);
            push(s.b);
        }
        for (const auto& t : tri) {
            push(t.g);
            push(t.p);
            push(t.k);
            push(t.gi);
        }
        for (const auto& d : diode) {
            push(d.a);
            push(d.b);
        }
        for (const auto& cr : ctrlRes) {
            push(cr.a);
            push(cr.b);
            if (cr.photo) {
                push(cr.photo->sensePos);
                push(cr.photo->senseNeg);
            }
        }
        for (const auto& v : vcvs) {
            push(v.outA);
            push(v.outB);
            push(v.inA);
            push(v.inB);
        }
        for (const auto& pt : pent) {
            push(pt.p);
            push(pt.s);
            push(pt.g);
            push(pt.k);
        }
        for (const auto& j : jfet) {
            push(j.d);
            push(j.g);
            push(j.s);
            push(j.dExt);
            push(j.gExt);
            push(j.sExt);
        }
        for (const auto& m : mosfet) {
            push(m.d);
            push(m.g);
            push(m.s);
        }
        for (const auto& bj : bjt) {
            push(bj.c);
            push(bj.b);
            push(bj.e);
        }
        // Op-amp ports are all referenced to ground in the compiled model;
        // only the signal terminals (out, neg, pos) are compiled nodes.
        for (const auto& oa : opamp) {
            push(oa.out);
            push(oa.neg);
            push(oa.pos);
        }
    }

    std::vector<std::string> allNodes() const {
        std::vector<std::string> out;
        visitStampedNodes([&out](const std::string& n) {
            if (!n.empty() && std::find(out.begin(), out.end(), n) == out.end())
                out.push_back(n);
        });
        return out;
    }
};

} // namespace wdf
