#ifndef WDF_BJT_H
#define WDF_BJT_H

// BJT Wave Digital Model based on:
// Bernardini, Vergani, Sarti (2020) — CSSP 39, 2020.
// DOI: 10.1007/s00034-019-01331-7
//
// Ebers-Moll transport (IS/BF/BR/NF/NR), mirrored for PNP. Opt-in MiSTer
// cards add low-current Gummel-Poon corrections: Early effect (VAF, VAR)
// and base-emitter recombination (ISE, NE). The model has no junction
// charge, high-current base charge or series parasitics.

#include <cmath>
#include <string_view>

#include "devices/Diode.h"

namespace wdf {

namespace solver { class RootResidual; }

enum class BjtRegion {
    Invalid,
    Cutoff,
    ForwardActive,
    Saturation,
    ReverseActive,
};

inline BjtRegion classifyRegion(double vbe, double vbc) noexcept {
    const double vth = 5.0 * 25.86e-3; // ~5*Vt @ 300K
    bool be = vbe > vth;
    bool bc = vbc > vth;
    if (!be && !bc) return BjtRegion::Cutoff;
    if (be && !bc) return BjtRegion::ForwardActive;
    if (be && bc) return BjtRegion::Saturation;
    return BjtRegion::ReverseActive;
}

// Where an Ebers-Moll exponential argument stops being a number the solver
// can use: at 40 the junction current is already Is * e^40. The law caps
// there and `EngineCompile`'s DC Newton step is held to the voltage that
// argument corresponds to, so the constant lives here once.
inline constexpr double kBJtMaxExpArg = 40.0;

// ---------------------------------------------------------------------------
// Ebers-Moll BJT model (five transport parameters)
// ---------------------------------------------------------------------------
struct BjtModel {
    double Is = 1e-14;     // IS transport saturation current (A)
    double Bf = 100.0;     // BF forward current gain (beta_F)
    double Br = 1.0;       // BR reverse current gain (beta_R)
    double Nf = 1.0;       // NF forward ideality factor
    double Nr = 1.0;       // NR reverse ideality factor
    double Vt = 25.85e-3;  // Thermal voltage (V) at 300.15K

    // --- Polarity ---
    bool pnp = false;      // false: NPN; true: PNP (mirrored law)
    double Vaf = 0.0;      // zero disables Early effect in this direction
    double Var = 0.0;
    double Ise = 0.0;      // optional base-emitter recombination current
    double Ne = 1.5;
    double Ikf = 0.0;      // zero disables forward high-injection gain roll-off
    double cbe = 0.0;      // constant base-emitter capacitance (F)
    double cbc = 0.0;      // constant base-collector capacitance (F)

    // --- Derived constants (computed in prepare()) ---
    double nfVt = 0.0;
    double nrVt = 0.0;
    double alphaF = 0.0;
    double alphaR = 0.0;

    // Junction limiters for the engine's DC Newton step (`pnjlim`). These are
    // solver infrastructure, not model parameters: they hold the same Is and
    // ideality factor the Ebers-Moll law uses.
    DiodeModel diode{Is, Nf, 0.0};    // base-emitter junction
    DiodeModel diodeBc{Is, Nr, 0.0};  // base-collector junction

    BjtModel() = default;

    BjtModel(double Is_, double Bf_, double Br_, double Nf_, double Nr_,
             bool pnp_ = false, double Vt_ = 25.85e-3)
        : Is(Is_), Bf(Bf_), Br(Br_), Nf(Nf_), Nr(Nr_), Vt(Vt_), pnp(pnp_) {
        prepare();
    }

    void prepare() noexcept {
        nfVt = Nf * Vt;
        nrVt = Nr * Vt;
        alphaF = Bf / (Bf + 1.0);
        alphaR = Br / (Br + 1.0);
        diode = DiodeModel(Is, Nf, 0.0);
        diodeBc = DiodeModel(Is, Nr, 0.0);
    }

