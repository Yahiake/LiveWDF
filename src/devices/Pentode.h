#pragma once

#include <algorithm>
#include <cmath>

#include <string>

#include "devices/Triode.h"
#include "wdf/solver/PushPullTries.h"

namespace wdf {

inline bool livewdfGridLawEnabled() noexcept {
    static const bool enabled = livewdfPpTry("gridlaw");
    return enabled;
}

inline void evalGridLaw(double vgk, double& ig, double& dig_dvgk) noexcept {
    // Unfitted least-squares approximation to V^1.5 over 0..20 V.
    constexpr double knots[] = {0.0, 2.56};
    constexpr double coefs[] = {5.0474394831736653e-05, -3.9847576135144427e-05};
    ig = 0.0;
    dig_dvgk = 0.0;
    for (int j = 0; j < 2; ++j) {
        const double h = vgk - knots[j];
        if (h > 0.0) {
            ig += coefs[j] * h * h;
            dig_dvgk += 2.0 * coefs[j] * h;
        }
    }
}

/// Currents of one pentode (or a matched pair) and their derivatives with
/// respect to the port voltages (control grid G1 = g-k, screen S = s-k, plate P = p-k).
/// Currents flow into the grid, screen, and plate and out of the cathode.
struct PentodeCurrents {
    double ig = 0.0, is = 0.0, ip = 0.0;
    double dig_dvgk = 0.0;                                    // dIg/dVsk = dIg/dVpk = 0
    double dis_dvgk = 0.0, dis_dvsk = 0.0, dis_dvpk = 0.0;
    double dip_dvgk = 0.0, dip_dvsk = 0.0, dip_dvpk = 0.0;

    void scale(double k) noexcept {
        for (double* x : {&ig, &is, &ip, &dig_dvgk, &dis_dvgk, &dis_dvsk, &dis_dvpk, &dip_dvgk, &dip_dvsk, &dip_dvpk})
            *x *= k;
    }
};

/// Knee formulation for the Reefman pentode model.
enum class PentodeKneeType {
    DerkD,  ///< Tier 2A: rational knee, kappa = 1 / (1 + beta * Vp)
    DerkE   ///< Tier 2B: exponential knee, kappa = exp(-(beta * Vp)^1.5)
};

/// Emission as a sum of squared hinges of (Vgk + av*Vsk - knot), split between
/// plate and screen by the knee K = 1/(1 + beta*Vpk):
///   Ip = E*(plate*(1 - K) + lambda*Vpk),  Is = E*(rho + sigma*K).
/// The fits conduct a little at zero screen voltage, so E fades out below
/// about screenFade volts instead of stepping to zero at Vsk = 0.
struct QuadricEmission {
    static constexpr double screenFade = 4.0;
    double av[5], knot[5], coef[5];
    double beta, plate, lambda, rho, sigma;

    double screenShare(double vpk) const noexcept { return rho + sigma / (1.0 + beta * vpk); }

