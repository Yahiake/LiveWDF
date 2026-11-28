#pragma once

#include <cmath>

#include "devices/QuadricTriode.h"

namespace wdf {

/// ln(1 + e^u) and 1/(1 + e^-u) with one exponential and matching branches.
/// `u` reaches +-600 at the extremes of the plate swing, so the naive forms
/// overflow where these do not. Shared with the Koren pentode.
inline void softplusSigmoid(double u, double& sp, double& sg) noexcept {
    if (u > 30.0) {
        const double e = std::exp(-u);
        sp = u + e;
        sg = 1.0 / (1.0 + e);
    } else if (u > 16.0) {
        const double e = std::exp(-u);
        sp = u + e * (1.0 - 0.5 * e);
        sg = 1.0 / (1.0 + e);
    } else if (u < -30.0) {
        const double e = std::exp(u);
        sp = e;
        sg = e / (1.0 + e);
    } else if (u < -16.0) {
        const double e = std::exp(u);
        sp = e * (1.0 - 0.5 * e);
        sg = e / (1.0 + e);
    } else {
        const double e = std::exp(u);
        sp = std::log1p(e);
        sg = e / (1.0 + e);
    }
}

/// The triode law used by the engine: the D'Angelo et al. DAFx-23 quadric
/// surface (`devices/QuadricTriode.h`) on the domain the paper states for
/// itself.
///
///     q  = Vpk + cg*Vgk + c0
///     ip = kP2*q^2    if Vpk >= 0 and q > 0
///     Vpk = 0         if the solved Vpk < 0 (plate shorted to cathode)
///
/// `Vpk >= 0` is the paper's own post-solve test: `triode_stage_quadric.m`
/// solves the quadric, reads the solved plate voltage back, and projects
/// when `Vpk < 0` onto `Vpk = 0` (`QuadricTriodeWdf::reflect`'s
/// `Consistent` flavour). The engine makes that test on each sample's solve:
/// with an open-circuit plate voltage `vo > 0` it lands as the short,
/// `ip = vo/Z`, and with `vo <= 0` as cold, `ip = 0`, so it never conducts
/// in reverse and meets the quadric's own root continuously on both sides.
///
/// The internal grid conducts to the cathode piecewise linearly,
///
///     ig = max(0, max over kGridLines of (Vgk - on) / r)
///
/// Inside each line the plate law stays one quadratic in `q`, so the group
/// keeps its closed form; a diode here costs a Newton solve per sample.
/// `gridConducts = false` is the plate-only law.
struct TriodeModel {
    /// The projected short is `kShort*Z`, not zero, because the Newton row is
    /// i(v) and needs a finite conductance; the closed form uses the same one
    /// so both solve one circuit.
    static constexpr double kShort = 1e-9;
    struct GridLine {
        double on, r;
    };
    // Dempwolf RSD-1 12AX7 grid curve fitted over -0.25..1.05 V where amp grids sit on a DI, the last line over 0.25..2.5 V.
    static constexpr GridLine kGridLines[] = {{-0.0745, 3494.0}, {0.0941, 1550.0}, {0.3, 1100.0}};

    QuadricTriodeParams p{};
    double cg = p.kPg / (2.0 * p.kP2);  // kPg/(2*kP2)
    double c0 = p.kP / (2.0 * p.kP2);   // kP/(2*kP2)
    double two_kP2 = 2.0 * p.kP2;
    double pieceScale = 1.0;
    bool pieceQuadric = false;
    bool gridConducts = true;

    void prepareQuadric() noexcept {
        cg = p.kPg / (2.0 * p.kP2);
        c0 = p.kP / (2.0 * p.kP2);
        two_kP2 = 2.0 * p.kP2;
    }

    void preparePieceQuadric(double scale = 1.0) noexcept {
        pieceScale = scale;
        pieceQuadric = true;
    }

    /// Currents into the tube and their derivatives w.r.t. the port voltages.
    /// `ip` flows plate->cathode and answers to the INTERNAL grid through that
    /// port's voltage, via `q`.
    struct Currents {
        double ip = 0.0;
        double dvgk = 0.0;  // dIp/dVgk_int
        double dvpk = 0.0;  // dIp/dVpk
    };

