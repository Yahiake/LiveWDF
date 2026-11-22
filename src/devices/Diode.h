#pragma once

// The Shockley diode and its extended form, in closed form.
//
// This is the project's one nonlinear two-terminal device law, used by the
// netlist `D` element (`src/spice/SpiceNet.h`), where `rs` is the SPICE diode's
// own `RS`. With `rp > 0` this is the extended Shockley diode model of
// Bernardini et al. (Bernardini, Werner, Maffezzoni, Sarti, 144th AES Conv.,
// 2018; Bernardini, Maffezzoni, Sarti, IEEE/ACM TASLP 2019; and the closed-form
// Wright-Omega inversion in Bernardini et al., IEEE/ACM TASLP 2021, Sec. IV.B,
// eqns (43)-(55), as implemented in Giampiccolo/Ravasi/Bernardini's ATTILA,
// `extendedSchockleyDiodeScat.m`): the Shockley junction with a parallel
// resistor on top,
//
//     i = Is * (exp((v - i*Rs) / (n*Vt)) - 1) + (v - i*Rs) / Rp
//
// `Rp == 0` is the plain SPICE junction law, and the closed form degenerates
// into it exactly; `Rp = 1e6` is the core default. The extension's whole point
// is that it bounds the
// conductance into [1/(Rp+Rs), 1/Rs]: the plain law's deep-reverse saturation
// flushes its conductance to exactly zero and leaves Newton with a zero
// gradient to ride, which is where the reference engines diverge and this one
// stalls. With the floor in place that stall is structurally impossible.
//
// Absorbing the series drop turns it into the omega equation, so `eval` is one
// omega seed plus two Halley refinements, and never an iteration to
// convergence:
//
//     g1 = 1 + Rs/Rp,   c = n*Vt*g1,   s = Is*Rs/c,   z = s + ln(s)
//     w  = wrightOmega(z + v/c)
//     i  = (n*Vt/Rs)*w - Is*Rp/(Rp+Rs) + v/(Rp+Rs)
//     di = (w/Rs + 1/Rp) * (Rp/(Rp+Rs)) / (1 + w)
//
// The seed is Stefano D'Angelo's fourth-order approximation (DAFx-19, see
// devices/WrightOmega.h), which is what wdf's diode library runs. The paper's
// omega4 is float-quality -- a ~5e-4 residual -- and this engine's Newton gates
// are 1e-12, so the two Halley steps lift the same seed to machine precision.
// wdf's pair library seeds its pair solve on `omega4` alone; this engine seeds
// the same eqn (39) start with `wrightOmega` and finishes the port inversion
// with a few guarded-Newton steps on the exact branch laws. Rp ->
// infinity degenerates to the plain
// Shockley formulas
//
//     w = omega(Is*Rs/nVt + ln(Is*Rs/nVt) + v/nVt),  i = (n*Vt/Rs)*w - Is,
//     di = w / (Rs*(1 + w))
//
// term for term; the same omega path serves both laws.
//
// The alternative -- the naive `i = Is*(exp(v/nVt) - 1)` -- is what a WDF
// diode usually gets, and it is the reason diode clippers are famous for
// diverging: the exponential's derivative outgrows any Newton step. The omega
// form is the *exact* solution of the series system, so there is nothing to
// diverge from.

#include <cmath>
#include <string_view>

#include "devices/WrightOmega.h"

