#pragma once

#include <algorithm>
#include <cmath>

#include <string>

#include "devices/Triode.h"

namespace wdf {

/// Currents of one pentode (or a matched pair) and their derivatives with
/// respect to the port voltages (control grid G1 = g-k, plate P = p-k).
/// Currents flow into the grid and plate and out of the cathode.
///
/// The imported pedalkernel three-port law has no screen current: the
/// screen voltage that enters E1 is the model-table constant `vg2`, not
/// a live port voltage, so `is`, every `dis_*` and `dip_dvsk` are
/// identically zero, and the second `eval` argument is unused.
struct PentodeCurrents {
    double ig = 0.0, is = 0.0, ip = 0.0;
    double dig_dvgk = 0.0;                                    // dIg/dVsk = dIg/dVpk = 0
    double dis_dvgk = 0.0, dis_dvsk = 0.0, dis_dvpk = 0.0;
    double dip_dvgk = 0.0, dip_dvsk = 0.0, dip_dvpk = 0.0;
};

/// Koren pentode imported wholesale from pedalkernel's `PentodeThreePort`
/// (`pedalkernel-rt/src/elements/nonlinear/pentode.rs`, parameters from
/// `pedalkernel/models/pentodes.model`, GND-referenced variant with
/// `v_offset = 0`):
///
///     E1    = kp*(1/mu + Vgk/vg2)  // vg2: the table's fixed screen constant
///     base  = (vg2/kp)*softplus(E1)
///     Ip    = ((base^ex)/kg1)*max(atan(Vp/kvb), 0)  // Vp = min(Vpk, v_max)
///     Is    = 0
///     Ig    = grid_is*(exp(clamp(Vgk/grid_vt, -500, 500)) - 1)
///
/// pedalkernel guards `vpk <= 0` / `vg2 <= 0` / `base <= 0` / `ip <= 0` with
/// Ip = 0 and floors `dip_dvpk` at LEAKAGE_CONDUCTANCE (1e-12 S) on both the
/// guarded and the normal path, so Newton always sees a positive diagonal
/// there. `vp = min(Vpk, v_max)` carries pedalkernel's (0, v_max) plate-port
/// clamp into the law; the lower bound is the `vpk <= 0` guard. `kvb2` and
/// `rp` are carried as pedalkernel table constants; the three-port law
/// never reads them.
///
/// The grid is pedalkernel's bare exponential -- no series resistance and no
/// `Tubes.lib` junction. Its Newton guard is pedalkernel's own `pnjlim`
/// (`pedalkernel-rt/src/elements/nonlinear/bjt.rs`), exposed as `limitGrid`
/// and applied to this device's grid port by the engine. A limiter moves the
/// iterate, not the law: converged, it is the same exponential root.
///
/// `count` scales every current and derivative (2 = a matched pair with
/// identical drive, ampsim2 SPEC 4.2 B6).
struct Pentode {
    double mu = 11.0, ex = 1.35, kg1 = 650.0, kg2 = 4200.0, kp = 60.0, kvb = 24.0;
    double kvb2 = 18.0;  // pedalkernel table constant; unused by the three-port law
    double vg2 = 470.0;  // fixed screen voltage (Vg2k) that enters E1
    double rp = 15000.0; // rated power (table metadata); unused by the law
    double v_max = 500.0;
    double count = 1.0;
    // pedalkernel `PentodeThreePort::grid_iv` constants.
    double grid_is = 1e-9;
    double grid_vt = 0.025;
    // `pnjlim`'s critical voltage, same expression as `limitGrid` computes but
    // hoisted to construction: `grid_is`/`grid_vt` are table constants that no
    // code mutates, so hoisting is exact and turns a per-Newton-iteration `log`
    // into a load. It must stay the last member: the aggregate initializers
    // (`kEL34Pair{...}`) end before these two, and an in-class initializer
    // reads the members above it, so it sees the constructed values.
    double grid_vcrit = grid_vt * std::log(grid_vt / (1.4142135623730950488 * (grid_is > 1e-30 ? grid_is : 1e-30)));
    double e1_0 = (mu > 0.0) ? (kp / mu) : 0.0;
    double e1_g = (vg2 > 0.0) ? (kp / vg2) : 0.0;
    double vg2_over_kp = (kp > 0.0) ? (vg2 / kp) : 0.0;
    double inv_kvb = (kvb > 0.0) ? (1.0 / kvb) : 0.0;
    double inv_kg1 = (kg1 > 0.0) ? (1.0 / kg1) : 0.0;
    double inv_grid_vt = (grid_vt > 0.0) ? (1.0 / grid_vt) : 0.0;
    double grid_is_over_vt = (grid_vt > 0.0) ? (grid_is / grid_vt) : 0.0;

