#include <catch2.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "devices/Pentode.h"
#include "spice/Net.h"
#include "spice/SpiceNet.h"
#include "wdf/Engine.h"

// Phase 1e: Power Pentodes (EL34, 6L6GC, EL84, 6V6S, KT66, KT77).

TEST_CASE("Pentode registry looks up models case-insensitively", "[pentode][registry]") {
    REQUIRE(wdf::findPentodeByName("EL34") != nullptr);
    REQUIRE(wdf::findPentodeByName("el34") != nullptr);
    REQUIRE(wdf::findPentodeByName("6L6GC") != nullptr);
    REQUIRE(wdf::findPentodeByName("6l6") != nullptr);
    REQUIRE(wdf::findPentodeByName("EL84") != nullptr);
    REQUIRE(wdf::findPentodeByName("6V6S") != nullptr);
    REQUIRE(wdf::findPentodeByName("KT66") != nullptr);
    REQUIRE(wdf::findPentodeByName("kt77") != nullptr);
    REQUIRE(wdf::findPentodeByName("EL34Pair") != nullptr);
    REQUIRE(wdf::findPentodeByName("6L6GCPair") != nullptr);
    REQUIRE(wdf::findPentodeByName("NonExistentPentode") == nullptr);
}

TEST_CASE("Pentode analytical Jacobian matches numerical finite differences", "[pentode][jacobian]") {
    const wdf::Pentode& el34 = wdf::kEL34;
    const double vgk = -12.0;
    const double vsk = 250.0;
    const double vpk = 250.0;
    constexpr double eps = 1e-6;

    const wdf::PentodeCurrents c0 = el34.eval(vgk, vsk, vpk);
    REQUIRE(c0.ip > 0.01); // Positive plate current (~240 mA at this law's EL34 constants)
    REQUIRE(c0.is == 0.0); // Imported three-port law: the screen carries no current
    REQUIRE(c0.ig == Approx(0.0).margin(2e-9)); // Grid fully off at -12 V: reverse leak (-1e-9 A)

    // 1. dIp / dVpk
    const wdf::PentodeCurrents c_vpk_p = el34.eval(vgk, vsk, vpk + eps);
    const wdf::PentodeCurrents c_vpk_m = el34.eval(vgk, vsk, vpk - eps);
    const double num_dip_dvpk = (c_vpk_p.ip - c_vpk_m.ip) / (2.0 * eps);
    REQUIRE(c0.dip_dvpk == Approx(num_dip_dvpk).epsilon(1e-3));

    // 2. dIp / dVsk (the passive screen port: both the analytic and the
    // numerical derivative are exactly zero under the imported law)
    const wdf::PentodeCurrents c_vsk_p = el34.eval(vgk, vsk + eps, vpk);
    const wdf::PentodeCurrents c_vsk_m = el34.eval(vgk, vsk - eps, vpk);
    const double num_dip_dvsk = (c_vsk_p.ip - c_vsk_m.ip) / (2.0 * eps);
    REQUIRE(c0.dip_dvsk == Approx(num_dip_dvsk).epsilon(1e-3));

    // 3. dIp / dVgk
    const wdf::PentodeCurrents c_vgk_p = el34.eval(vgk + eps, vsk, vpk);
    const wdf::PentodeCurrents c_vgk_m = el34.eval(vgk - eps, vsk, vpk);
    const double num_dip_dvgk = (c_vgk_p.ip - c_vgk_m.ip) / (2.0 * eps);
    REQUIRE(c0.dip_dvgk == Approx(num_dip_dvgk).epsilon(1e-3));

    // 4. dIs / dVsk
    const double num_dis_dvsk = (c_vsk_p.is - c_vsk_m.is) / (2.0 * eps);
    REQUIRE(c0.dis_dvsk == Approx(num_dis_dvsk).epsilon(1e-3));

    // 5. Grid conduction under positive bias
    const wdf::PentodeCurrents c_pos_g = el34.eval(+1.5, vsk, vpk);
    REQUIRE(c_pos_g.ig > 0.0);
    REQUIRE(c_pos_g.dig_dvgk > 0.0);
}