namespace wdf {

/// Thermal voltage at 27 degC with ngspice's constants (25.8649170 mV). Shared
/// by every Shockley-law device, which today is just the netlist diode.
inline constexpr double kThermalVoltage = 1.38064852e-23 * 300.15 / 1.6021766208e-19;

/// A Shockley diode with a series resistance and the extended form's parallel
/// resistor -- see the header for the law and its one-omega closed form.
struct DiodeModel {
    double isat = 1e-9;  // IS, amperes
    double n = 1.0;      // emission coefficient (SPICE's `N`, the paper's eta)
    double rs = 1.0;     // series resistance, ohms (SPICE's `RS`, the paper's Rs)
    double rp = 0.0;     // parallel resistance, ohms; 0 keeps the plain law
    double nvt = 1.0;    // n * Vt, derived
    double z0 = 0.0;     // plain-law (rp == 0) omega constant; meaningless when rs == 0
    double zext = 0.0;   // extended-law omega constant: s + ln(s), s = Is*Rs/(nVt*(1+Rs/Rp))
    double vcrit = 0.0;  // the knee boundary `limit` works against (see there)
    double inv_nvt = (nvt > 0.0) ? (1.0 / nvt) : 0.0;
    double inv_rs = (rs > 0.0) ? (1.0 / rs) : 0.0;
    double nvt_over_rs = (rs > 0.0) ? (nvt / rs) : 0.0;
    double isat_over_nvt = (nvt > 0.0) ? (isat / nvt) : 0.0;
    // Extended-law derived constants, zero on the plain law (rp == 0):
    double inv_rp = 0.0;      // 1/Rp
    double inv_c = 0.0;       // 1/(nVt*(1 + Rs/Rp))
    double inv_g1 = 0.0;      // 1/(1 + Rs/Rp)
    double isat_g1 = 0.0;     // Is*inv_g1 = Is*Rp/(Rp+Rs)
    double inv_rple = 0.0;    // 1/(Rp + Rs)

    /// `exp` of this overflows a double. Clamping the exponent past it keeps the
    /// law finite instead of producing `inf`/`NaN` for the Newton to chew on.
    static constexpr double kMaxExponent = 700.0;

    DiodeModel() = default;
    DiodeModel(double saturation, double emission, double series, double parallel = 0.0) noexcept
        : isat(saturation),
          n(emission),
          rs(series),
          rp(parallel),
          nvt(emission * kThermalVoltage),
          z0(series > 0.0 ? std::log(saturation * series / (emission * kThermalVoltage)) +
                               saturation * series / (emission * kThermalVoltage)
                          : 0.0),
          zext(parallel > 0.0 && series > 0.0
                   ? saturation * series /
                         (emission * kThermalVoltage * (1.0 + series / parallel)) +
                         std::log(saturation * series /
                                  (emission * kThermalVoltage * (1.0 + series / parallel)))
                   : 0.0),
          vcrit(emission > 0.0 && saturation > 0.0
                    ? emission * kThermalVoltage *
                          std::log(emission * kThermalVoltage /
                                   (std::sqrt(2.0) * saturation))
                    : 0.0),
          inv_nvt(emission > 0.0 ? 1.0 / (emission * kThermalVoltage) : 0.0),
          inv_rs(series > 0.0 ? 1.0 / series : 0.0),
          nvt_over_rs(series > 0.0 ? (emission * kThermalVoltage) / series : 0.0),
          isat_over_nvt(emission > 0.0 ? saturation / (emission * kThermalVoltage) : 0.0),
          inv_rp(parallel > 0.0 ? 1.0 / parallel : 0.0),
          inv_c(parallel > 0.0 && series > 0.0
                    ? 1.0 / (emission * kThermalVoltage * (1.0 + series / parallel))
                    : 0.0),
          inv_g1(parallel > 0.0 && series > 0.0 ? 1.0 / (1.0 + series / parallel) : 0.0),
          isat_g1(parallel > 0.0 && series > 0.0
                      ? saturation / (1.0 + series / parallel)
                      : 0.0),
          inv_rple(parallel + series > 0.0 ? 1.0 / (parallel + series) : 0.0) {}

    /// Current through the diode and its derivative, at the voltage across it
    /// (`v` positive = forward). Allocation-free and branch-predictable, so it
    /// is what the audio loop calls: one omega seed and two Halley refinements,
    /// no iteration to convergence. (The bare `omega4` is float-quality --
    /// residual ~5e-4 -- and this engine's Newton gates are 1e-12, so the
    /// refinement is what makes the closed form usable as a truth function.)
    void eval(double v, double& i, double& di) const noexcept {
        if (rs > 0.0) {
            if (rp > 0.0) {
                const double w = wrightOmega(zext + v * inv_c);
                i = nvt_over_rs * w - isat_g1 + v * inv_rple;
                di = (w * inv_rs + inv_rp) * inv_g1 / (1.0 + w);
            } else {
                const double w = wrightOmega(z0 + v * inv_nvt);
                i = nvt_over_rs * w - isat;
                di = (w * inv_rs) / (1.0 + w);
            }
            return;
        }
        // Rs == 0: with no series resistance the law is explicit in `v`, and it
        // is the one case the omega form cannot reach (`z` diverges as Rs -> 0).
        // The exponent is clamped at the overflow point and the law is continued
        // past it on its own tangent: still finite, still positive, still
        // Newton-stable. No reference diode comes near this -- it is a guard
        // against a bad iterate, not a working regime. The Rp term keeps the
        // conductance bounded here too (a plain law just adds nothing).
        const double x = v * inv_nvt;
        const double e = std::exp(x < kMaxExponent ? x : kMaxExponent);
        i = isat * (e - 1.0) + v * inv_rp;
        di = isat_over_nvt * e + inv_rp;
    }