    void currents(double vgk, double vsk, double vpk, PentodeCurrents& c) const noexcept {
        double e = 0.0, eg = 0.0, es = 0.0;
        for (int j = 0; j < 5; ++j) {
            const double h = vgk + av[j] * vsk - knot[j];
            if (h > 0.0) {
                e += coef[j] * h * h;
                eg += 2.0 * coef[j] * h;
                es += 2.0 * coef[j] * av[j] * h;
            }
        }
        const double d = vsk * vsk + screenFade * screenFade, fade = vsk * vsk / d;
        es = es * fade + e * 2.0 * vsk * screenFade * screenFade / (d * d);
        e *= fade;
        eg *= fade;
        const double k = 1.0 / (1.0 + beta * vpk), dk = -beta * k * k;
        const double toPlate = plate * (1.0 - k) + lambda * vpk, toScreen = rho + sigma * k;
        c.ip = e * toPlate;
        c.dip_dvgk = eg * toPlate;
        c.dip_dvsk = es * toPlate;
        c.dip_dvpk = e * (lambda - plate * dk);
        c.is = e * toScreen;
        c.dis_dvgk = eg * toScreen;
        c.dis_dvsk = es * toScreen;
        c.dis_dvpk = e * sigma * dk;
    }
};

// Fitted emission laws for the Reefman cards below.
// Regenerate with gen_table.py from the coefficient JSON files.
inline constexpr QuadricEmission kQuadricEL34{
    {0.14941715565, 0.116693513724, 0.0899492817556, 0.169916903759, 0.0707959155439},
    {-4.57832099533, -3.23528010883, -4.80749734689, -3.16789655526, -11.5090101715},
    {2.06472547436e-05, 4.23025346972e-05, 0.000173284856326, 2.34779823681e-06, 8.68713032692e-05},
    0.036799799546, 1.0, 0.000295976411557, 0.110476548186, 0.358758320581};
inline constexpr QuadricEmission kQuadric6L6GC{
    {0.372459360661, 0.105362788647, 0.0368435498852, 0.0661503846848, 0.00500000000126},
    {24.9999995649, -11.5033736576, -12.0548484009, 24.888132515, -2.58337583801},
    {2.89314279738e-06, 0.000105123050115, 3.5453144483e-05, 0.0074594868438, 0.000742684157285},
    0.0548896189473, 1.0, 0.000380824543824, 0.0379148763953, 0.492122847629};
// The cheaper fit to the Reefman card keeps Preview inside its headroom.
inline constexpr QuadricEmission kQuadric6L6GCPreview{
    {0.170219523967, 0.142616525263, 0.11307917015, 0.217636087066, 0.0729490552428},
    {-5.54151904163, -5.53857990429, -7.04389240158, -5.504627238, -9.90692531081},
    {0.00346625766973, 0.0105724907037, 0.0233999810999, 0.000529542856001, 0.0280165437189},
    0.069, 0.00208927130543, 1.1e-06, 0.000149868864743, 0.00121393780442};
inline constexpr QuadricEmission kQuadric6V6S{
    {0.00764425794421, 0.0839000976494, 0.205935970485, 0.0838937728185, 0.005},
    {3.10266014067, -14.8216633914, 24.9999751576, -14.8730522558, -8.69030854282},
    {1.99455071527e-10, 8.42717386662e-05, 2.41144751575e-05, 3.16340767724e-07, 0.000298499591498},
    0.0319817275545, 1.0, 1.0000000004e-08, 0.0746126498141, 0.128582259592};
inline constexpr QuadricEmission kQuadricEL84{
    {0.0517005884028, 0.0515018852113, 0.051499877391, 0.0717704163969, 0.0357177251847},
    {-2.95930771384, -2.98612383167, -2.98559010944, -35.3285120241, 0.00780446244687},
    {3.41060925377e-07, 0.000110300864644, 0.000623071538105, 3.67939579773e-11, 3.69299380011e-05},
    0.0285128761724, 1.0, 5.7088672908e-05, 0.0844099160086, 0.143847084188};

/// Clean, self-contained constitutive evaluator for the Reefman pentode model
/// (incorporating Derk D and Derk E knees and optional secondary emission).
///
/// Returns currents in Amperes and conductances in Siemens (A/V).
/// Allocation-free and completely separated from solver matrices.
struct ReefmanPentode {
    PentodeKneeType knee = PentodeKneeType::DerkD;
    bool secondaryEmission = false;

    // Core tube parameters (default values seeded from 6L6GC reference)
    double mu = 9.41;
    double ex = 1.306;
    double kg1 = 446.6;
    double kg2 = 6672.5;
    double kp = 45.2;
    double kvb = 3205.1;
    double beta = 0.069;
    double alpha_s = 8.10;         // alS
    double a = 0.11e-5 * 446.6;    // raw bracket coefficient (Aokg1 * kg1)

    // Secondary emission parameters (active when secondaryEmission == true)
    double sC = 0.031;
    double ap = 0.017;
    double lamv = 14.66;
    double nu = 3.08;
    double wv = -5.0;

    // Control grid diode parameters (matching existing Pentode.h)
    double grid_is = 1e-9;
    double grid_vt = 0.025;

    // Count / matched-pair scaling
    double count = 1.0;

    const QuadricEmission* quadric = nullptr;
    const QuadricEmission* previewQuadric = nullptr;

    bool operator==(const ReefmanPentode&) const = default;