    bool valid() const noexcept {
        return std::isfinite(Is) && Is > 0.0 &&
               std::isfinite(Bf) && Bf > 0.0 &&
               std::isfinite(Br) && Br > 0.0 &&
               std::isfinite(Nf) && Nf > 0.0 &&
               std::isfinite(Nr) && Nr > 0.0 &&
               std::isfinite(Vaf) && Vaf >= 0.0 &&
               std::isfinite(Var) && Var >= 0.0 &&
               std::isfinite(Ise) && Ise >= 0.0 &&
               std::isfinite(Ne) && Ne > 0.0 &&
               std::isfinite(Ikf) && (Ikf == 0.0 || Ikf > 4.0 * Is) &&
               std::isfinite(cbe) && cbe >= 0.0 &&
               std::isfinite(cbc) && cbc >= 0.0 &&
               std::isfinite(Vt) && Vt > 0.0;
    }

    bool hasLowCurrentCorrections() const noexcept {
        return Vaf > 0.0 || Var > 0.0 || Ise > 0.0;
    }

    // -------------------------------------------------------------------------
    // Modified Newton-Raphson junction compensation (Bernardini et al.,
    // Section 4.2, eq. 66). This is NOT pnjlim -- it is the MNR compensation
    // for the Newton step. Leaves a junction voltage below the threshold
    // alone, and logarithmically compresses any proposed value above it.
    // `thr` is the voltage at which the junction current is 1 A, so
    // `exp(thr/nVt) - 1 = 1/Is`.
    // -------------------------------------------------------------------------
    double compensateJunction(double v, double nVt) const noexcept {
        if (!(nVt > 0.0) || !(Is > 0.0) || v <= 0.0) return v;
        const double thr = nVt * std::log1p(1.0 / Is);
        if (v <= thr) return v;
        return nVt * std::log1p(v / (thr * Is));
    }
    double compensateBe(double vbe) const noexcept {
        return pnp ? -compensateJunction(-vbe, nfVt)
                   : compensateJunction(vbe, nfVt);
    }
    double compensateBc(double vbc) const noexcept {
        return pnp ? -compensateJunction(-vbc, nrVt)
                   : compensateJunction(vbc, nrVt);
    }

    // -------------------------------------------------------------------------
    // DAFx-23 logarithmic step damping (Massi, Giampiccolo, Bernardini, Sarti,
    // DAFx23, Section 3.1, eq. 15-16).
    // Damps large proposed Newton voltage steps before they can overshoot the
    // steep exponential junction curve:
    //   |dv| <= 2*nVt: dv (linear, untouched)
    //   |dv| > 2*nVt: sign(dv) * (2*nVt + nVt * ln(1 + (|dv| - 2*nVt)/nVt))
    // This is C^1 smooth (both value and first derivative match at 2*nVt).
    // -------------------------------------------------------------------------
    static double dampStep(double dv, double nVt) noexcept {
        if (!(nVt > 0.0)) return dv;
        const double limit = 2.0 * nVt;
        const double mag = std::abs(dv);
        if (mag <= limit) return dv;
        const double s = (dv >= 0.0) ? 1.0 : -1.0;
        return s * (limit + nVt * std::log1p((mag - limit) / nVt));
    }
    double dampBeStep(double dvbe) const noexcept {
        return dampStep(dvbe, nfVt);
    }
    double dampBcStep(double dvbc) const noexcept {
        return dampStep(dvbc, nrVt);
    }