TEST_CASE("Single-ended EL34 power stage solves DC operating point accurately", "[pentode][engine][dc]") {
    // Single-ended EL34 guitar power stage:
    // Supply B+ = 350 V
    // Plate connected to B+ via 2.5k output transformer primary impedance
    // Screen connected to 300 V via 1k screen stopper
    // Cathode connected to ground via 250 Ohm bias resistor + 47 uF bypass capacitor
    // Grid referenced to ground via 220k leak resistor
    const auto c = wdf::buildFromString(
        "V_Bplus bplus 0 DC 350.0\n"
        "V_Screen screen_in 0 DC 300.0\n"
        "R_primary bplus plate 2.5k\n"
        "R_screen screen_in screen 1k\n"
        "R_grid in 0 220k\n"
        "R_k k 0 250.0\n"
        "C_k k 0 47u\n"
        "XP1 plate screen in k EL34\n");

    REQUIRE(c.pent.size() == 1);
    REQUIRE(c.pent[0].name == "XP1");

    wdf::Engine engine(c, 48000.0);
    REQUIRE(engine.pentodeCount() == 1);
    REQUIRE(engine.rootCount() == 3); // 3 root ports: (plate,k), (grid,k), (screen,k -- passive)
    REQUIRE_FALSE(engine.allowsExplicitRoot()); // Pentode requires joint multiport Newton

    const int plateIdx = engine.nodeIndex("plate");
    const int screenIdx = engine.nodeIndex("screen");
    const int kIdx = engine.nodeIndex("k");
    const int inIdx = engine.nodeIndex("in");
    REQUIRE(plateIdx >= 0);
    REQUIRE(screenIdx >= 0);
    REQUIRE(kIdx >= 0);
    REQUIRE(inIdx >= 0);

    std::vector<double> node, rootV;
    const bool converged = engine.solveDc(node, rootV);
    REQUIRE(converged);

    const double vk = node[static_cast<size_t>(kIdx)];
    const double vp = node[static_cast<size_t>(plateIdx)];
    const double vs = node[static_cast<size_t>(screenIdx)];
    const double vin = node[static_cast<size_t>(inIdx)];

    // Grid draws only reverse saturation leakage current at negative Vgk -> Vin < 1 mV
    REQUIRE(vin == Approx(0.0).margin(1e-3));

    // Cathode bias: Vk > 0 V, establishing negative Vgk = 0 - Vk
    REQUIRE(vk > 5.0);
    REQUIRE(vk < 30.0);

    // Plate and screen voltages sit in realistic conduction range below supplies
    REQUIRE(vp > 80.0);
    REQUIRE(vp < 350.0);
    // No screen current under the imported law: the 1k stopper carries nothing,
    // so the screen node sits exactly at its 300 V supply.
    REQUIRE(vs == Approx(300.0).margin(1e-3));
}

TEST_CASE("Single-ended EL34 stage audio rate transient simulation runs cleanly", "[pentode][engine][step]") {
    const auto c = wdf::buildFromString(
        "V_Bplus bplus 0 DC 350.0\n"
        "V_Screen screen_in 0 DC 300.0\n"
        "V_Sig in 0 DC 0.0 AC 1.0 SIN(0.0 5.0 1000)\n"
        "R_primary bplus plate 2.5k\n"
        "R_screen screen_in screen 1k\n"
        "R_k k 0 250.0\n"
        "C_k k 0 47u\n"
        "XP1 plate screen in k EL34\n");

    wdf::Engine engine(c, 48000.0);
    engine.reset();

    const int plateIdx = engine.nodeIndex("plate");
    REQUIRE(plateIdx >= 0);

    constexpr int kSamples = 480; // 10 ms at 48 kHz
    for (int n = 0; n < kSamples; ++n) {
        const double t = static_cast<double>(n) / 48000.0;
        const double vin = 5.0 * std::sin(2.0 * M_PI * 1000.0 * t);
        const auto& nodes = engine.step({350.0, 300.0, vin});

        REQUIRE(engine.lastConverged());
        REQUIRE(engine.lastResidual() < 1e-9);

        const double vp = nodes[static_cast<size_t>(plateIdx)];
        // Plate swings dynamically around DC bias but stays between ground and
        // the supply rails (the imported law's kvb is smaller, so the swing is
        // wider than the old law's).
        REQUIRE(vp > -50.0);
        REQUIRE(vp < 450.0);
    }
}

