#pragma once

// The Shockley diode, in closed form.
//
// This is the project's one nonlinear two-terminal device law, used by the
// netlist `D` element (`src/spice/Net.h`), where `rs` is the SPICE diode's own
// `RS`. `GridDiode` -- the alias the pentode's grid diode of `Tubes.lib` used
// to carry -- survives here as a name `test_diode.cpp` pins, but the imported
// pedalkernel pentode models its grid as an explicit exponential diode instead,
// so no device in the engine uses the alias anymore. The quadric triode carries
// no grid current and has no such diode.
//
// Both are one omega call and no iteration. The alternative -- the naive
// `i = Is*(exp(v/nVt) - 1)` -- is what a WDF diode usually gets, and it is the
// reason diode clippers are famous for diverging: the exponential's derivative
// outgrows any Newton step. The omega form is the *exact* solution of the
// series system, so there is nothing to diverge from.

#include <cmath>
#include <string_view>

#include "devices/WrightOmega.h"
#include "devices/KTable.h"

namespace wdf {

/// Thermal voltage at 27 degC with ngspice's constants (25.8649170 mV). Shared
/// by every Shockley-law device, which today is just the netlist diode.
inline constexpr double kThermalVoltage = 1.38064852e-23 * 300.15 / 1.6021766208e-19;

/// A Shockley diode with a series resistance -- SPICE's own junction law:
///
///     i = Is * (exp((v - i*Rs) / (n*Vt)) - 1)
///
/// which is implicit in `i`. Absorbing the series drop turns it into the omega
/// equation, so `eval` costs one `wrightOmega` call:
///
///     w = omega(z0 + v/(n*Vt)),  z0 = ln(Is*Rs/(n*Vt)) + Is*Rs/(n*Vt)
///     i   = (n*Vt/Rs)*w - Is
///     di  = w / (Rs*(1 + w))
///
/// Both branches are exact: at v = 0 the two terms cancel and `i` is exactly
/// zero; at large negative `v` the omega tail cancels `Is` and the current
/// saturates at `-Is`, as an ideal junction does.
struct DiodeModel {
    double isat = 1e-9;  // IS, amperes
    double n = 1.0;      // emission coefficient (ngspice's `N`)
    double rs = 1.0;     // series resistance, ohms (ngspice's `RS`)
    double nvt = 1.0;    // n * Vt, derived
    double z0 = 0.0;     // constant part of z; meaningless when rs == 0
    double vcrit = 0.0;  // the knee boundary `limit` works against (see there)
    double inv_nvt = (nvt > 0.0) ? (1.0 / nvt) : 0.0;
    double inv_rs = (rs > 0.0) ? (1.0 / rs) : 0.0;
    double nvt_over_rs = (rs > 0.0) ? (nvt / rs) : 0.0;
    double isat_over_nvt = (nvt > 0.0) ? (isat / nvt) : 0.0;

    /// `exp` of this overflows a double. Clamping the exponent past it keeps the
    /// law finite instead of producing `inf`/`NaN` for the Newton to chew on.
    static constexpr double kMaxExponent = 700.0;

    DiodeModel() = default;
    DiodeModel(double saturation, double emission, double series) noexcept
        : isat(saturation),
          n(emission),
          rs(series),
          nvt(emission * kThermalVoltage),
          z0(series > 0.0 ? std::log(saturation * series / (emission * kThermalVoltage)) +
                               saturation * series / (emission * kThermalVoltage)
                          : 0.0),
          vcrit(emission > 0.0 && saturation > 0.0
                    ? emission * kThermalVoltage *
                          std::log(emission * kThermalVoltage /
                                   (std::sqrt(2.0) * saturation))
                    : 0.0),
          inv_nvt(emission > 0.0 ? 1.0 / (emission * kThermalVoltage) : 0.0),
          inv_rs(series > 0.0 ? 1.0 / series : 0.0),
          nvt_over_rs(series > 0.0 ? (emission * kThermalVoltage) / series : 0.0),
          isat_over_nvt(emission > 0.0 ? saturation / (emission * kThermalVoltage) : 0.0) {}

