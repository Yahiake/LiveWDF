#pragma once

#include <cmath>

namespace wdf {

/// Parameters of the DAFx-23 quadric-surface triode.
///
/// R. Giampiccolo, S. D'Angelo, A. Bernardini, A. Sarti, "A Quadric Surface
/// Model of Vacuum Tubes for Virtual Analog Applications", DAFx-23, paper 15.
///
/// `kP2` is the coefficient of `Vpk^2` -- it is NOT `kP` squared. Keep the three
/// names distinct (the paper's `kp2`/`kp` pair is a standing trap).
struct QuadricTriodeParams {
    double kP2 = 5.498e-8;  // A/V^2, coefficient of Vpk^2 (must be > 0)
    double kPg = 1.076e-5;  // A/V^2, Vgk*Vpk coupling
    double kP  = 1.014e-5;  // A/V,   coefficient of Vpk

    bool valid() const noexcept {
        return std::isfinite(kP2) && std::isfinite(kPg) && std::isfinite(kP) && kP2 > 0.0;
    }
};

/// 12AX7 working point of the paper's Table 3. Cross-checked against the 12AX7
/// datasheet at the paper's own operating point: Vgk=-2 V, Vpk=250 V gives
/// Ip = 1.18 mA (datasheet ~1.2 mA), so volts/amperes are the unit convention.
inline constexpr QuadricTriodeParams kQuadric12AX7{};

// ---------------------------------------------------------------------------
// Triode device registry.
//
// Most entries are copied from `Cathodyne/src/dsp/TriodeQuadricWDF.h:19-54`
// (GPLv3, recorded in THIRD-PARTY-NOTICES.md). Same model form as
// `QuadricTriodeLaw` above, but a *different fit* of it: Cathodyne re-fitted
// the three coefficients per tube rather than publishing the paper's set.
//
// `12AX7` is the exception: it uses the paper's Table 3 fit (`kQuadric12AX7`,
// {5.498e-8, 1.076e-5, 1.014e-5}) so that the default tube in a document
// agrees with the closed-form law and the DAFx-23 reference. The other 13
// entries keep Cathodyne's fits.
//
// `ParallelTriodeElement` uses the second table. The 12AX7 pair is exactly 2x
// the single-triode law; the other entries are Cathodyne's own parallel fits,
// which are not simply 2x (e.g. 6DJ8 is 1.772e-6 vs 2x8.853e-7 = 1.771e-6,
// so those fits have their own residual).
// ---------------------------------------------------------------------------

/// One selectable tube: a display name and its three quadric coefficients.
struct QuadricTriodeEntry {
    const char* name;
    QuadricTriodeParams p;
};

/// The 14 single-triode types, in Cathodyne's order so that a menu index maps
/// straight onto the table. The 12AX7 entry is the DAFx-23 fit (see above).
inline constexpr QuadricTriodeEntry kQuadricTriodeTable[] = {
    {"12AX7", kQuadric12AX7},
    {"12AX7A", {4.612e-08, 9.320e-06, 6.141e-06}},
    {"12AX7ASYL", {4.450e-08, 8.532e-06, 5.539e-06}},
    {"12AT7", {2.562e-07, 2.838e-05, 4.263e-05}},
    {"12AU7", {3.438e-07, 1.268e-05, 5.916e-05}},
    {"12AY7", {7.972e-08, 6.880e-06, 1.930e-05}},
    {"12AZ7", {2.465e-07, 2.743e-05, 3.979e-05}},
    {"12BH7A", {6.533e-07, 2.519e-05, 1.056e-04}},
    {"6AN8T", {3.436e-07, 1.267e-05, 5.914e-05}},
    {"6DJ8", {8.853e-07, 4.898e-05, 1.541e-04}},
    {"7025", {3.925e-08, 7.918e-06, 6.130e-06}},
    {"SV6N1P", {3.823e-07, 2.625e-05, 4.243e-05}},
    {"ECC83", {4.254e-08, 8.191e-06, 5.943e-06}},
    {"ECC81", {1.429e-07, 1.685e-05, 3.728e-05}},
};

/// The same 14 types wired as a matched parallel pair. 12AX7 is 2x the
/// single-triode fit; the rest are Cathodyne's separately fitted pairs.
inline constexpr QuadricTriodeEntry kQuadricParallelTriodeTable[] = {
    {"12AX7", {2.0 * kQuadric12AX7.kP2, 2.0 * kQuadric12AX7.kPg, 2.0 * kQuadric12AX7.kP}},
    {"12AX7A", {9.304e-08, 1.874e-05, 1.211e-05}},
    {"12AX7ASYL", {8.826e-08, 1.687e-05, 1.120e-05}},
    {"12AT7", {5.127e-07, 5.676e-05, 8.513e-05}},
    {"12AU7", {6.858e-07, 2.531e-05, 1.186e-04}},
    {"12AY7", {1.596e-07, 1.377e-05, 3.857e-05}},
    {"12AZ7", {4.921e-07, 5.477e-05, 7.962e-05}},
    {"12BH7A", {1.304e-06, 5.032e-05, 2.118e-04}},
    {"6AN8T", {6.856e-07, 2.530e-05, 1.186e-04}},
    {"6DJ8", {1.772e-06, 9.801e-05, 3.080e-04}},
    {"7025", {7.872e-08, 1.586e-05, 1.221e-05}},
    {"SV6N1P", {7.634e-07, 5.245e-05, 8.514e-05}},
    {"ECC83", {8.522e-08, 1.640e-05, 1.186e-05}},
    {"ECC81", {2.838e-07, 3.354e-05, 7.485e-05}},
};

/// Entry counts (both tables are the same 14 names, in the same order).
inline constexpr int kNumQuadricTriodes = 14;

/// Finds a tube by name ("12AX7", "ECC81", ...), case-sensitive. Returns
/// nullptr when the name is not in the table, so a caller can report a bad
/// selection instead of silently falling back to a default tube.
inline const QuadricTriodeParams* quadricTriodeByName(const char* name,
                                                      bool parallel = false) noexcept {
    if (name == nullptr)
        return nullptr;
    const auto& table = parallel ? kQuadricParallelTriodeTable : kQuadricTriodeTable;
    for (int i = 0; i < kNumQuadricTriodes; ++i) {
        const char* a = table[i].name;
        const char* b = name;
        while (*a != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        if (*a == '\0' && *b == '\0')
            return &table[i].p;
    }
    return nullptr;
}

/// Every table entry is usable as a device: finite coefficients and a positive
/// kP2 (the sign that keeps `ip = kP2*q^2` a current). Cheap enough to run at
/// startup or on preset load, and it is what catches a bad copy of the table.
inline bool quadricTriodeTablesValid() noexcept {
    for (const auto& e : kQuadricTriodeTable)
        if (!e.p.valid())
            return false;
    for (const auto& e : kQuadricParallelTriodeTable)
        if (!e.p.valid())
            return false;
    return true;
}
/// The memoryless plate-current law, paper Eq. 13, in completed-square form:
///
///     q  = Vpk + (kPg/(2*kP2))*Vgk + kP/(2*kP2)
///     ip = kP2*q^2   if q >= 0     (equivalently d(ipk)/d(Vpk) >= 0)
///     ip = 0         if q <  0
///     ig = 0, ik = -ip
///
/// The model assumes Vpk >= 0 and carries NO grid current. Both restrictions
/// are the model's, not approximations made here -- see the header of
/// `QuadricTriodeWdf` for what that costs in a driven stage.
struct QuadricTriodeLaw {
    QuadricTriodeParams p{};
    double cg = 0.0;  // kPg/(2*kP2)
    double c0 = 0.0;  // kP/(2*kP2)

    void prepare() noexcept {
        cg = p.kPg / (2.0 * p.kP2);
        c0 = p.kP / (2.0 * p.kP2);
    }

    /// Completed-square variable. The device conducts iff `q >= 0`.
    double q(double vpk, double vgk) const noexcept { return vpk + cg * vgk + c0; }

    /// Plate current and its two first derivatives (all zero at/below cutoff).
    /// dIp/dVpk = 2*kP2*q and dIp/dVgk = kPg*q follow from q being affine in
    /// both voltages; both are continuous at q = 0.
    double evaluate(double vpk, double vgk, double& dipdVpk, double& dipdVgk) const noexcept {
        const double qq = q(vpk, vgk);
        if (!(qq > 0.0)) {
            dipdVpk = 0.0;
            dipdVgk = 0.0;
            return 0.0;
        }
        dipdVpk = 2.0 * p.kP2 * qq;
        dipdVgk = p.kPg * qq;
        return p.kP2 * qq * qq;
    }
};

/// What the `Vpk<0` domain projection does to the cathode wave.
enum class QuadricProjection {
    /// The authors' published Octave/Python reference: replace `bp` only, and
    /// leave `bk` at its pre-projection value. This is NOT current-consistent
    /// (it leaves `ik != -ip` and does not actually land on Vpk = 0) -- see the
    /// selftest, which measures both. Kept as the default so the element
    /// reproduces the reference bit-for-bit.
    Reference,
    /// Project onto Vpk = 0 and keep `ik = -ip`. Self-consistent; the port
    /// relation survives, and the boundary is reached exactly.
    Consistent,
};

/// Explicit three-port WDF scattering for one quadric triode -- no Newton, no
/// iteration, one `sqrt` per call. Derived from the constitutive law and the
/// wave convention above (paper Sec. 5-6).
///
/// Ports are (plate, grid, cathode) against a shared reference, with scalar
/// port resistances Zp, Zg, Zk. Waves follow `a = v + Z*i`, `b = v - Z*i` with
/// `i` positive into the element, so `v = (a+b)/2`, `i = (a-b)/(2Z)`.
///
/// Because `ig = 0`, port 2 is trivial (`bg = ag`) and its port resistance is
/// never used -- the authors' reference passes it (R0g) and ignores it too.
/// The remaining two waves come from the scalar quadratic
///
///     2*Zp*kP2*q1*q^2 + q - S = 0,   q1 = (1+r)/2 + r*kPg/(4*kP2), r = Zk/Zp
///
/// solved in the cancellation-free rationalised form `q = 2S/(1 + sqrt(D))`.
///
/// `q1` is the `gamma` the paper prints in Eq. 23. The printed form drops the
/// 1/2 on `(1+r)`; the authors' own code states `k_eta = 1/(2*kP2*gamma)` in a
/// comment, and inverting that gives exactly the `q1` above, so the derivation
/// used here -- and the printed Eq. 23 -- are reconciled in favour of `q1`.
struct QuadricTriodeWdf {
    QuadricTriodeParams p{};
    double Zg = 1.0e6;  // grid port resistance; unused (ig = 0)
    double Zk = 1000.0;
    double Zp = 39000.0;
    QuadricProjection projection = QuadricProjection::Reference;

    // Derived once in prepare().
    double cg = 0.0, c0 = 0.0;   // law coefficients
    double r = 0.0;              // Zk/Zp
    double q1 = 0.0;             // gamma
    double kD = 0.0;             // 8*Zp*kP2*q1
    double twoZpKp2 = 0.0;       // 2*Zp*kP2
    double h1mr = 0.0, hr = 0.0; // (1-r)/2, r/2
    double projA = 0.0, projB = 0.0;
    bool ok = false;

    /// Validate the fixed parameters and precompute the ratios. Returns false
    /// (and leaves the element in open-circuit fallback) if the model is not
    /// usable -- the failure is a setup error, not a signal condition.
    bool prepare() noexcept {
        ok = false;
        QuadricTriodeLaw law{p};
        law.prepare();
        cg = law.cg;
        c0 = law.c0;
        if (!p.valid() || !(Zp > 0.0) || !(Zk > 0.0))
            return false;
        r = Zk / Zp;
        q1 = 0.5 * (1.0 + r) + 0.5 * r * cg;
        const double inv = 1.0 / (Zp + Zk);
        projA = (Zk - Zp) * inv;
        projB = 2.0 * Zp * inv;
        kD = 8.0 * Zp * p.kP2 * q1;
        twoZpKp2 = 2.0 * Zp * p.kP2;
        h1mr = 0.5 * (1.0 - r);
        hr = 0.5 * r;
        // q1 > 0 is required by the stable-root branch and the cutoff logic.
        ok = q1 > 0.0 && std::isfinite(q1) && std::isfinite(kD);
        return ok;
    }

    /// Scattering: incident (ag, ak, ap) -> reflected (bg, bk, bp).
    /// `bg = ag` on every path, including the fallbacks.
    void reflect(double ag, double ak, double ap, double& bg, double& bk, double& bp,
                 int* branch = nullptr) const noexcept {
        bg = ag;
        if (!ok) {
            bk = ak;
            bp = ap;
            if (branch)
                *branch = 3;
            return;
        }
        // q = q0 + q1*bp
        const double q0 = h1mr * ap - ak + cg * (ag - ak - hr * ap) + c0;
        const double S = q0 + q1 * ap;
        const double D = 1.0 + kD * S;

        double b1 = ap, b3 = ak;
        double vpk = ap - ak;
        int br = 1;  // negative discriminant / q < 0: zero current, both ports open
        if (D >= 0.0) {
            const double qq = 2.0 * S / (1.0 + std::sqrt(D));
            if (qq >= 0.0) {
                b1 = ap - twoZpKp2 * qq * qq;
                b3 = ak + r * (ap - b1);
                vpk = 0.5 * (ap + b1) - ak - hr * (ap - b1);
                br = 0;
            }
        }
        // The Vpk >= 0 domain clamp sits *after* both branches in the
        // reference, so it applies to the open-circuit case too.
        if (vpk < 0.0) {
            b1 = projA * ap + projB * ak;
            if (projection == QuadricProjection::Consistent)
                b3 = ak + r * (ap - b1);
            br = 2;
        }
        bk = b3;
        bp = b1;
        if (branch)
            *branch = br;
    }

    /// Port voltage and current of a wave pair: v = (a+b)/2, i = (a-b)/(2Z).
    static void portOhm(double Z, double a, double b, double& v, double& i) noexcept {
        v = 0.5 * (a + b);
        i = (a - b) / (2.0 * Z);
    }
};

/// Small-signal parameters of the quadric triode at a bias point, for `.ac`
/// cross-checks and gain sanity: `gm = dIp/dVgk`, `gds = dIp/dVpk` (an output
/// *conductance*; `rout()` is the plate resistance).
struct QuadricTriodeSmallSignal {
    double ip = 0.0, gm = 0.0, gds = 0.0;
    double rout() const noexcept { return gds > 0.0 ? 1.0 / gds : 1e30; }
};

inline QuadricTriodeSmallSignal quadricSmallSignal(const QuadricTriodeParams& p, double vpk, double vgk) noexcept {
    QuadricTriodeLaw law{p};
    law.prepare();
    double dipdVpk = 0.0, dipdVgk = 0.0;
    QuadricTriodeSmallSignal ss;
    ss.ip = law.evaluate(vpk, vgk, dipdVpk, dipdVgk);
    ss.gm = dipdVgk;
    ss.gds = dipdVpk;
    return ss;
}

/// DC bias of the paper's Sec. 8 common-cathode stage, in closed form -- the
/// authors' own initial-condition solve. Reproduces the Octave `triode_stage_quadric`
/// starting state, so a stage can be initialised at its operating point instead
/// of settling from zero (which is what the reference does).
struct QuadricCommonCathodeBias {
    double vk = 0.0, vp = 0.0;
};

/// The authors' DC bias (their `Vk0`/`Vp0` closed form) is only the right
/// starting state when the bias voltage keeps the quadric inside its own
/// domain -- `delta0 = kP2*(kPg/(2*kP2) + Rp/Rk + 1 + 2*E)^2 - 2*(1 + Rp/Rk)
/// >= 0` in their Sec. 7 terms, which is what their `Vpk >= 0` post-solve
/// test keeps. Below it the bias lands out of the domain (the paper's Table
/// 4 stage sits at delta0 ~ 2.7e4 with E = 250 V; E = 25 V gives delta0 ~
/// -1.9), the projection is live at the bias itself, and the paper never
/// claims the closed form there.
inline QuadricCommonCathodeBias quadricCommonCathodeBias(const QuadricTriodeParams& p, double E, double Rp,
                                                         double Rk) noexcept {
    const double k1 = p.kPg / (2.0 * p.kP2) + Rp / Rk + 1.0;
    const double k2 = k1 * (p.kP / p.kP2 + 2.0 * E) * p.kP2;
    const double k3 = Rk * k2 + 1.0;
    const double s = k1 < 0.0 ? -1.0 : (k1 > 0.0 ? 1.0 : 0.0);
    QuadricCommonCathodeBias b;
    b.vk = (k3 - s * std::sqrt(2.0 * k3 - 1.0)) / (2.0 * Rk * k1 * k1 * p.kP2);
    b.vp = E - Rp / Rk * b.vk;
    return b;
}

}  // namespace wdf