    /// The active plate/screen split; Reefman's is exact for DerkD without secondary emission.
    QuadricEmission split() const noexcept {
        return quadric ? *quadric : QuadricEmission{{}, {}, {}, beta, 1.0 / kg1 - 1.0 / kg2, a / kg1, 1.0 / kg2, alpha_s / kg2};
    }

    static ReefmanPentode el34(double count = 1.0) noexcept {
        ReefmanPentode r;
        r.mu = 12.50;
        r.ex = 1.363;
        r.kg1 = 217.7;
        r.kg2 = 1950.2;
        r.kp = 50.5;
        r.kvb = 1282.7;
        r.beta = 0.105;
        r.alpha_s = 6.09;
        r.a = 0.16e-5 * 217.7;
        r.sC = 0.022;
        r.ap = 0.033;
        r.lamv = 4.23;
        r.nu = 2.91;
        r.wv = 64.0;
        r.knee = PentodeKneeType::DerkD;
        r.secondaryEmission = false;
        r.quadric = &kQuadricEL34;
        r.count = count;
        return r;
    }

    static ReefmanPentode sixL6GC(double count = 1.0) noexcept {
        ReefmanPentode r;
        r.mu = 9.41;
        r.ex = 1.306;
        r.kg1 = 446.6;
        r.kg2 = 6672.5;
        r.kp = 45.2;
        r.kvb = 3205.1;
        r.beta = 0.069;
        r.alpha_s = 8.10;
        r.a = 0.11e-5 * 446.6;
        r.sC = 0.031;
        r.ap = 0.017;
        r.lamv = 14.66;
        r.nu = 3.08;
        r.wv = -5.0;
        r.knee = PentodeKneeType::DerkD;
        r.secondaryEmission = false;
        r.quadric = &kQuadric6L6GC;
        r.previewQuadric = &kQuadric6L6GCPreview;
        r.count = count;
        return r;
    }

    static ReefmanPentode el84(double count = 1.0) noexcept {
        ReefmanPentode r;
        r.mu = 23.36;
        r.ex = 1.138;
        r.kg1 = 117.4;
        r.kg2 = 1275.0;
        r.kp = 152.4;
        r.kvb = 4015.8;
        r.beta = 0.148;
        r.alpha_s = 7.66;
        r.a = 0.37e-5 * 117.4;
        r.sC = 0.038;
        r.ap = 0.054;
        r.lamv = 4.13;
        r.nu = 1.56;
        r.wv = 58.0;
        r.knee = PentodeKneeType::DerkD;
        r.secondaryEmission = false;
        r.quadric = &kQuadricEL84;
        r.count = count;
        return r;
    }

    static ReefmanPentode sixV6S(double count = 1.0) noexcept {
        ReefmanPentode r;
        r.mu = 10.56;
        r.ex = 1.306;
        r.kg1 = 609.8;
        r.kg2 = 17267.3;
        r.kp = 47.9;
        r.kvb = 2171.5;
        r.beta = 0.068;
        r.alpha_s = 18.72;
        r.a = 0.57e-6 * 609.8;
        r.sC = 0.081;
        r.ap = 0.013;
        r.lamv = 5.7;
        r.nu = 0.92;
        r.wv = 18.0;
        r.knee = PentodeKneeType::DerkD;
        r.secondaryEmission = false;
        r.quadric = &kQuadric6V6S;
        r.count = count;
        return r;
    }

    static ReefmanPentode kt66(double count = 1.0) noexcept {
        return sixL6GC(count);
    }

    static ReefmanPentode kt77(double count = 1.0) noexcept {
        return el34(count);
    }

    /// Checks whether the operating point lies within the mathematically and
    /// physically supported domain of the constitutive equations.
    bool isSupported(double vgk, double vsk, double vpk) const noexcept {
        if (!std::isfinite(vgk) || !std::isfinite(vsk) || !std::isfinite(vpk))
            return false;
        if (vpk < 0.0)
            return false;
        if (vsk <= 0.0)
            return false;
        if (knee == PentodeKneeType::DerkD && (1.0 + beta * vpk) <= 0.0)
            return false;
        return true;
    }