    double gridCurrent(double vgk, double& slope) const noexcept {
        double ig = 0.0;
        slope = 0.0;
        if (!gridConducts || vgk <= kGridLines[0].on)
            return 0.0;
        for (const GridLine& l : kGridLines)
            if (gridConducts && vgk - l.on > l.r * ig) {
                ig = (vgk - l.on) / l.r;
                slope = 1.0 / l.r;
            }
        return ig;
    }

    /// Beyond the forward grid conduction threshold `kGridLines[0].on` (-0.0745 V),
    /// the grid conducts with steep piecewise-linear conductance.
    /// The Newton solver taking raw steps from cutoff overshoots the knee by volts,
    /// triggering massive current oscillations between cutoff and forward saturation.
    /// This limiter compresses forward steps logarithmically (like SPICE pnjlim),
    /// ensuring the iterate stays near the conduction knee so subsequent Newton iterations
    /// receive accurate Jacobian slopes without oscillating.
    double limitGrid(double vold, double vnew, bool& limited) const noexcept {
        if (!gridConducts)
            return vnew;
        constexpr double on0 = kGridLines[0].on;
        constexpr double vt = 0.05;
        if (vold <= on0) {
            // Crossing into forward conduction from cutoff:
            // Damp large positive overshoot so Newton enters the conducting knee smoothly.
            if (vnew > on0 && (vnew - on0) > 2.0 * vt) {
                limited = true;
                const double arg = 1.0 + (vnew - on0) / vt;
                return on0 + vt * std::log(arg);
            }
        } else {
            // Already conducting: piecewise-linear region has finite conductance.
            // Only damp large runaway steps (> 0.5 V).
            if (vnew > vold && (vnew - vold) > 0.5) {
                limited = true;
                const double arg = 1.0 + (vnew - vold) / 0.2;
                return vold + 0.2 * std::log(arg);
            }
        }
        return vnew;
    }

    Currents evaluate(double vgk, double vpk) const noexcept {
        if (vpk < 0.0)
            return {};
        return evaluateQuadric(vgk, vpk);
    }

    /// The quadric law proper, no domain test: the domain decision is the
    /// caller's, made on the solved plate voltage the way the paper does;
    /// this is what the joint Newton rows run inside a pinned state. `evaluate`
    /// above is the device-level spelling of the paper's test.
    Currents evaluateQuadric(double vgk, double vpk) const noexcept {
        Currents c;
        if (pieceQuadric) {
            constexpr double a = 9.97167208, b = 3.4528332, cc = 1.40669992;
            constexpr double d = -3.12592106, e = 2.40056664, f = 1.71907587;
            constexpr double h = -7.98593702, joint = 100.43918363 / 250.0;
            const double x = vpk / 250.0, y = vgk / 2.0;
            const double dx = x - joint;
            const double upper = x > joint ? h * dx * dx : 0.0;
            const double q = a * x * x + b * x * y + cc * y * y + d * x + e * y + f + upper;
            if (q > 0.0) {
                c.ip = 0.001 * pieceScale * q;
                c.dvpk = 0.001 * pieceScale *
                    (2.0 * a * x + b * y + d + (x > joint ? 2.0 * h * dx : 0.0)) / 250.0;
                c.dvgk = 0.001 * pieceScale * (b * x + 2.0 * cc * y + e) / 2.0;
            }
            return c;
        }
        const double q = vpk + cg * vgk + c0;
        if (q > 0.0) {
            c.ip = p.kP2 * q * q;
            c.dvgk = p.kPg * q;
            c.dvpk = two_kP2 * q;
        }
        return c;
    }
};

/// Build a triode from a quadric parameter set. The paper's Table 3 12AX7 is
/// the default, so a default-constructed stage is the model the closed form
/// and the benchmarks were validated against.
inline TriodeModel quadricTriode(const QuadricTriodeParams& params = kQuadric12AX7) {
    TriodeModel t;
    t.p = params;
    t.prepareQuadric();
    return t;
}

/// The D'Angelo DAFx-23 12AX7 (paper Table 3), as a drop-in `TriodeModel`.
inline TriodeModel k12ax7Quadric() {
    return quadricTriode(kQuadric12AX7);
}

}  // namespace wdf