    PentodeCurrents eval(double vgk, double /*vsk*/, double vpk) const noexcept {
        PentodeCurrents c;

        // Grid: pedalkernel's explicit exponential diode, exactly as in
        // `grid_iv` (exponent clamped to +-500 there too). Below -37*vt
        // (x < -37), exp(x) < 1e-16 is strictly zero in double precision.
        const double x = vgk * inv_grid_vt;
        if (x < -37.0) {
            c.ig = -grid_is;
            c.dig_dvgk = 0.0;
        } else {
            const double xClamped = (x > 500.0) ? 500.0 : x;
            const double ev = std::exp(xClamped);
            c.ig = grid_is * (ev - 1.0);
            c.dig_dvgk = ev * grid_is_over_vt;
        }

        // Plate. The LEAKAGE_CONDUCTANCE floor (`leak`) keeps the plate
        // unknown's derivative positive through every guard, as in pedalkernel.
        constexpr double leak = 1e-12;
        const double vp = (vpk > v_max) ? v_max : vpk;
        if (vp > 0.0 && vg2 > 0.0) {
            const double e1 = e1_0 + e1_g * vgk;
            double sp, sg;
            softplusSigmoid(e1, sp, sg);
            const double base = vg2_over_kp * sp;
            if (base > 0.0) {
                const double ip_base = std::pow(base, ex);
                const double r = vp * inv_kvb;
                const double a = std::max(std::atan(r), 0.0);
                const double ip_over_kg1 = ip_base * inv_kg1;
                c.ip = ip_over_kg1 * a;
                if (c.ip > 0.0) {
                    const double d_pf = inv_kvb / (1.0 + r * r);
                    const double dvpk = ip_over_kg1 * d_pf;
                    c.dip_dvpk = (dvpk > leak) ? dvpk : leak;
                    c.dip_dvgk = ex * c.ip * sg / base;
                } else {
                    c.dip_dvpk = leak;
                }
            } else {
                c.dip_dvpk = leak;
            }
        } else {
            c.dip_dvpk = leak;
        }

        if (count != 1.0) {
            c.ig *= count;
            c.is *= count;
            c.ip *= count;
            c.dig_dvgk *= count;
            c.dis_dvgk *= count;
            c.dis_dvsk *= count;
            c.dis_dvpk *= count;
            c.dip_dvgk *= count;
            c.dip_dvsk *= count;
            c.dip_dvpk *= count;
        }
        return c;
    }