    // -------------------------------------------------------------------------
    // DAFx-23 Modified Newton-Raphson update for (vce, vbe) given raw steps
    // (dvce, dvbe). Evaluates the damped step and applies the compensation
    // functions phi_1 and phi_2 on the base-emitter and base-collector junctions,
    // returning the damped (vceNext, vbeNext).
    // -------------------------------------------------------------------------
    void applyMNRStep(double vce, double vbe, double dvce, double dvbe,
                      double& vceNext, double& vbeNext) const noexcept {
        const double p = pnp ? -1.0 : 1.0;
        const double vbe_dev = p * vbe;
        const double vbc_dev = p * (vbe - vce);
        const double dvbe_dev = p * dvbe;
        const double dvbc_dev = p * (dvbe - dvce);

        // 1. DAFx-23 logarithmic step damping
        const auto dampJunction = [](double v, double dv, double nVt) {
            // A step that stays reverse-biased cannot overshoot the
            // exponentially rising forward-current region.
            return v <= 0.0 && v + dv <= 0.0 ? dv : dampStep(dv, nVt);
        };
        const double dvbe_d = dampJunction(vbe_dev, dvbe_dev, nfVt);
        const double dvbc_d = dampJunction(vbc_dev, dvbc_dev, nrVt);

        // 2. Proposed trial voltages
        const double vbe_trial = vbe_dev + dvbe_d;
        const double vbc_trial = vbc_dev + dvbc_d;

        // 3. Compensation functions phi_1 and phi_2
        const double vbe_comp = compensateJunction(vbe_trial, nfVt);
        const double vbc_comp = compensateJunction(vbc_trial, nrVt);

        // 4. Return in circuit coordinates
        vbeNext = p * vbe_comp;
        const double vbcNext = p * vbc_comp;
        vceNext = vbeNext - vbcNext;
    }

    // -------------------------------------------------------------------------
    // Core Ebers-Moll transport equations with analytical Jacobian.
    // All voltages and currents are in the NPN frame.
    // Returns (ic, ib, [dib/dvbe, dib/dvbc, dic/dvbe, dic/dvbc]).
    // -------------------------------------------------------------------------
    struct CurrentsAndJacobian {
        double ic = 0.0, ib = 0.0;
        double dib_dvbe = 0.0, dib_dvbc = 0.0;
        double dic_dvbe = 0.0, dic_dvbc = 0.0;
        bool valid = false;
    };