    /// Evaluates control grid current and derivative using the established
    /// exponential diode law.
    void evalGrid(double vgk, double& ig, double& dig_dvgk) const noexcept {
        if (livewdfGridLawEnabled()) return evalGridLaw(vgk, ig, dig_dvgk);
        const double inv_grid_vt = (grid_vt > 0.0) ? (1.0 / grid_vt) : 0.0;
        const double grid_is_over_vt = (grid_vt > 0.0) ? (grid_is / grid_vt) : 0.0;
        const double x = vgk * inv_grid_vt;
        if (x < -37.0) {
            ig = -grid_is;
            dig_dvgk = 0.0;
        } else {
            const double xClamped = (x > 500.0) ? 500.0 : x;
            const double ev = std::exp(xClamped);
            ig = grid_is * (ev - 1.0);
            dig_dvgk = (x <= 500.0) ? (ev * grid_is_over_vt) : 0.0;
        }
    }

    /// Full evaluation returning plate, screen, and grid currents along with
    /// all analytical partial derivatives (in Amperes and Siemens).
    PentodeCurrents eval(double vgk, double vsk, double vpk) const noexcept {
        PentodeCurrents c;
        evalGrid(vgk, c.ig, c.dig_dvgk);
        if (quadric) {
            if (vpk >= 0.0 && vsk > 0.0)
                quadric->currents(vgk, vsk, vpk, c);
            c.scale(count);
            return c;
        }

        if (!isSupported(vgk, vsk, vpk)) {
            if (count != 1.0) {
                c.ig *= count;
                c.dig_dvgk *= count;
            }
            return c;
        }

        // --- 1. Island calculation ---
        const double S = std::sqrt(kvb + vsk * vsk);
        const double inv_S = 1.0 / S;
        const double u = (1.0 / mu) + vgk * inv_S;
        const double w = kp * u;

        double sp = 0.0, sg = 0.0;
        softplusSigmoid(w, sp, sg);

        const double e1 = (vsk / kp) * sp;
        if (e1 <= 0.0) {
            if (count != 1.0) {
                c.ig *= count;
                c.dig_dvgk *= count;
            }
            return c;
        }

        const double i_space = std::pow(e1, ex);
        const double de1_deg = vsk * sg * inv_S;
        const double de1_des = (sp / kp) - (sg * vgk * vsk * vsk) * (inv_S * inv_S * inv_S);
        const double di_over_e1 = ex * (i_space / e1);
        const double di_deg = di_over_e1 * de1_deg;
        const double di_des = di_over_e1 * de1_des;

        // --- 2. Knee calculation ---
        double kap = 0.0;
        double dkap = 0.0;
        if (knee == PentodeKneeType::DerkD) {
            const double den = 1.0 + beta * vpk;
            kap = 1.0 / den;
            dkap = -beta / (den * den);
        } else {
            const double z = beta * vpk;
            const double sqrt_z = std::sqrt(z);
            kap = std::exp(-z * sqrt_z);
            dkap = -1.5 * beta * sqrt_z * kap;
        }

        // --- 3. Secondary emission calculation ---
        double se = 0.0;
        double dse_ep = 0.0;
        double dse_eg = 0.0;
        double dse_es = 0.0;
        if (secondaryEmission) {
            const double Vc = (vsk / lamv) - nu * vgk - wv;
            const double q = -ap * (vpk - Vc);
            if (q > 20.0) {
                se = 2.0 * sC * vpk;
                dse_ep = 2.0 * sC;
            } else if (q < -20.0) {
                se = 0.0;
                dse_ep = 0.0;
            } else {
                const double th = std::tanh(q);
                const double one_plus_th = 1.0 + th;
                se = sC * vpk * one_plus_th;
                const double d1 = 1.0 - th * th;
                const double sC_vpk_d1 = sC * vpk * d1;
                dse_ep = sC * one_plus_th - sC_vpk_d1 * ap;
                dse_eg = -sC_vpk_d1 * (ap * nu);
                dse_es = sC_vpk_d1 * (ap / lamv);
            }
        }

        // --- 4. Brackets and currents ---
        const double inv_kg1 = 1.0 / kg1;
        const double inv_kg2 = 1.0 / kg2;
        const double O = inv_kg1 - inv_kg2;
        const double C = O;  // Algebraic identity C == O

        const double B = O + (a * vpk) * inv_kg1 - se * inv_kg2 - C * kap;
        const double Dk = 1.0 + alpha_s * kap + se;

        c.ip = i_space * B;
        c.is = i_space * Dk * inv_kg2;

        // --- 5. Full analytical Jacobian conductances (A/V) ---
        c.dip_dvpk = i_space * (a * inv_kg1 - dse_ep * inv_kg2 - C * dkap);
        c.dip_dvgk = di_deg * B - i_space * (dse_eg * inv_kg2);
        c.dip_dvsk = di_des * B - i_space * (dse_es * inv_kg2);

        c.dis_dvpk = i_space * (alpha_s * dkap + dse_ep) * inv_kg2;
        c.dis_dvgk = (di_deg * Dk + i_space * dse_eg) * inv_kg2;
        c.dis_dvsk = (di_des * Dk + i_space * dse_es) * inv_kg2;

        // --- 6. Matched pair scaling ---
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

    /// Value-only evaluation for currents (in Amperes).
    void evalCurrents(double vgk, double vsk, double vpk,
                      double& ig, double& is, double& ip) const noexcept {
        if (quadric) {
            const auto c = eval(vgk, vsk, vpk);
            ig = c.ig, is = c.is, ip = c.ip;
            return;
        }
        double dummy_dig = 0.0;
        evalGrid(vgk, ig, dummy_dig);

        if (!isSupported(vgk, vsk, vpk)) {
            is = 0.0;
            ip = 0.0;
            if (count != 1.0) {
                ig *= count;
            }
            return;
        }

        const double S = std::sqrt(kvb + vsk * vsk);
        const double inv_S = 1.0 / S;
        const double u = (1.0 / mu) + vgk * inv_S;
        const double w = kp * u;

        double sp = 0.0, sg = 0.0;
        softplusSigmoid(w, sp, sg);

        const double e1 = (vsk / kp) * sp;
        if (e1 <= 0.0) {
            is = 0.0;
            ip = 0.0;
            if (count != 1.0) {
                ig *= count;
            }
            return;
        }

        const double i_space = std::pow(e1, ex);

        double kap = 0.0;
        if (knee == PentodeKneeType::DerkD) {
            kap = 1.0 / (1.0 + beta * vpk);
        } else {
            const double z = beta * vpk;
            kap = std::exp(-z * std::sqrt(z));
        }

        double se = 0.0;
        if (secondaryEmission) {
            const double Vc = (vsk / lamv) - nu * vgk - wv;
            const double q = -ap * (vpk - Vc);
            if (q > 20.0) {
                se = 2.0 * sC * vpk;
            } else if (q >= -20.0) {
                se = sC * vpk * (1.0 + std::tanh(q));
            }
        }

        const double inv_kg1 = 1.0 / kg1;
        const double inv_kg2 = 1.0 / kg2;
        const double O = inv_kg1 - inv_kg2;
        const double C = O;

        const double B = O + (a * vpk) * inv_kg1 - se * inv_kg2 - C * kap;
        const double Dk = 1.0 + alpha_s * kap + se;

        ip = i_space * B;
        is = i_space * Dk * inv_kg2;

        if (count != 1.0) {
            ig *= count;
            is *= count;
            ip *= count;
        }
    }

    /// Grid junction limiter: beyond vcrit the exponential's conductance
    /// outgrows any Newton step. The limiter moves the iterate in log space,
    /// never the law: at convergence it leaves the same exponential root.
    double limitGrid(double vold, double vnew, bool& limited) const noexcept {
        const double vcrit = grid_vt * std::log(grid_vt / (1.4142135623730950488 * (grid_is > 1e-30 ? grid_is : 1e-30)));
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

/// Authentic three-port Norman Koren (1996/2001) pentode model with active screen conduction:
///
///   Island potential:
///     u  = 1/mu + Vgk/Vsk
///     E1 = (Vsk/kp) * ln(1 + exp(kp * u)) = (Vsk/kp) * softplus(kp * u)
///
///   Plate current:
///     Ip = (E1^ex / kg1) * atan(Vpk / kvb)  (for E1 > 0, Vpk > 0)
///
///   Screen current:
///     b  = Vsk/mu + Vgk
///     Is = (b^ex / kg2)                     (for b > 0)
///
///   Grid current:
///     Ig = grid_is * (exp(clamp(Vgk/grid_vt, -500, 500)) - 1)
///
/// Complete 3x3 analytical Jacobian with exact partial derivatives:
///   dIp/dVpk, dIp/dVgk, dIp/dVsk
///   dIg/dVgk
///   dIs/dVpk (= 0), dIs/dVgk, dIs/dVsk
struct KorenPentode {
    double mu = 11.0;
    double ex = 1.35;
    double kg1 = 650.0;
    double kg2 = 4200.0;
    double kp = 60.0;
    double kvb = 24.0;
    double count = 1.0;

    double grid_is = 1e-9;
    double grid_vt = 0.025;

    bool operator==(const KorenPentode&) const = default;

    static KorenPentode el34(double count = 1.0) noexcept {
        KorenPentode k;
        k.mu = 11.0;
        k.ex = 1.35;
        k.kg1 = 650.0;
        k.kg2 = 4200.0;
        k.kp = 60.0;
        k.kvb = 24.0;
        k.count = count;
        return k;
    }

    static KorenPentode sixL6GC(double count = 1.0) noexcept {
        KorenPentode k;
        k.mu = 8.7;
        k.ex = 1.35;
        k.kg1 = 1460.0;
        k.kg2 = 4500.0;
        k.kp = 48.0;
        k.kvb = 12.0;
        k.count = count;
        return k;
    }

    static KorenPentode el84(double count = 1.0) noexcept {
        KorenPentode k;
        k.mu = 19.0;
        k.ex = 1.35;
        k.kg1 = 500.0;
        k.kg2 = 4200.0;
        k.kp = 60.0;
        k.kvb = 24.0;
        k.count = count;
        return k;
    }

    static KorenPentode sixV6S(double count = 1.0) noexcept {
        KorenPentode k;
        k.mu = 12.0;
        k.ex = 1.30;
        k.kg1 = 800.0;
        k.kg2 = 4500.0;
        k.kp = 50.0;
        k.kvb = 25.0;
        k.count = count;
        return k;
    }

    static KorenPentode six550(double count = 1.0) noexcept {
        KorenPentode k;
        k.mu = 7.9;
        k.ex = 1.35;
        k.kg1 = 890.0;
        k.kg2 = 4200.0;
        k.kp = 60.0;
        k.kvb = 24.0;
        k.count = count;
        return k;
    }

    void evalGrid(double vgk, double& ig, double& dig_dvgk) const noexcept {
        if (livewdfGridLawEnabled()) return evalGridLaw(vgk, ig, dig_dvgk);
        const double x = vgk / grid_vt;
        if (x < -37.0) {
            ig = -grid_is;
            dig_dvgk = 0.0;
        } else {
            const double xClamped = (x > 500.0) ? 500.0 : x;
            const double ev = std::exp(xClamped);
            ig = grid_is * (ev - 1.0);
            dig_dvgk = ev * (grid_is / grid_vt);
        }
    }

    PentodeCurrents eval(double vgk, double vsk, double vpk) const noexcept {
        constexpr double leak = 1e-12;
        PentodeCurrents c;

        evalGrid(vgk, c.ig, c.dig_dvgk);

        // Screen voltage regularized to avoid singularity at Vsk <= 0
        const double vsSafe = (vsk > 1e-6) ? vsk : 1e-6;
        const double invVs = 1.0 / vsSafe;
        const double u = (1.0 / mu) + vgk * invVs;
        const double w = kp * u;

        double sp = 0.0, lam = 0.0;
        softplusSigmoid(w, sp, lam);

        const double e1 = (vsSafe / kp) * sp;

        // Plate current
        if (e1 > 0.0 && vpk > 0.0 && vsk > 0.0) {
            const double ip_base = std::pow(e1, ex);
            const double inv_kvb = 1.0 / kvb;
            const double r = vpk * inv_kvb;
            const double at = std::atan(r);
            const double ip_over_kg1 = ip_base / kg1;
            c.ip = ip_over_kg1 * at;

            const double d_pf = inv_kvb / (1.0 + r * r);
            const double dvpk = ip_over_kg1 * d_pf;
            c.dip_dvpk = (dvpk > leak) ? dvpk : leak;

            const double de1_dvgk = lam;
            const double de1_dvsk = (sp / kp) - lam * (vgk * invVs);

            const double dip_de1 = (ex * ip_base / e1) * (at / kg1);
            c.dip_dvgk = dip_de1 * de1_dvgk;
            c.dip_dvsk = dip_de1 * de1_dvsk;
        } else {
            c.ip = 0.0;
            c.dip_dvpk = leak;
            c.dip_dvgk = 0.0;
            c.dip_dvsk = 0.0;
        }

        // Screen current
        const double b = (vsSafe / mu) + vgk;
        if (b > 1e-12 && vsk > 0.0) {
            const double b_pow_ex = std::pow(b, ex);
            c.is = b_pow_ex / kg2;

            const double dis_db = (ex * std::pow(b, ex - 1.0)) / kg2;
            c.dis_dvgk = dis_db;
            c.dis_dvsk = dis_db / mu;
            c.dis_dvpk = 0.0;
        } else {
            c.is = 0.0;
            c.dis_dvgk = 0.0;
            c.dis_dvsk = 0.0;
            c.dis_dvpk = 0.0;
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

    void eval(double vgk, double vsk, double vpk, double& ip, double& is, double& ig) const noexcept {
        const auto c = eval(vgk, vsk, vpk);
        ip = c.ip;
        is = c.is;
        ig = c.ig;
    }

    double limitGrid(double vold, double vnew, bool& limited) const noexcept {
        const double vcrit = grid_vt * std::log(grid_vt / (1.4142135623730950488 * (grid_is > 1e-30 ? grid_is : 1e-30)));
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

enum class PentodeModelKind {
    Reefman,
    Koren
};

/// Composite pentode model: defaults to Reefman DerkD evaluator.
/// An authentic 3-port Koren model remains selectable for comparison.
struct Pentode {
    PentodeModelKind modelKind = PentodeModelKind::Reefman;
    bool useReefman = true;
    ReefmanPentode reefman = ReefmanPentode::el34();
    KorenPentode koren = KorenPentode::el34();
    double count = 1.0;

    // Metadata fields for document round-trip
    double mu = 11.0, ex = 1.35, kg1 = 650.0, kg2 = 4200.0, kp = 60.0, kvb = 24.0;
    double kvb2 = 18.0;
    double vg2 = 470.0;
    double rp = 15000.0;
    double v_max = 500.0;
    double grid_is = 1e-9;
    double grid_vt = 0.025;

    bool operator==(const Pentode&) const = default;

    static Pentode makeReefman(const ReefmanPentode& r) {
        Pentode p{};
        p.modelKind = PentodeModelKind::Reefman;
        p.useReefman = true;
        p.reefman = r;
        p.count = r.count;
        return p;
    }

    static Pentode makeKoren(const KorenPentode& k) {
        Pentode p{};
        p.modelKind = PentodeModelKind::Koren;
        p.useReefman = false;
        p.koren = k;
        p.count = k.count;
        p.mu = k.mu; p.ex = k.ex; p.kg1 = k.kg1; p.kg2 = k.kg2; p.kp = k.kp; p.kvb = k.kvb;
        return p;
    }

    static Pentode makeKorenFromParams(double mu, double ex, double kg1, double kg2, double kp,
                              double kvb, double kvb2 = 18.0, double vg2 = 470.0, double rp = 15000.0,
                              double v_max = 500.0, double count = 1.0) {
        KorenPentode k;
        k.mu = mu; k.ex = ex; k.kg1 = kg1; k.kg2 = kg2; k.kp = kp; k.kvb = kvb; k.count = count;
        Pentode p = makeKoren(k);
        p.kvb2 = kvb2; p.vg2 = vg2; p.rp = rp; p.v_max = v_max;
        return p;
    }

    PentodeCurrents eval(double vgk, double vsk, double vpk) const noexcept {
        if (modelKind == PentodeModelKind::Reefman && useReefman) {
            return reefman.eval(vgk, vsk, vpk);
        }
        return koren.eval(vgk, vsk, vpk);
    }

    double limitGrid(double vold, double vnew, bool& limited) const noexcept {
        if (livewdfGridLawEnabled()) return vnew;
        if (modelKind == PentodeModelKind::Reefman && useReefman) {
            return reefman.limitGrid(vold, vnew, limited);
        }
        return koren.limitGrid(vold, vnew, limited);
    }

    void ensureTable() const noexcept {}
};

/// EL34: Reefman DerkD power pentode model.
inline const Pentode kEL34 = Pentode::makeReefman(ReefmanPentode::el34(1.0));
/// Matched EL34 pair.
inline const Pentode kEL34Pair = Pentode::makeReefman(ReefmanPentode::el34(2.0));

/// 6L6GC: Reefman DerkD beam power tube model.
inline const Pentode k6L6GC = Pentode::makeReefman(ReefmanPentode::sixL6GC(1.0));
/// Matched 6L6GC pair.
inline const Pentode k6L6GCPair = Pentode::makeReefman(ReefmanPentode::sixL6GC(2.0));

/// EL84: Reefman DerkD power pentode model.
inline const Pentode kEL84 = Pentode::makeReefman(ReefmanPentode::el84(1.0));
/// Matched EL84 pair.
inline const Pentode kEL84Pair = Pentode::makeReefman(ReefmanPentode::el84(2.0));

/// 6V6S: Reefman DerkD beam power tube model.
inline const Pentode k6V6S = Pentode::makeReefman(ReefmanPentode::sixV6S(1.0));
/// Matched 6V6S pair.
inline const Pentode k6V6SPair = Pentode::makeReefman(ReefmanPentode::sixV6S(2.0));

/// KT66: Reefman DerkD beam power tube model (6L6GC parameters).
inline const Pentode kKT66 = Pentode::makeReefman(ReefmanPentode::kt66(1.0));

/// KT77: Reefman DerkD power pentode model (EL34 parameters).
inline const Pentode kKT77 = Pentode::makeReefman(ReefmanPentode::kt77(1.0));

/// Koren power pentodes (authentic 3-port model with active screen conduction)
inline const Pentode kKorenEL34 = Pentode::makeKoren(KorenPentode::el34(1.0));
inline const Pentode kKorenEL34Pair = Pentode::makeKoren(KorenPentode::el34(2.0));
inline const Pentode kKoren6L6GC = Pentode::makeKoren(KorenPentode::sixL6GC(1.0));
inline const Pentode kKoren6L6GCPair = Pentode::makeKoren(KorenPentode::sixL6GC(2.0));
inline const Pentode kKorenEL84 = Pentode::makeKoren(KorenPentode::el84(1.0));
inline const Pentode kKoren6V6S = Pentode::makeKoren(KorenPentode::sixV6S(1.0));
inline const Pentode kKoren6550 = Pentode::makeKoren(KorenPentode::six550(1.0));

/// Philips EF86 transfer fit at its 250 V plate and 140 V screen reference.
/// https://hfc-fs.s3-eu-west-1.amazonaws.com/s3fs-public/ef86_phillips_datasheet.pdf
inline const Pentode kEF86 = Pentode::makeKoren(
    {38.0, 1.35, 951.99, 2840.32, 177.12, 13.1, 1.0, 1e-8, 0.06});

/// Raytheon 5725/6AS6W fit with the suppressor at cathode potential.
/// The four-terminal pentode law does not have a separate suppressor port.
/// https://frank.yueksel.org/sheets/138/5/5725.pdf
inline const Pentode k6AS6 = Pentode::makeKoren(
    {29.0, 1.5, 932.09, 893.15, 131.09, 13.0, 1.0, 1e-8, 0.06});

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
    {"KOREN-EL34", &kKorenEL34},
    {"KOREN-EL34PAIR", &kKorenEL34Pair},
    {"KOREN-6L6GC", &kKoren6L6GC},
    {"KOREN-6L6GCPAIR", &kKoren6L6GCPair},
    {"KOREN-EL84", &kKorenEL84},
    {"KOREN-6V6S", &kKoren6V6S},
    {"KOREN-6550", &kKoren6550},
    {"EF86", &kEF86},
    {"6AS6", &k6AS6},
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