TEST_CASE("Pentode grid limiter is pnjlim and keeps the exponential representable", "[pentode][grid][limiter]") {
    const wdf::Pentode p;  // 6L6GC-class defaults: grid_is = 1e-9, grid_vt = 0.025
    const double vt = p.grid_vt;
    const double vcrit = vt * std::log(vt / (std::sqrt(2.0) * p.grid_is));
    REQUIRE(vcrit == Approx(0.4174).margin(2e-3));

    SECTION("below the knee the iterate is untouched") {
        bool limited = false;
        REQUIRE(p.limitGrid(0.0, 0.1, limited) == 0.1);
        REQUIRE_FALSE(limited);
    }

    SECTION("inside the 2*vt window the iterate is untouched even past vcrit") {
        bool limited = false;
        REQUIRE(p.limitGrid(0.40, 0.43, limited) == 0.43);
        REQUIRE_FALSE(limited);
    }

    SECTION("a forward overshoot is limited in log space") {
        bool limited = false;
        const double v = p.limitGrid(0.1, 22.3, limited);
        REQUIRE(limited);
        REQUIRE(v == Approx(0.1 + vt * std::log(1.0 + (22.3 - 0.1) / vt)));
        REQUIRE(v < 22.3);
        const auto c = p.eval(v, 0.0, 450.0);
        REQUIRE(std::isfinite(c.ig));
        REQUIRE(std::isfinite(c.dig_dvgk));
        REQUIRE(c.dig_dvgk < 1.0);
    }

    SECTION("from a cold grid the overshoot maps to vt*log(vnew/vt)") {
        bool limited = false;
        const double v = p.limitGrid(-1.0, 22.3, limited);
        REQUIRE(limited);
        REQUIRE(v == Approx(vt * std::log(22.3 / vt)));
        REQUIRE(v < 22.3);
        const auto c = p.eval(v, 0.0, 450.0);
        REQUIRE(std::isfinite(c.dig_dvgk));
        REQUIRE(c.dig_dvgk < 1.0);
    }

    SECTION("repeated application stalls inside the 2*vt window") {
        // pnjlim bounds the *step*, not the value: once the gap to the
        // previous iterate is within 2*vt it stops firing, so the iterate may
        // settle slightly above vcrit (here 0.4279 V, where dig is ~1 S --
        // finite and physically meaningful, unlike the 1e209 S this guard
        // exists to prevent). It must never move the iterate forward.
        double v = 22.3, prev = 22.3;
        for (int i = 0; i < 32; ++i) {
            bool limited = false;
            v = p.limitGrid(0.4, v, limited);
            REQUIRE(v <= prev);
            prev = v;
        }
        REQUIRE(std::abs(v - 0.4) <= 2.0 * vt + 1e-12);
        bool limited = false;
        REQUIRE(p.limitGrid(0.4, v, limited) == v);  // fixed point
        REQUIRE_FALSE(limited);
    }

    SECTION("every seed reachable under the per-port prediction limit stays finite") {
        // The engine clamps a port's prediction and trial step to +-50 V
        // (Limits::predGrid*/stepGrid*), so this is the input set `limitGrid`
        // can actually see. pnjlim reduces the overshoot only logarithmically,
        // so this property is joint with that clamp, not a property of
        // `limitGrid` alone -- which is exactly the contract the solver needs.
        const double vold[] = {-500.0, -168.0, -52.0, -10.0, -0.5, 0.0, 0.1, 0.4, 0.5};
        const double vnew[] = {-100.0, -52.0, -1.0, 0.0, 0.1, 0.417, 0.43, 1.0, 5.0, 22.3, 50.0};
        for (double vo : vold) {
            for (double vn : vnew) {
                bool limited = false;
                const double v = p.limitGrid(vo, vn, limited);
                INFO("vold=" << vo << " vnew=" << vn << " v=" << v);
                REQUIRE(std::isfinite(v));
                REQUIRE(v <= std::max(vo, vn) + 1e-12);
                const auto c = p.eval(v, 0.0, 450.0);
                REQUIRE(std::isfinite(c.ig));
                REQUIRE(std::isfinite(c.dig_dvgk));
                REQUIRE(std::abs(c.ig) < 1e6);
                REQUIRE(c.dig_dvgk < 1e6);
            }
        }
    }
}