    /// SPICE's `pnjlim`, as the junction's own predicate: move the terminal
    /// voltage from `vold` toward `vnew` without letting the exponential leave
    /// the range in which a Newton linearisation means anything.
    ///
    /// This is not a device constraint. The law is smooth and monotone
    /// everywhere. It is a statement about the *iterate*: past `vcrit` the
    /// conductance `dI/dV` grows like the current itself, so a Newton step taken
    /// from out there is either enormous or (far more often, on a cold start)
    /// absurdly small -- and the iteration then makes no progress for its whole
    /// budget while reporting only "did not converge". Two distinct failures, so
    /// two halves to the rule.
    ///
    /// The extended law does not need this clamp at all: its conductance is
    /// bounded on both sides by construction, so the iterate cannot outrun a
    /// linearisation. Only the plain law takes it.
    ///
    /// `limited` reports whether either fired, for the caller's diagnostics.
    double limit(double vold, double vnew, bool& limited) const noexcept {
        if (rp > 0.0)
            return vnew;  // bounded conductance: nothing to guard
        // A non-physical model (no `vcrit`) disables the clamp rather than
        // producing `NaN` from it; the parser refuses such a model up front.
        if (!(vcrit > 0.0) || vnew <= vcrit || std::abs(vnew - vold) <= 2.0 * nvt)
            return vnew;
        limited = true;
        if (vold > 0.0) {
            const double arg = 1.0 + (vnew - vold) / nvt;
            return arg > 0.0 ? vold + nvt * std::log(arg) : vcrit;
        }
        return nvt * std::log(vnew / nvt);
    }
};

/// The alias `devices/Pentode.h` carries for the pentode grid diode of `Tubes.lib`
/// -- the same Shockley law under a grid-flavoured name; `test_diode.cpp` pins it.
using GridDiode = DiodeModel;

/// An anti-parallel diode pair (e.g. a clipping stage in a TS808, a Rat, a
/// Distortion+): the port solve keeps a closed-form start, so the branch models
/// need not match for it to run -- symmetric branches make the start exact, and
/// the finish on the exact laws is what keeps an asymmetric pair.
///
/// Current law for a pair across terminal voltage v:
///     i_net = i1(v) - i2(-v)
/// which for identical branches with `rs == 0` is
///     i_net = 2*Is*sinh(v/nVt) + 2*v/Rp,   di = 2*Is*cosh(v/nVt)/nVt + 2/Rp,
/// the pair conductance bounded by the parallel terms.
///
/// Under the Thevenin port constraint v = vo - Rp*i_net, eqn (39) of the Werner
/// paper -- in wdf's pair library (`wdf_lib_diode_pair.h`, Jatin Chowdhury's
/// wdf_compiler) -- supplies the closed-form dual-omega start for the port
/// voltage:
///     b = a - 2*nVt*sgn(a)*[omega(ln(R_eff*Is/nVt) + |a|/nVt)
///                           - omega(ln(R_eff*Is/nVt) - |a|/nVt)]
/// with R_eff = Rp + Rs, then i = (vo - b)/(2*R_eff) and v = vo - Rp*i. It
/// assumes the two branches carry equal-and-opposite currents, which the
/// extended law's constant term (-Is*Rp/(Rp+Rs)) breaks, so it is a start and
/// not the root: the port equation v + Rp*(i1(v) - i2(-v)) = vo carries two
/// exponentials and admits no single-omega inversion. It is finished with a
/// few guarded-Newton steps on the exact branch laws -- the residual's
/// derivative is >= 1, so one bracket keeps the steps inside it and the
/// convergence is global (1-4 evaluations from the eqn (39) start).
struct AntiParallelDiodeModel {
    DiodeModel d1;
    DiodeModel d2;
    double isat = 1e-9;
    double rs = 0.0;
    double nvt = 1.0;
    bool symmetrical = true;
    double inv_nvt = 1.0;
    mutable double cachedRp = -1.0;
    mutable double cachedLogTerm = 0.0;