    CurrentsAndJacobian evaluateEM(double vbe, double vbc) const noexcept {
        return valid() ? evaluateValidatedEM(vbe, vbc) : CurrentsAndJacobian{};
    }

private:
    CurrentsAndJacobian evaluateValidatedEM(double vbe, double vbc) const noexcept {
        CurrentsAndJacobian out{};
        if (!std::isfinite(vbe) || !std::isfinite(vbc))
            return out;
        if (!(nfVt > 0.0) || !(nrVt > 0.0))
            return out;

        const double beArg = vbe / nfVt;
        const double bcArg = vbc / nrVt;
        const bool beCapped = beArg >= kBJtMaxExpArg;
        const bool bcCapped = bcArg >= kBJtMaxExpArg;
        const double eBe = std::exp(beCapped ? kBJtMaxExpArg : beArg);
        const double eBc = std::exp(bcCapped ? kBJtMaxExpArg : bcArg);
        const double iBe = Is * (eBe - 1.0);
        const double iBc = Is * (eBc - 1.0);
        out.ib = iBe / Bf + iBc / Br;
        out.ic = iBe - iBc / alphaR;
        const double gBe = beCapped ? 0.0 : Is * eBe / nfVt;
        const double gBc = bcCapped ? 0.0 : Is * eBc / nrVt;
        out.dib_dvbe = gBe / Bf;
        out.dib_dvbc = gBc / Br;
        out.dic_dvbe = gBe;
        out.dic_dvbc = -gBc / alphaR;
        // MiSTer-Discrete 6393ca3, circuitsim/floatsim.py::early. Its
        // af*i_be - ar*i_bc is iBe - iBc in this transport convention.
        // Include the analytical voltage derivatives in the WDF Jacobian.
        // https://github.com/jopdorp/MiSTer-Discrete/commit/6393ca319f43a3b08e651bd5ca257020a66da90d
        if (Ikf > 0.0) {
            // SPICE Gummel-Poon forward injection. Divide only the transport
            // current by the normalized base charge. The reciprocal Early
            // factor is already the multiplier used by the low-current law.
            const double invVaf = Vaf > 0.0 ? 1.0 / Vaf : 0.0;
            const double invVar = Var > 0.0 ? 1.0 / Var : 0.0;
            const double earlyFactor = 1.0 - vbc * invVaf - vbe * invVar;
            const double transport = iBe - iBc;
            const double root = std::sqrt(1.0 + 4.0 * (iBe / Ikf));
            const double injection = 0.5 * (1.0 + root);
            const double injectionSlope = (gBe / Ikf) / root;
            const double adjustedTransport = transport * earlyFactor / injection;
            out.ic = adjustedTransport - iBc / Br;
            out.dic_dvbe = (gBe * earlyFactor - transport * invVar
                            - adjustedTransport * injectionSlope) / injection;
            out.dic_dvbc = (-gBc * earlyFactor - transport * invVaf) / injection - gBc / Br;
        } else if (Vaf > 0.0 || Var > 0.0) {
            const double invVaf = Vaf > 0.0 ? 1.0 / Vaf : 0.0;
            const double invVar = Var > 0.0 ? 1.0 / Var : 0.0;
            const double early = -vbc * invVaf - vbe * invVar;
            const double transport = iBe - iBc;
            out.ic += transport * early;
            out.dic_dvbe += gBe * early - transport * invVar;
            out.dic_dvbc += -gBc * early - transport * invVaf;
        }
        if (Ise > 0.0) {
            const double neVt = Ne * Vt;
            const double leakageArg = vbe / neVt;
            const bool capped = leakageArg >= kBJtMaxExpArg;
            const double expLeak = std::exp(capped ? kBJtMaxExpArg : leakageArg);
            out.ib += Ise * (expLeak - 1.0);
            if (!capped)
                out.dib_dvbe += Ise * expLeak / neVt;
        }
        out.valid = std::isfinite(out.ib) && std::isfinite(out.ic);
        return out;
    }

public:
    // -------------------------------------------------------------------------
    // Terminal evaluation at the circuit's port voltages (vce, vbe), with
    // the polarity mirror and the chain rule from (vbe, vbc) to (vce, vbe):
    //   vbe_n = p*vbe,  vbc_n = p*(vbe - vce)
    //   d/dvce = -d/dvbc_n,  d/dvbe = d/dvbe_n + d/dvbc_n
    // -------------------------------------------------------------------------
    struct BjtCurrents {
        double iC = 0.0, iB = 0.0, iE = 0.0;
        double diC_dVce = 0.0, diC_dVbe = 0.0;
        double diB_dVce = 0.0, diB_dVbe = 0.0;
        bool valid = false;
    };

    BjtCurrents evaluateTerminals(double vce, double vbe) const noexcept {
        return valid() ? evaluateValidatedTerminals(vce, vbe) : BjtCurrents{};
    }

private:
    friend class solver::RootResidual;