    /// pedalkernel's `pnjlim` for this pentode's control grid, ported verbatim
    /// from `pedalkernel-rt/src/elements/nonlinear/bjt.rs`:
    ///
    ///     vcrit = vt*ln(vt / (sqrt(2)*is))
    ///
    /// Beyond `vcrit` the exponential's conductance outgrows any Newton step.
    /// The limiter moves the *iterate* in log space (bounding the current
    /// ratio), never the law: at convergence it leaves the same exponential
    /// root. `limited` reports whether it fired, for the caller's diagnostics.
    double limitGrid(double vold, double vnew, bool& limited) const noexcept {
        const double vcrit = grid_vcrit;
        if (vnew > vcrit && std::abs(vnew - vold) > 2.0 * grid_vt) {
            limited = true;
            if (vold > 0.0) {
                const double arg = 1.0 + (vnew - vold) / grid_vt;
                return arg > 0.0 ? vold + grid_vt * std::log(arg) : vcrit;
            }
            return grid_vt * std::log(std::max(vnew / grid_vt, 1e-30));
        }
        return vnew;
    }
};

/// EL34: pedalkernel's row `MU=11 EX=1.35 KG1=650 KG2=4200 KP=60 KVB=24 KVB2=18
/// VG2=470 RP=15000`.
inline const Pentode kEL34{};
/// Matched EL34 pair with identical drive: every current doubled.
inline const Pentode kEL34Pair{11.0, 1.35, 650.0, 4200.0, 60.0, 24.0, 18.0, 470.0, 15000.0, 500.0, 2.0};

/// 6L6GC: pedalkernel's row `MU=8.7 EX=1.35 KG1=1460 KG2=4500 KP=48 KVB=12
/// KVB2=25 VG2=450 RP=33000`.
inline const Pentode k6L6GC{8.7, 1.35, 1460.0, 4500.0, 48.0, 12.0, 25.0, 450.0, 33000.0, 500.0, 1.0};
/// Matched 6L6GC pair.
inline const Pentode k6L6GCPair{8.7, 1.35, 1460.0, 4500.0, 48.0, 12.0, 25.0, 450.0, 33000.0, 500.0, 2.0};

/// EL84: pedalkernel's row `MU=19 EX=1.35 KG1=650 KG2=4200 KP=600 KVB=300
/// KVB2=20 VG2=250 RP=38000`.
inline const Pentode kEL84{19.0, 1.35, 650.0, 4200.0, 600.0, 300.0, 20.0, 250.0, 38000.0, 500.0, 1.0};
/// Matched EL84 pair.
inline const Pentode kEL84Pair{19.0, 1.35, 650.0, 4200.0, 600.0, 300.0, 20.0, 250.0, 38000.0, 500.0, 2.0};

/// 6V6S: pedalkernel's 6V6GT row `MU=12 EX=1.3 KG1=1100 KG2=4500 KP=300 KVB=150
/// KVB2=18 VG2=315 RP=52000`.
inline const Pentode k6V6S{12.0, 1.3, 1100.0, 4500.0, 300.0, 150.0, 18.0, 315.0, 52000.0, 500.0, 1.0};
/// Matched 6V6S pair.
inline const Pentode k6V6SPair{12.0, 1.3, 1100.0, 4500.0, 300.0, 150.0, 18.0, 315.0, 52000.0, 500.0, 2.0};

/// KT66: pedalkernel's 5881/KT66 alias row (the 6L6GC parameters).
inline const Pentode kKT66{8.7, 1.35, 1460.0, 4500.0, 48.0, 12.0, 25.0, 450.0, 33000.0, 500.0, 1.0};

/// KT77: pedalkernel's KT77 alias row (the EL34 parameters).
inline const Pentode kKT77{11.0, 1.35, 650.0, 4200.0, 60.0, 24.0, 18.0, 470.0, 15000.0, 500.0, 1.0};

/// Named entry in the pentode registry.
struct NamedPentode {
    const char* name;
    const Pentode* model;
};

inline const NamedPentode kPentodeTable[] = {
    {"EL34", &kEL34},
    {"EL34PAIR", &kEL34Pair},
    {"6L6", &k6L6GC},
    {"6L6GC", &k6L6GC},
    {"6L6GCPAIR", &k6L6GCPair},
    {"EL84", &kEL84},
    {"EL84PAIR", &kEL84Pair},
    {"6V6", &k6V6S},
    {"6V6S", &k6V6S},
    {"6V6SPAIR", &k6V6SPair},
    {"KT66", &kKT66},
    {"KT77", &kKT77},
};

/// Lookup a pentode model by name (case-insensitive ASCII). Returns nullptr
/// if not found.
inline const Pentode* findPentodeByName(const std::string& name) {
    auto equalCi = [&name](const char* s) {
        size_t len = 0;
        while (s[len]) ++len;
        if (len != name.size())
            return false;
        for (size_t i = 0; i < len; ++i) {
            const char ca = (s[i] >= 'a' && s[i] <= 'z') ? static_cast<char>(s[i] - 32) : s[i];
            const char cb = (name[i] >= 'a' && name[i] <= 'z') ? static_cast<char>(name[i] - 32) : name[i];
            if (ca != cb)
                return false;
        }
        return true;
    };
    for (const auto& entry : kPentodeTable) {
        if (equalCi(entry.name))
            return entry.model;
    }
    return nullptr;
}

}  // namespace wdf