    AntiParallelDiodeModel() = default;
    AntiParallelDiodeModel(const DiodeModel& a, const DiodeModel& b) noexcept
        : d1(a), d2(b) {
        symmetrical = (std::abs(a.isat - b.isat) < 1e-15 * a.isat) &&
                      (std::abs(a.n - b.n) < 1e-6) &&
                      (std::abs(a.rs - b.rs) < 1e-6 * (a.rs + 1.0)) &&
                      (std::abs(a.rp - b.rp) <= 1e-9 * (a.rp + 1.0));
        isat = a.isat;
        rs = a.rs;
        nvt = a.nvt;
        inv_nvt = (nvt > 0.0) ? (1.0 / nvt) : 0.0;
    }

    /// Solves the port equation v = vo - Rp*i_net. The branch laws are exact;
    /// eqn (39) of the Werner paper (in wdf's pair library, `wdf_lib_diode_pair.h`,
    /// Jatin Chowdhury's wdf_compiler) supplies the closed-form dual-omega
    /// start:
    ///
    ///     z = ln(R_eff*Is/nVt),  R_eff = Rp + Rs
    ///     b = vo - 2*nVt*sgn(vo)*[omega(z + |vo|/nVt) - omega(z - |vo|/nVt)]
    ///     i = (vo - b) / (2*R_eff),   v = vo - Rp*i
    ///
    /// The start is asymptotic in the branch split: it assumes the two branches
    /// carry equal-and-opposite currents, which the extended law's constant
    /// (-Is*Rp/(Rp+Rs)) breaks, so the port equation itself
    ///     v + Rp*(i1(v) - i2(-v)) = vo
    /// still needs its inversion finished -- with two exponentials on each
    /// branch there is no single-omega closed form for it. The residual
    ///     f(v) = v - vo + Rp*(i1(v) - i2(-v))
    /// is strictly increasing, f'(v) = 1 + Rp*(di1 + di2) >= 1, so the root is
    /// unique and a bracket plus guarded Newton on the exact laws converges
    /// globally. From the eqn (39) start that is 1-4 evaluations on audio
    /// samples; no table and no scan are involved.
    void solvePort(double vo, double Rp, double& vOut, double& i1, double& i2) const noexcept {
        if (vo == 0.0) {
            // Both junctions sit at zero voltage, so both branch currents are
            // exactly zero whatever the models are.
            vOut = 0.0;
            i1 = 0.0;
            i2 = 0.0;
            return;
        }

        double v = vo;

        // eqn (39): the paper's dual-omega start for the pair.
        if (symmetrical && Rp > 0.0) {
            const double rEff = Rp + rs;
            const double sgn = (vo > 0.0) ? 1.0 : -1.0;
            const double absA = std::abs(vo);
            double logTerm;
            if (cachedRp == Rp) {
                logTerm = cachedLogTerm;
            } else {
                logTerm = std::log(rEff * isat / nvt);
                cachedRp = Rp;
                cachedLogTerm = logTerm;
            }
            const double argPlus = logTerm + absA * inv_nvt;
            const double argMinus = logTerm - absA * inv_nvt;
            const double wPlus = wrightOmega(argPlus);
            const double wMinus = (argMinus > -50.0) ? wrightOmega(argMinus) : 0.0;
            const double b = vo - 2.0 * nvt * sgn * (wPlus - wMinus);
            const double iNet = (vo - b) / (2.0 * rEff);
            v = vo - Rp * iNet;
        }

        // Finish on the exact branch laws. f is strictly increasing, so a
        // bracket around the root always exists and a Newton step kept inside
        // it cannot leave it -- convergence is global, stiff ports included.
        auto evalPort = [&](double x, double& j1, double& j2, double& df) noexcept {
            double dp1 = 0.0, dp2 = 0.0;
            d1.eval(x, j1, dp1);
            d2.eval(-x, j2, dp2);
            df = 1.0 + Rp * (dp1 + dp2);
            return x - vo + Rp * (j1 - j2);
        };

        double j1 = 0.0, j2 = 0.0, df = 0.0;
        double f = evalPort(v, j1, j2, df);
        double lo = v, hi = v;
        if (f > 0.0) {
            // The root is below the start; walk down until the residual
            // changes sign. The walk only needs signs, so its currents go
            // to throw-aways and j1/j2 stay the seed's.
            hi = v;
            double w = v - (1.0 + std::abs(vo));
            double tw1 = 0.0, tw2 = 0.0, twd = 0.0;
            for (;;) {
                const double fw = evalPort(w, tw1, tw2, twd);
                if (fw < 0.0) {
                    lo = w;
                    break;
                }
                const double wn = w - (v - w);
                if (!(wn < w))
                    break;
                w = wn;
            }
        } else if (f < 0.0) {
            // The root is above the start; walk up until the residual changes
            // sign.
            lo = v;
            double w = v + (1.0 + std::abs(vo));
            double tw1 = 0.0, tw2 = 0.0, twd = 0.0;
            for (;;) {
                const double fw = evalPort(w, tw1, tw2, twd);
                if (fw > 0.0) {
                    hi = w;
                    break;
                }
                const double wn = w + (w - v);
                if (!(wn > w))
                    break;
                w = wn;
            }
        }

        constexpr int kMaxSteps = 24;
        for (int it = 0; it < kMaxSteps; ++it) {
            if (f == 0.0)
                break;
            const double nvNewton = v - f / df;
            if (nvNewton == v) {
                // The step underflowed: v is the root to the last bit.
                break;
            }
            double nv = nvNewton;
            if (lo != hi && !(nv > lo && nv < hi))
                nv = 0.5 * (lo + hi);
            if (!(std::abs(nv - v) > 1e-12 * (1.0 + std::abs(v))))
                break;
            v = nv;
            f = evalPort(v, j1, j2, df);
            if (f > 0.0)
                hi = v;
            else if (f < 0.0)
                lo = v;
            else
                break;
        }

        vOut = v;
        i1 = j1;
        i2 = j2;
    }
};

// --------------------------------------------------------------------------- //
// Named diode device registry (Cathodyne table, DAFx / SPICE models)
// --------------------------------------------------------------------------- //

/// One selectable diode model: a display name and its Shockley-law parameters.
struct DiodeEntry {
    const char* name;
    double isat;
    double n;
    double rs;
    double rp;
};

/// The 6 diode types Cathodyne offers (`Cathodyne/src/dsp/DiodeClipper.h`), with
/// the 1N4148/1N914 saturation and emission from ATTILA's extended Shockley
/// example (`Is = 4.352e-9, eta = 1.905, Rs = 0.56, Rp = 1e6`) and `Rp = 1e6`
/// carrying the Bernardini et al extension on every named diode.
inline constexpr DiodeEntry kDiodeTable[] = {
    {"1N4148", 4.352e-9, 1.905, 0.56, 1e6},
    {"1N914",  4.352e-9, 1.905, 0.56, 1e6},
    {"1N4007", 1.0e-6, 1.0, 0.04, 1e6},
    {"1N34A",  1.0e-6, 1.0, 5.0, 1e6},
    {"1N5819", 1.0e-6, 1.0, 0.05, 1e6},
    {"GZ34",   2.52e-9, 1.0, 15.0, 1e6},
};

inline constexpr int kNumDiodes = 6;

/// Finds a diode by name ("1N4148", "D1N914", etc.), case-insensitive.
/// Leading 'D' is stripped if not found directly, matching SPICE naming conventions.
inline const DiodeEntry* findDiodeByName(std::string_view name) noexcept {
    auto iequals = [](std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
            char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + 32) : b[i];
            if (ca != cb)
                return false;
        }
        return true;
    };

    for (const auto& e : kDiodeTable) {
        if (iequals(name, e.name))
            return &e;
    }
    // Also try stripping leading 'd' or 'D' if present (e.g. D1N4148 -> 1N4148)
    if (name.size() > 1 && (name.front() == 'd' || name.front() == 'D')) {
        std::string_view stripped = name.substr(1);
        for (const auto& e : kDiodeTable) {
            if (iequals(stripped, e.name))
                return &e;
        }
    }
    return nullptr;
}

}  // namespace wdf