    /// Current through the diode and its derivative, at the voltage across it
    /// (`v` positive = forward). Allocation-free and branch-predictable, so it
    /// is what the audio loop calls.
    void eval(double v, double& i, double& di) const noexcept {
        if (rs > 0.0) {
            const double w = wrightOmega(z0 + v * inv_nvt);
            i = nvt_over_rs * w - isat;
            di = (w * inv_rs) / (1.0 + w);
            return;
        }
        // Rs == 0: with no series resistance the law is explicit in `v`, and it
        // is the one case the omega form cannot reach (`z0` diverges as Rs -> 0).
        // The exponent is clamped at the overflow point and the law is continued
        // past it on its own tangent: still finite, still positive, still
        // Newton-stable. No reference diode comes near this -- it is a guard
        // against a bad iterate, not a working regime.
        const double x = v * inv_nvt;
        const double e = std::exp(x < kMaxExponent ? x : kMaxExponent);
        i = isat * (e - 1.0);
        di = isat_over_nvt * e;
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
    /// two halves to the rule:
    ///
    ///   * from the reverse side (`vold <= 0`), aim at `nVt*ln(vnew/nVt)`: the
    ///     voltage at which the exponential is O(1) in `vnew/nVt`. That is the
    ///     cold-start case. `solveDc` begins at the open-circuit voltage, which
    ///     on a plate supply is hundreds of volts; without this the first step
    ///     is ~1e-3 V and the solve never arrives.
    ///   * from the forward side, if the step would cross `vcrit` by more than
    ///     two thermal voltages, land on `vold + nVt*ln(1 + (vnew-vold)/nVt)`.
    ///     That is the same curve, logarithmically compressed -- the overshoot is
    ///     given up, not the progress.
    ///
    /// `limited` reports whether either fired, for the caller's diagnostics.
    double limit(double vold, double vnew, bool& limited) const noexcept {
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

/// The alias `devices/Pentode.h` carried for the pentode grid diode of `Tubes.lib`
/// -- the same Shockley law under a grid-flavoured name. The imported pedalkernel
/// pentode no longer uses it; `test_diode.cpp` still pins the alias.
using GridDiode = DiodeModel;

/// A symmetrical anti-parallel diode pair (e.g. clipping stage in TS808, Rat, Distortion+).
///
/// Current law for identical antiparallel junctions across terminal voltage v:
///     i_net = 2 * Is * sinh(v / nVt)
///
/// Under Thévenin port constraint v = vo - Rp * i_net, Werner's dual Wright Omega
/// estimate gives an excellent O(1) starting voltage:
///     b = a - 2*nVt * sgn(a) * [ omega(ln(R_eff*Is/nVt) + |a|/nVt) - omega(ln(R_eff*Is/nVt) - |a|/nVt) ]
/// where R_eff = Rp + Rs.
///
/// That estimate is asymptotic (it models both branches with a single shared Rs and
/// ignores the off-branch series drop), so it is polished with a fixed three Newton
/// corrections against the exact branch laws.  Like PedalKernel's explicit pair root
/// this is O(1) with no convergence loop; the final voltage satisfies the port
/// equation to machine precision, including unequal branch models.
struct AntiParallelDiodeModel {
    DiodeModel d1;
    DiodeModel d2;
    double isat = 1e-9;
    double n = 1.0;
    double rs = 0.0;
    double nvt = 1.0;
    bool symmetrical = true;
    double inv_nvt = 1.0;
    double two_isat_over_nvt = 2.0e-9;
    mutable double cachedRp = -1.0;
    mutable double cachedLogTerm = 0.0;
    mutable double cachedZ0Fast = 0.0;
    mutable double cachedZ0FastNeg = 0.0;
    mutable KTable1D kTable_;
    mutable double kTableRp_ = -1.0;

    AntiParallelDiodeModel() = default;
    AntiParallelDiodeModel(const DiodeModel& a, const DiodeModel& b) noexcept
        : d1(a), d2(b) {
        symmetrical = (std::abs(a.isat - b.isat) < 1e-15 * a.isat) &&
                      (std::abs(a.n - b.n) < 1e-6) &&
                      (std::abs(a.rs - b.rs) < 1e-6 * (a.rs + 1.0));
        isat = a.isat;
        n = a.n;
        rs = a.rs;
        nvt = a.nvt;
        inv_nvt = (nvt > 0.0) ? (1.0 / nvt) : 0.0;
        two_isat_over_nvt = (nvt > 0.0) ? (2.0 * isat / nvt) : 0.0;
    }

    /// Evaluates net current i_net = i1 - i2 and total differential conductance.
    void eval(double v, double& i_net, double& di) const noexcept {
        if (symmetrical && rs == 0.0) {
            const double x = v * inv_nvt;
            const double clampedX = std::clamp(x, -DiodeModel::kMaxExponent, DiodeModel::kMaxExponent);
            i_net = 2.0 * isat * std::sinh(clampedX);
            di = two_isat_over_nvt * std::cosh(clampedX);
            return;
        }
        double i1 = 0.0, di1 = 0.0, i2 = 0.0, di2 = 0.0;
        d1.eval(v, i1, di1);
        d2.eval(-v, i2, di2);
        i_net = i1 - i2;
        di = di1 + di2;
    }

    /// Solves the port equation v = vo - Rp * i_net to machine precision.
    ///
    /// Evaluates using precomputed K-Method cubic Hermite interpolation table
    /// with an optional single Newton polish step to guarantee machine precision
    /// (< 1e-12 V) in O(1) time without transcendentals or loops.
    ///
    /// Returns port voltage v and individual forward currents i1 (D1: A->B) and i2 (D2: B->A).
    void solvePort(double vo, double Rp, double& vOut, double& i1, double& i2) const noexcept {
        if (symmetrical) {
            if (vo == 0.0) {
                vOut = 0.0;
                i1 = 0.0;
                i2 = 0.0;
                return;
            }
            if (vo < 0.0) {
                solvePort(-vo, Rp, vOut, i2, i1);
                vOut = -vOut;
                return;
            }
        }

        if (Rp <= 0.0) {
            solvePortOmega(vo, Rp, vOut, i1, i2);
            return;
        }

        ensureTable(Rp);

        double v = kTable_.lookup(vo);
        double j1 = 0.0, dj1 = 0.0, j2 = 0.0, dj2 = 0.0;
        bool converged = false;

        // O(1) single/double scalar Newton polish to machine precision (< 1e-12 V).
        for (int iter = 0; iter < 2; ++iter) {
            d1.eval(v, j1, dj1);
            d2.eval(-v, j2, dj2);
            const double f = v - vo + Rp * (j1 - j2);
            if (std::abs(f) <= 1e-12 * (1.0 + std::abs(v))) {
                converged = true;
                break;
            }
            const double df = 1.0 + Rp * (dj1 + dj2);
            if (!(std::abs(df) > 1e-30) || !std::isfinite(df))
                break;
            v -= f / df;
            if (!std::isfinite(v))
                break;
        }

        if (converged) {
            vOut = v;
            i1 = j1;
            i2 = j2;
            return;
        }

        if (solvePortFast(vo, Rp, vOut, i1, i2))
            return;
        solvePortOmega(vo, Rp, vOut, i1, i2);
    }

private:
    /// Lazily precompute the K-Method 1D lookup table for the fixed port resistance Rp.
    void ensureTable(double Rp) const noexcept {
        if (kTableRp_ == Rp && kTable_.isReady())
            return;
        kTableRp_ = Rp;
        kTable_.build(-30.0, 30.0, 4097, [this, Rp](double vo) -> double {
            double vOut = 0.0, i1 = 0.0, i2 = 0.0;
            if (solvePortFast(vo, Rp, vOut, i1, i2))
                return vOut;
            solvePortOmega(vo, Rp, vOut, i1, i2);
            return vOut;
        });
    }
    /// Refresh the two single-forward-branch seeds used by the fast paths.
    /// `cachedZ0Fast` is D1 with Rp + Rs1; `cachedZ0FastNeg` is D2 with Rp + Rs2.
    /// Both are exact single-diode omega coordinates, so they are also valid
    /// seeds when one of the two branches has Rs == 0.
    void refreshFastCache(double Rp) const noexcept {
        const double r1 = Rp + d1.rs;
        cachedLogTerm = std::log(r1 * d1.isat / d1.nvt);
        cachedZ0Fast = cachedLogTerm + r1 * d1.isat / d1.nvt;
        const double r2 = Rp + d2.rs;
        cachedZ0FastNeg = std::log(r2 * d2.isat / d2.nvt) + r2 * d2.isat / d2.nvt;
        cachedRp = Rp;
    }

    /// Exact ideal/mixed-junction pair in one scalar Newton loop.
    bool solvePortIdeal(double vo, double Rp, double& vOut, double& i1, double& i2) const noexcept {
        double v;
        if (vo == 0.0) {
            v = 0.0;
        } else {
            if (cachedRp != Rp)
                refreshFastCache(Rp);
            if (vo > 0.0) {
                const double rEff = Rp + d1.rs;
                const double w = detail::omega4_seed(cachedZ0Fast + vo * d1.inv_nvt);
                if (std::isfinite(w) && w > 0.0) {
                    const double iFwd = (d1.nvt / rEff) * w - d1.isat;
                    v = vo - Rp * iFwd;
                } else {
                    v = vo;
                }
            } else {
                const double rEff = Rp + d2.rs;
                const double w =
                    detail::omega4_seed(cachedZ0FastNeg + (-vo) * d2.inv_nvt);
                if (std::isfinite(w) && w > 0.0) {
                    const double iFwd = (d2.nvt / rEff) * w - d2.isat;
                    v = vo + Rp * iFwd;
                } else {
                    v = vo;
                }
            }
        }

        double j1 = 0.0, j2 = 0.0;
        bool converged = false;
        for (int iter = 0; iter < 8; ++iter) {
            double dj1 = 0.0, dj2 = 0.0;
            d1.eval(v, j1, dj1);
            d2.eval(-v, j2, dj2);
            const double f = v - vo + Rp * (j1 - j2);
            if (std::abs(f) < 1e-12 * (1.0 + std::abs(v))) {
                converged = true;
                break;
            }
            const double df = 1.0 + Rp * (dj1 + dj2);
            if (!(std::abs(df) > 1e-30) || !std::isfinite(f) || !std::isfinite(df))
                return false;
            double dv = f / df;
            dv = std::clamp(dv, -0.5, 0.5);
            v -= dv;
            if (!std::isfinite(v))
                return false;
        }
        if (!converged)
            return false;

        vOut = v;
        i1 = j1;
        i2 = j2;
        return true;
    }

    /// Exact series-resistance pair in junction-voltage space.
    ///
    ///     i1 = Is1 (exp(u1/nVt1) - 1),  v  = u1 + Rs1 i1
    ///     i2 = Is2 (exp(u2/nVt2) - 1), -v  = u2 + Rs2 i2
    ///
    /// Eliminating v gives two equations for u1 and u2.  A single omega seed
    /// (no Halley refinement) puts the forward junction near its answer; the
    /// loop then uses only exp.  The result is validated by both voltage
    /// equations before it is accepted.
    bool solvePortFast(double vo, double Rp, double& vOut, double& i1, double& i2) const noexcept {
        if (!(d1.rs > 0.0 && d2.rs > 0.0))
            return solvePortIdeal(vo, Rp, vOut, i1, i2);

        double u1 = vo;
        double u2 = -vo;
        if (cachedRp != Rp)
            refreshFastCache(Rp);
        if (vo > 0.0) {
            // Forward branch with the whole source behind Rp + Rs.  This is the
            // exact single-diode solution when D2 is off, so it lands next to
            // the pair root even at large drive; one omega seed is enough.
            const double rEff = Rp + d1.rs;
            const double w = detail::omega4_seed(cachedZ0Fast + vo * d1.inv_nvt);
            if (std::isfinite(w) && w > 0.0) {
                const double iFwd = (d1.nvt / rEff) * w - d1.isat;
                const double vFwd = vo - Rp * iFwd;
                u1 = vo - rEff * iFwd;
                u2 = -vFwd;
            }
        } else if (vo < 0.0) {
            const double rEff = Rp + d2.rs;
            const double w = detail::omega4_seed(cachedZ0FastNeg + (-vo) * d2.inv_nvt);
            if (std::isfinite(w) && w > 0.0) {
                const double iFwd = (d2.nvt / rEff) * w - d2.isat;
                const double vFwd = (-vo) - Rp * iFwd;
                u2 = (-vo) - rEff * iFwd;
                u1 = -vFwd;
            }
        } else {
            u1 = 0.0;
            u2 = 0.0;
        }

        double j1 = 0.0, j2 = 0.0;
        double f1 = 0.0, f2 = 0.0;
        bool converged = false;
        for (int iter = 0; iter < 8; ++iter) {
            const double x1 = std::clamp(u1 * d1.inv_nvt, -DiodeModel::kMaxExponent,
                                         DiodeModel::kMaxExponent);
            const double x2 = std::clamp(u2 * d2.inv_nvt, -DiodeModel::kMaxExponent,
                                         DiodeModel::kMaxExponent);
            const double e1 = std::exp(x1);
            const double e2 = std::exp(x2);
            j1 = d1.isat * (e1 - 1.0);
            j2 = d2.isat * (e2 - 1.0);
            const double g1 = d1.isat_over_nvt * e1;
            const double g2 = d2.isat_over_nvt * e2;

            const double v1 = u1 + d1.rs * j1;
            f1 = v1 + u2 + d2.rs * j2;
            f2 = v1 - vo + Rp * (j1 - j2);
            if (std::max(std::abs(f1), std::abs(f2)) <
                1e-12 * (1.0 + std::abs(vo) + std::abs(u1) + std::abs(u2))) {
                converged = true;
                break;
            }

            const double a = 1.0 + d1.rs * g1;
            const double b = 1.0 + d2.rs * g2;
            const double c = 1.0 + (d1.rs + Rp) * g1;
            const double d = -Rp * g2;
            const double det = a * d - b * c;
            if (!(std::abs(det) > 1e-30) || !std::isfinite(det) ||
                !std::isfinite(f1) || !std::isfinite(f2))
                return false;

            const double du1 = (-f1 * d + b * f2) / det;
            const double du2 = (-a * f2 + c * f1) / det;
            if (!std::isfinite(du1) || !std::isfinite(du2))
                return false;

            bool limited = false;
            const double n1 = d1.limit(u1, u1 + du1, limited);
            const double n2 = d2.limit(u2, u2 + du2, limited);
            if (!std::isfinite(n1) || !std::isfinite(n2))
                return false;
            u1 = n1;
            u2 = n2;
        }
        if (!converged)
            return false;

        vOut = u1 + d1.rs * j1;
        i1 = j1;
        i2 = j2;
        return std::isfinite(vOut) && std::isfinite(i1) && std::isfinite(i2);
    }

    void solvePortOmega(double vo, double Rp, double& vOut, double& i1, double& i2) const noexcept {
        double v = vo;

        // O(1) warm start: Werner dual Wright Omega estimate for a symmetrical pair.
        if (symmetrical && Rp > 0.0) {
            const double rEff = Rp + rs;
            const double sgn = (vo > 0.0) ? 1.0 : ((vo < 0.0) ? -1.0 : 0.0);
            if (sgn != 0.0) {
                const double absA = std::abs(vo);
                double logTerm;
                if (cachedRp == Rp) {
                    logTerm = cachedLogTerm;
                } else {
                    logTerm = std::log(rEff * isat / nvt);
                    cachedRp = Rp;
                    cachedLogTerm = logTerm;
                }
                const double argPlus = logTerm + absA / nvt;
                const double argMinus = logTerm - absA / nvt;
                const double wPlus = wrightOmega(argPlus);
                const double wMinus = (argMinus > -50.0) ? wrightOmega(argMinus) : 0.0;
                const double b = vo - 2.0 * nvt * sgn * (wPlus - wMinus);
                const double iNet = (vo - b) / (2.0 * rEff);
                v = vo - Rp * iNet;
            }
        }

        // Fixed Newton corrections on the exact branch equations.
        for (int iter = 0; iter < 3; ++iter) {
            double j1 = 0.0, dj1 = 0.0, j2 = 0.0, dj2 = 0.0;
            d1.eval(v, j1, dj1);
            d2.eval(-v, j2, dj2);
            const double f = v - vo + Rp * (j1 - j2);
            if (std::abs(f) < 1e-12 * (1.0 + std::abs(v)))
                break;
            const double df = 1.0 + Rp * (dj1 + dj2);
            if (!(std::abs(df) > 1e-30) || !std::isfinite(f) || !std::isfinite(df))
                break;
            const double dv = f / df;
            v -= dv;
            if (std::abs(dv) < 1e-15 * (1.0 + std::abs(v)))
                break;
        }

        vOut = v;
        double di1 = 0.0, di2 = 0.0;
        d1.eval(vOut, i1, di1);
        d2.eval(-vOut, i2, di2);
    }
};

// --------------------------------------------------------------------------- //
// Named diode device registry (Cathodyne table, DAFx / SPICE models)
// --------------------------------------------------------------------------- //

/// One selectable diode model: a display name and its Shockley parameters.
struct DiodeEntry {
    const char* name;
    double isat;
    double n;
    double rs;
};

/// The 6 diode types Cathodyne offers (Cathodyne DiodeClipper.h / chowdsp_wdf).
inline constexpr DiodeEntry kDiodeTable[] = {
    {"1N4148", 4.0e-9, 1.0, 0.56},
    {"1N914",  4.0e-9, 1.0, 0.56},
    {"1N4007", 1.0e-6, 1.0, 0.04},
    {"1N34A",  1.0e-6, 1.0, 5.0},
    {"1N5819", 1.0e-6, 1.0, 0.05},
    {"GZ34",   2.52e-9, 1.0, 15.0},
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

