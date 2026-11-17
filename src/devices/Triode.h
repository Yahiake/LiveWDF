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
///     ip = Vpk/Z      if Vpk < 0, Z = the plate port impedance
///
/// `Vpk >= 0` is the paper's own post-solve test: `triode_stage_quadric.m`
/// solves the quadric, reads the solved plate voltage back, and projects
/// when `Vpk < 0` -- replacing the tube with a junction wire of the
/// element's impedance. The decomposed network does not carry that
/// impedance separately -- `Z` is the impedance seen at the plate port,
/// everything the wire can spell -- so the resolution state here is an
/// impedance filter of it: every law in this engine takes the same-
/// iterate port voltage (the resistor row is `i = v/R`), so `ip = vpk/Z`
/// is Ohm's law read against `vpk = vo - Z*ip`, landing the solved
/// `Vpk` at the midpoint of the excursion. It is the analogue of the
/// paper's projection (`QuadricTriodeWdf::reflect`'s `Consistent`
/// flavour in wave space), and it always exists -- which is what keeps
/// the coupled decks solvable where a cold tube (no flow) would leave
/// no fixed point.
///
/// The law conducts plate current only, so neither grid port carries a current
/// of its own and the grid ports are passive: the triode group is exact in
/// closed form rather than a Newton circuit.
struct TriodeModel {
    QuadricTriodeParams p{};
    double cg = p.kPg / (2.0 * p.kP2);  // kPg/(2*kP2)
    double c0 = p.kP / (2.0 * p.kP2);   // kP/(2*kP2)
    double two_kP2 = 2.0 * p.kP2;

    void prepareQuadric() noexcept {
        cg = p.kPg / (2.0 * p.kP2);
        c0 = p.kP / (2.0 * p.kP2);
        two_kP2 = 2.0 * p.kP2;
    }

    /// Currents into the tube and their derivatives w.r.t. the port voltages.
    /// `ip` flows plate->cathode and is the only current a triode conducts; it
    /// answers to the INTERNAL grid only through that port's voltage, via `q`.
    /// `zeeWire` is the impedance seen at the plate port (`Z[o][o]`): the
    /// wire the out-of-domain quadric projects onto.
    struct Currents {
        double ip = 0.0;
        double dvgk = 0.0;  // dIp/dVgk_int
        double dvpk = 0.0;  // dIp/dVpk
    };

    Currents evaluate(double vgk, double vpk, double zeeWire = 0.0) const noexcept {
        Currents c;
        if (vpk < 0.0) {
            if (zeeWire > 0.0) {
                c.ip = vpk / zeeWire;
                c.dvpk = 1.0 / zeeWire;
            }
            return c;
        }
        return evaluateQuadric(vgk, vpk);
    }

    /// The quadric law proper, no domain test: the domain decision is the
    /// caller's. The engine decides it once per sample (`EngineSolve` pins
    /// the row) the way the paper tests the *solved* plate voltage; this
    /// is what the joint Newton rows run inside a pinned state. `evaluate`
    /// above is the device-level spelling of the paper's test.
    Currents evaluateQuadric(double vgk, double vpk) const noexcept {
        Currents c;
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