    /// The compiled engine validates the fixed model once. Trial voltages
    /// and computed currents still receive the same checks on every solve.
    BjtCurrents evaluateValidatedTerminals(double vce, double vbe) const noexcept {
        BjtCurrents out{};
        const double p = pnp ? -1.0 : 1.0;
        const auto em = evaluateValidatedEM(p * vbe, p * (vbe - vce));
        if (!em.valid) return out;

        out.iC = p * em.ic;
        out.iB = p * em.ib;
        out.iE = -(out.iC + out.iB);

        out.diC_dVce = -em.dic_dvbc;
        out.diC_dVbe = em.dic_dvbe + em.dic_dvbc;
        out.diB_dVce = -em.dib_dvbc;
        out.diB_dVbe = em.dib_dvbe + em.dib_dvbc;
        out.valid = std::isfinite(out.iC) && std::isfinite(out.iB);
        return out;
    }
};

// ---------------------------------------------------------------------------
// Preset registry (DAFx-2017 vintage-extracted + LTspice standard.bjt)
// ---------------------------------------------------------------------------
struct BjtEntry {
    const char* name = nullptr;
    BjtModel model;
    BjtEntry(const char* n, const BjtModel& m) : name(n), model(m) {}
};

inline BjtModel makeEM(double Is, double Bf, double Br, double Nf, double Nr,
                       bool pnp = false) {
    return BjtModel(Is, Bf, Br, Nf, Nr, pnp);
}

inline BjtModel withLowCurrentCorrections(BjtModel model, double vaf, double ise, double ne,
                                          double var = 0.0) {
    model.Vaf = vaf;
    model.Var = var;
    model.Ise = ise;
    model.Ne = ne;
    return model;
}

inline BjtModel withForwardRolloff(BjtModel model, double ikf) {
    model.Ikf = ikf;
    return model;
}

// Fairchild/National cards from MiSTer-Discrete circuit-compiler@692e777,
// circuits/parts/discretes.lib. Keep transport and recombination shared with
// the earlier cards; the extra IKF variants opt in to forward roll-off.
inline const BjtModel kMiSTer2N3904 = withLowCurrentCorrections(
    makeEM(6.734e-15, 416.4, 0.7371, 1.0, 1.0), 74.03, 6.734e-15, 1.259);
inline const BjtModel kMiSTer2N3906 = withLowCurrentCorrections(
    makeEM(1.41e-15, 180.7, 4.977, 1.0, 1.0, true), 18.7, 0.0, 1.5);

inline BjtModel withJunctionCaps(BjtModel model, double cbe, double cbc) noexcept {
    model.cbe = cbe;
    model.cbc = cbc;
    return model;
}

inline const BjtModel kAC128 = withLowCurrentCorrections(
    makeEM(22.61e-6, 1000.0, 3.76, 1.04, 1.07, true), 38.46, 0.7111e-6, 1.203, 30.02);
inline const BjtModel kOC44 = withLowCurrentCorrections(
    makeEM(2.167e-6, 1000.0, 7.98, 1.05, 1.109, true), 13.17, 31.88e-9, 1.191, 8.362);

// One registry for the engine and the picker.
inline const BjtEntry kBjtTable[] = {
    // Silicon NPN (LTspice standard.bjt)
    // onsemi 2N3904 Figs. 15-17 typical gain and VBE(on), 0.1-10 mA.
    {"2N3904",  makeEM(1.428e-14,126.9,  4.0,   1.0,   1.0,   false)},
    {"2N2222A", makeEM(1.43e-14, 255.9,  6.092, 1.0,   1.0,   false)},
    {"2N2222",  makeEM(1e-14,    200.0,  3.0,   1.0,   1.0,   false)},
    {"BC547B",  makeEM(2.39e-14, 294.3,  7.946, 1.008, 1.004, false)},
    // onsemi 2N5088/89 typical gain and VBE(on), 2N5089 gain scaled by its limits.
    {"2N5088",  makeEM(1.543e-13,557.8,  1.271, 1.09,  1.09,  false)},
    {"BC109C",  makeEM(7.049e-15,677.0,  2.209, 1.0,   1.0,   false)},
    // Toshiba 2SC2240 SPICE transport parameters (IS, BF, BR, default NF/NR).
    // https://blog.kdenki.com/2020/01/spice.html
    {"2SC2240", makeEM(1.41e-15, 310.0,  0.8893, 1.0,  1.0,   false)},
    {"2N5089",  makeEM(1.543e-13,743.8,  1.262, 1.09,  1.09,  false)},

    // Silicon PNP (LTspice standard.bjt)
    {"2N3906",  makeEM(1e-14,    200.0,  4.0,   1.0,   1.0,   true)},
    {"2N2907",  makeEM(1e-14,    250.0,  3.0,   1.0,   1.0,   true)},
    // onsemi 2N4403 Figs. 14-16 typical gain and VBE(on); Early voltage from LTspice.
    {"2N4403", withForwardRolloff(withLowCurrentCorrections(
                    makeEM(999.3e-15, 193.7, 3.578, 1.15, 1.15, true),
                    115.7, 9.486e-12, 1.887), 0.4952)},
    {"BC557B",  makeEM(3.83e-14, 344.4,  14.84, 1.008, 1.005, true)},
    {"2N5087",  makeEM(6.734e-15,254.1,  2.683, 1.0,   1.0,   true)},

    // Siemens MPSA cartridges (LTspice library, MFG=SIEMENS). LTspice
    // defaults NF and NR to 1, which is why MPSA63 carries no NF/NR of its own.
    {"MPSA63",  makeEM(1.037e-15, 19450.0, 1.144,  1.0,   1.0,   true)},
    {"MPSA70",  makeEM(28.0e-15,  284.436, 4.800,  1.0,   1.005, true)},
    {"MPSA20",  makeEM(45.0e-15,  516.544, 7.745,  1.010, 1.015, false)},

    // Germanium PNP fitted to the measured Gummel plots and output families of
    // Holmes, Holters, van Walstijn, DAFx-17, Figs. 6 and 7:
    // https://www.dafx17.eca.ed.ac.uk/papers/DAFx17_paper_28.pdf
    {"AC128",   kAC128},
    {"OC44",    kOC44},
    {"NKT275",  makeEM(1.5e-6,    85.0,   8.0,   1.05,  1.05,  true)},
    // Same static cards with the constant AC terms in DAFx-17 Table 1.
    {"AC128-DAFx-Cap", withJunctionCaps(kAC128, 0.0, 100e-12)},
    {"OC44-DAFx-Cap", withJunctionCaps(kOC44, 410e-12, 10e-12)},

    // Gert Willmann's 25 °C 2N270 fit, reproduced in Greg Whitehead's FZ-1 study:
    // https://github.com/grwhitehead/us3213181/blob/661e935/ngspice/fuzztone.mod
    // Static transport, Early voltages and base-emitter leakage. The full fit
    // also includes collector-base leakage, roll-off, charge and temperature.
    {"2N270", withLowCurrentCorrections(makeEM(24.3e-6, 172.0, 17.2, 1.055, 1.055, true),
                                         25.0, 2.57e-6, 1.99, 25.0)},

    {"2N3904-MiSTer", kMiSTer2N3904},
    {"2N3906-MiSTer", kMiSTer2N3906},
    {"2N3904-MiSTer-IKF", withForwardRolloff(kMiSTer2N3904, 66.78e-3)},
    {"2N3906-MiSTer-IKF", withForwardRolloff(kMiSTer2N3906, 80e-3)},
    // onsemi typical forward gain: 2N3904 Fig. 1 and 2N3906 Fig. 13.
    // The PNP figure is normalized. Use about 200 at 10 mA within its 100–300 limit.
    {"2N3904-onsemi", withForwardRolloff(withLowCurrentCorrections(
                        makeEM(6.734e-15, 218.45, 0.7371, 1.0, 1.0),
                        74.03, 0.0, 1.5), 0.2368)},
    {"2N3906-onsemi", withForwardRolloff(withLowCurrentCorrections(
                        makeEM(1.41e-15, 350.0, 4.977, 1.0, 1.0, true),
                        18.7, 2.513e-15, 1.318), 0.04462)},
};

inline constexpr int kNumBjts =
    static_cast<int>(sizeof(kBjtTable) / sizeof(kBjtTable[0]));

inline const BjtEntry* findBjtByName(std::string_view name) noexcept {
    for (int i = 0; i < kNumBjts; ++i) {
        const std::string_view candidate(kBjtTable[i].name);
        if (candidate == name)
            return &kBjtTable[i];
    }
    return nullptr;
}

} // namespace wdf
#endif // WDF_BJT_H
