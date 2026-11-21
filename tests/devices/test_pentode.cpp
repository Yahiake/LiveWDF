#include <catch2.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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

TEST_CASE("Koren Pentode analytical Jacobian matches numerical finite differences", "[pentode][jacobian]") {
    const wdf::Pentode& el34 = wdf::kKorenEL34;
    const double vgk = -12.0;
    const double vsk = 250.0;
    const double vpk = 250.0;
    constexpr double eps = 1e-6;

    const wdf::PentodeCurrents c0 = el34.eval(vgk, vsk, vpk);
    REQUIRE(c0.ip > 0.01); // Positive plate current
    REQUIRE(c0.is > 0.0);  // Active screen current under full Koren 3-port model
    REQUIRE(c0.ig == Approx(0.0).margin(2e-9)); // Grid fully off at -12 V: reverse leak (-1e-9 A)

    // 1. dIp / dVpk
    const wdf::PentodeCurrents c_vpk_p = el34.eval(vgk, vsk, vpk + eps);
    const wdf::PentodeCurrents c_vpk_m = el34.eval(vgk, vsk, vpk - eps);
    const double num_dip_dvpk = (c_vpk_p.ip - c_vpk_m.ip) / (2.0 * eps);
    REQUIRE(c0.dip_dvpk == Approx(num_dip_dvpk).epsilon(1e-3));

    // 2. dIp / dVsk (active under full Koren: plate current depends on screen voltage via island)
    const wdf::PentodeCurrents c_vsk_p = el34.eval(vgk, vsk + eps, vpk);
    const wdf::PentodeCurrents c_vsk_m = el34.eval(vgk, vsk - eps, vpk);
    const double num_dip_dvsk = (c_vsk_p.ip - c_vsk_m.ip) / (2.0 * eps);
    REQUIRE(c0.dip_dvsk == Approx(num_dip_dvsk).epsilon(1e-3));

    // 3. dIp / dVgk
    const wdf::PentodeCurrents c_vgk_p = el34.eval(vgk + eps, vsk, vpk);
    const wdf::PentodeCurrents c_vgk_m = el34.eval(vgk - eps, vsk, vpk);
    const double num_dip_dvgk = (c_vgk_p.ip - c_vgk_m.ip) / (2.0 * eps);
    REQUIRE(c0.dip_dvgk == Approx(num_dip_dvgk).epsilon(1e-3));

    // 4. dIs / dVsk (active screen conduction: Is = (Vsk/mu + Vgk)^ex / kg2)
    const double num_dis_dvsk = (c_vsk_p.is - c_vsk_m.is) / (2.0 * eps);
    REQUIRE(c0.dis_dvsk == Approx(num_dis_dvsk).epsilon(1e-3));

    // 5. dIs / dVgk
    const double num_dis_dvgk = (c_vgk_p.is - c_vgk_m.is) / (2.0 * eps);
    REQUIRE(c0.dis_dvgk == Approx(num_dis_dvgk).epsilon(1e-3));

    // 6. Grid conduction under positive bias
    const wdf::PentodeCurrents c_pos_g = el34.eval(+1.5, vsk, vpk);
    REQUIRE(c_pos_g.ig > 0.0);
    REQUIRE(c_pos_g.dig_dvgk > 0.0);
}

TEST_CASE("Single-ended EL34 Koren power stage solves DC operating point accurately", "[pentode][engine][dc]") {
    // Single-ended EL34 guitar power stage:
    // Supply B+ = 350 V
    // Plate connected to B+ via 2.5k output transformer primary impedance
    // Screen connected to 300 V via 1k screen stopper
    // Cathode connected to ground via 250 Ohm bias resistor + 47 uF bypass capacitor
    // Grid referenced to ground via 220k leak resistor
    auto c = wdf::buildFromString(
        "V_Bplus bplus 0 DC 350.0\n"
        "V_Screen screen_in 0 DC 300.0\n"
        "R_primary bplus plate 2.5k\n"
        "R_screen screen_in screen 1k\n"
        "R_grid in 0 220k\n"
        "R_k k 0 250.0\n"
        "C_k k 0 47u\n"
        "XP1 plate screen in k EL34\n");
    c.pent[0].model = wdf::kKorenEL34;

    REQUIRE(c.pent.size() == 1);
    REQUIRE(c.pent[0].name == "XP1");

    wdf::Engine engine(c, 48000.0);
    REQUIRE(engine.pentodeCount() == 1);
    REQUIRE(engine.rootCount() == 3); // 3 root ports: (plate,k), (grid,k), (screen,k)
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
    // Active screen current under full Koren 3-port: screen voltage drops below 300V
    // through the 1k screen stopper (Is = (Vsk/mu + Vgk)^ex / kg2).
    REQUIRE(vs < 300.0);
    REQUIRE(vs > 200.0); // screen voltage still in a physically reasonable range
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
        // the supply rails.
        REQUIRE(vp > -50.0);
        REQUIRE(vp < 450.0);
    }
}

TEST_CASE("Pentode grid limiter keeps the exponential representable", "[pentode][grid][limiter]") {
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
        // The limiter bounds the *step*, not the value: once the gap to the
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
        // can actually see. The limiter reduces the overshoot only logarithmically,
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

// =============================================================================
// Phase 2C.1: Reefman Pentode Evaluator Verification
// =============================================================================

TEST_CASE("Reefman Pentode analytical Jacobian matches central finite differences", "[pentode][reefman][jacobian]") {
    // Test across both knee models (Derk D, Derk E) and secondary emission (on, off)
    const wdf::PentodeKneeType knees[] = {wdf::PentodeKneeType::DerkD, wdf::PentodeKneeType::DerkE};
    const bool seModes[] = {false, true};

    // Test points from Phase 2B.2 (ep, eg, es in Volts)
    struct TestPoint {
        const char* name;
        double ep, eg, es;
    };
    const TestPoint points[] = {
        {"A (nominal) ep250 eg-40 es247", 250.0, -40.0, 247.0},
        {"B (high ep) ep390 eg-30 es247", 390.0, -30.0, 247.0},
        {"C (low ep)  ep5   eg-45 es247", 5.0,   -45.0, 247.0},
        {"D (low eg)  ep40  eg-55 es298", 40.0,  -55.0, 298.0},
        {"E (high es) ep380 eg-25 es298", 380.0, -25.0, 298.0},
        {"Nominal conduction ep250 eg-14 es250", 250.0, -14.0, 250.0},
        {"Positive grid conduction ep200 eg+1.0 es200", 200.0, 1.0, 200.0}
    };

    constexpr double h = 1e-4; // Finite-difference step

    for (auto knee : knees) {
        for (bool se : seModes) {
            wdf::ReefmanPentode p;
            p.knee = knee;
            p.secondaryEmission = se;

            const char* kneeName = (knee == wdf::PentodeKneeType::DerkD) ? "DerkD" : "DerkE";
            const char* seName = se ? "SE_ON" : "SE_OFF";

            for (const auto& pt : points) {
                DYNAMIC_SECTION(kneeName << " " << seName << " at " << pt.name) {
                    REQUIRE(p.isSupported(pt.eg, pt.es, pt.ep));

                    const auto c0 = p.eval(pt.eg, pt.es, pt.ep);
                    REQUIRE(std::isfinite(c0.ip));
                    REQUIRE(std::isfinite(c0.is));
                    REQUIRE(std::isfinite(c0.ig));
                    REQUIRE(std::isfinite(c0.dip_dvpk));
                    REQUIRE(std::isfinite(c0.dip_dvgk));
                    REQUIRE(std::isfinite(c0.dip_dvsk));
                    REQUIRE(std::isfinite(c0.dis_dvpk));
                    REQUIRE(std::isfinite(c0.dis_dvgk));
                    REQUIRE(std::isfinite(c0.dis_dvsk));
                    REQUIRE(std::isfinite(c0.dig_dvgk));

                    // 1. dip / dvpk
                    const auto c_ep_p = p.eval(pt.eg, pt.es, pt.ep + h);
                    const auto c_ep_m = p.eval(pt.eg, pt.es, pt.ep - h);
                    const double fd_dip_dvpk = (c_ep_p.ip - c_ep_m.ip) / (2.0 * h);
                    const double err_dip_dvpk = std::abs(c0.dip_dvpk - fd_dip_dvpk);
                    const double scale_dip_dvpk = std::max(std::abs(c0.dip_dvpk), 1e-6);
                    REQUIRE(err_dip_dvpk / scale_dip_dvpk < 1e-4);

                    // 2. dip / dvgk
                    const auto c_eg_p = p.eval(pt.eg + h, pt.es, pt.ep);
                    const auto c_eg_m = p.eval(pt.eg - h, pt.es, pt.ep);
                    const double fd_dip_dvgk = (c_eg_p.ip - c_eg_m.ip) / (2.0 * h);
                    const double err_dip_dvgk = std::abs(c0.dip_dvgk - fd_dip_dvgk);
                    const double scale_dip_dvgk = std::max(std::abs(c0.dip_dvgk), 1e-6);
                    REQUIRE(err_dip_dvgk / scale_dip_dvgk < 1e-4);

                    // 3. dip / dvsk
                    const auto c_es_p = p.eval(pt.eg, pt.es + h, pt.ep);
                    const auto c_es_m = p.eval(pt.eg, pt.es - h, pt.ep);
                    const double fd_dip_dvsk = (c_es_p.ip - c_es_m.ip) / (2.0 * h);
                    const double err_dip_dvsk = std::abs(c0.dip_dvsk - fd_dip_dvsk);
                    const double scale_dip_dvsk = std::max(std::abs(c0.dip_dvsk), 1e-6);
                    REQUIRE(err_dip_dvsk / scale_dip_dvsk < 1e-4);

                    // 4. dis / dvpk
                    const double fd_dis_dvpk = (c_ep_p.is - c_ep_m.is) / (2.0 * h);
                    const double err_dis_dvpk = std::abs(c0.dis_dvpk - fd_dis_dvpk);
                    const double scale_dis_dvpk = std::max(std::abs(c0.dis_dvpk), 1e-6);
                    REQUIRE(err_dis_dvpk / scale_dis_dvpk < 1e-4);

                    // 5. dis / dvgk
                    const double fd_dis_dvgk = (c_eg_p.is - c_eg_m.is) / (2.0 * h);
                    const double err_dis_dvgk = std::abs(c0.dis_dvgk - fd_dis_dvgk);
                    const double scale_dis_dvgk = std::max(std::abs(c0.dis_dvgk), 1e-6);
                    REQUIRE(err_dis_dvgk / scale_dis_dvgk < 1e-4);

                    // 6. dis / dvsk
                    const double fd_dis_dvsk = (c_es_p.is - c_es_m.is) / (2.0 * h);
                    const double err_dis_dvsk = std::abs(c0.dis_dvsk - fd_dis_dvsk);
                    const double scale_dis_dvsk = std::max(std::abs(c0.dis_dvsk), 1e-6);
                    REQUIRE(err_dis_dvsk / scale_dis_dvsk < 1e-4);
                }
            }
        }
    }
}

TEST_CASE("Reefman Pentode control-grid diode derivative matches finite differences", "[pentode][reefman][grid]") {
    wdf::ReefmanPentode p;
    constexpr double h = 1e-4;
    const double vg_test[] = {-10.0, -1.0, -0.1, -0.02, 0.0, 0.05, 0.5, 1.5, 3.0};

    for (double vg : vg_test) {
        DYNAMIC_SECTION("vg=" << vg) {
            const auto c0 = p.eval(vg, 250.0, 250.0);
            const auto c_p = p.eval(vg + h, 250.0, 250.0);
            const auto c_m = p.eval(vg - h, 250.0, 250.0);
            const double fd_dig = (c_p.ig - c_m.ig) / (2.0 * h);
            const double err = std::abs(c0.dig_dvgk - fd_dig);
            const double scale = std::max(std::abs(c0.dig_dvgk), 1e-9);
            REQUIRE(err / scale < 1e-4);
        }
    }
}

TEST_CASE("Reefman Pentode value-only evaluation produces identical currents to Jacobian evaluation", "[pentode][reefman][identity]") {
    wdf::ReefmanPentode p;
    for (auto knee : {wdf::PentodeKneeType::DerkD, wdf::PentodeKneeType::DerkE}) {
        for (bool se : {false, true}) {
            p.knee = knee;
            p.secondaryEmission = se;

            const double vgs[] = {-40.0, -20.0, -5.0, 0.5};
            const double vss[] = {150.0, 250.0, 350.0};
            const double vps[] = {10.0, 50.0, 250.0, 450.0};

            for (double vg : vgs) {
                for (double vs : vss) {
                    for (double vp : vps) {
                        double ig_val = 0.0, is_val = 0.0, ip_val = 0.0;
                        p.evalCurrents(vg, vs, vp, ig_val, is_val, ip_val);
                        const auto c = p.eval(vg, vs, vp);

                        REQUIRE(c.ig == ig_val);
                        REQUIRE(c.is == is_val);
                        REQUIRE(c.ip == ip_val);
                    }
                }
            }
        }
    }
}

TEST_CASE("Reefman Pentode count scaling scales all currents and derivatives linearly", "[pentode][reefman][count]") {
    wdf::ReefmanPentode p1;
    wdf::ReefmanPentode p2;
    p2.count = 2.0;

    for (auto knee : {wdf::PentodeKneeType::DerkD, wdf::PentodeKneeType::DerkE}) {
        p1.knee = knee;
        p2.knee = knee;
        p1.secondaryEmission = true;
        p2.secondaryEmission = true;

        const auto c1 = p1.eval(-15.0, 250.0, 200.0);
        const auto c2 = p2.eval(-15.0, 250.0, 200.0);

        REQUIRE(c2.ig == Approx(2.0 * c1.ig));
        REQUIRE(c2.is == Approx(2.0 * c1.is));
        REQUIRE(c2.ip == Approx(2.0 * c1.ip));
        REQUIRE(c2.dig_dvgk == Approx(2.0 * c1.dig_dvgk));
        REQUIRE(c2.dis_dvpk == Approx(2.0 * c1.dis_dvpk));
        REQUIRE(c2.dis_dvgk == Approx(2.0 * c1.dis_dvgk));
        REQUIRE(c2.dis_dvsk == Approx(2.0 * c1.dis_dvsk));
        REQUIRE(c2.dip_dvpk == Approx(2.0 * c1.dip_dvpk));
        REQUIRE(c2.dip_dvgk == Approx(2.0 * c1.dip_dvgk));
        REQUIRE(c2.dip_dvsk == Approx(2.0 * c1.dip_dvsk));

        double ig1, is1, ip1, ig2, is2, ip2;
        p1.evalCurrents(-15.0, 250.0, 200.0, ig1, is1, ip1);
        p2.evalCurrents(-15.0, 250.0, 200.0, ig2, is2, ip2);
        REQUIRE(ig2 == Approx(2.0 * ig1));
        REQUIRE(is2 == Approx(2.0 * is1));
        REQUIRE(ip2 == Approx(2.0 * ip1));
    }
}

TEST_CASE("Reefman Pentode supported domain is strictly enforced", "[pentode][reefman][domain]") {
    wdf::ReefmanPentode p;

    SECTION("Supported positive domain produces finite values") {
        for (double vp : {0.0, 0.1, 5.0, 250.0, 500.0}) {
            for (double vs : {1.0, 50.0, 250.0, 400.0}) {
                for (double vg : {-60.0, -20.0, -1.0, 0.0, 1.5}) {
                    REQUIRE(p.isSupported(vg, vs, vp));
                    const auto c = p.eval(vg, vs, vp);
                    REQUIRE(std::isfinite(c.ip));
                    REQUIRE(std::isfinite(c.is));
                    REQUIRE(std::isfinite(c.ig));
                    REQUIRE(std::isfinite(c.dip_dvpk));
                    REQUIRE(std::isfinite(c.dip_dvgk));
                    REQUIRE(std::isfinite(c.dip_dvsk));
                    REQUIRE(std::isfinite(c.dis_dvpk));
                    REQUIRE(std::isfinite(c.dis_dvgk));
                    REQUIRE(std::isfinite(c.dis_dvsk));
                    REQUIRE(std::isfinite(c.dig_dvgk));
                }
            }
        }
    }

    SECTION("Negative plate voltage is outside supported domain") {
        REQUIRE_FALSE(p.isSupported(-10.0, 250.0, -1.0));
        REQUIRE_FALSE(p.isSupported(-10.0, 250.0, -50.0));
        const auto c = p.eval(-10.0, 250.0, -5.0);
        // Enforces domain: plate and screen currents are 0, does NOT evaluate unstable exponential growth
        REQUIRE(c.ip == 0.0);
        REQUIRE(c.is == 0.0);
        REQUIRE(c.dip_dvpk == 0.0);
        REQUIRE(c.dis_dvpk == 0.0);
    }

    SECTION("Non-positive screen voltage is outside supported domain") {
        REQUIRE_FALSE(p.isSupported(-10.0, 0.0, 250.0));
        REQUIRE_FALSE(p.isSupported(-10.0, -50.0, 250.0));
        const auto c = p.eval(-10.0, -10.0, 250.0);
        REQUIRE(c.ip == 0.0);
        REQUIRE(c.is == 0.0);
    }
}

// =============================================================================
// Phase 2C.2: Solver Integration & Screen-Resistor Verification
// =============================================================================

TEST_CASE("Reefman Pentode with screen resistor exhibits physical screen coupling and operating point movement", "[pentode][reefman][screen_resistor]") {
    // Single-ended power stage with Reefman pentode model.
    // Screen is fed from 300 V through R_screen.
    auto buildStage = [](double rScreen, wdf::PentodeKneeType knee, bool se) {
        auto c = wdf::buildFromString(
            "V_Bplus bplus 0 DC 350.0\n"
            "V_Screen screen_in 0 DC 300.0\n"
            "R_primary bplus plate 2.5k\n"
            "R_screen screen_in screen " + std::to_string(rScreen) + "\n"
            "R_grid in 0 220k\n"
            "R_k k 0 250.0\n"
            "C_k k 0 47u\n"
            "XP1 plate screen in k EL34\n");

        wdf::ReefmanPentode rp;
        rp.knee = knee;
        rp.secondaryEmission = se;
        c.pent[0].model = wdf::Pentode::makeReefman(rp);
        return std::make_pair(c, rp);
    };

    const auto [c1k, rp1k] = buildStage(1000.0, wdf::PentodeKneeType::DerkD, false);
    const auto [c10k, rp10k] = buildStage(10000.0, wdf::PentodeKneeType::DerkD, false);

    wdf::Engine engine1k(c1k, 48000.0);
    wdf::Engine engine10k(c10k, 48000.0);

    REQUIRE(engine1k.rootCount() == 3);
    REQUIRE(engine10k.rootCount() == 3);

    std::vector<double> node1k, rootV1k;
    std::vector<double> node10k, rootV10k;

    const bool conv1k = engine1k.solveDc(node1k, rootV1k);
    const bool conv10k = engine10k.solveDc(node10k, rootV10k);

    REQUIRE(conv1k);
    REQUIRE(conv10k);

    const int plate1k = engine1k.nodeIndex("plate");
    const int screen1k = engine1k.nodeIndex("screen");
    const int k1k = engine1k.nodeIndex("k");
    const int in1k = engine1k.nodeIndex("in");

    const double vp_1k = node1k[static_cast<size_t>(plate1k)];
    const double vs_1k = node1k[static_cast<size_t>(screen1k)];
    const double vk_1k = node1k[static_cast<size_t>(k1k)];
    const double vin_1k = node1k[static_cast<size_t>(in1k)];

    const int plate10k = engine10k.nodeIndex("plate");
    const int screen10k = engine10k.nodeIndex("screen");
    const int k10k = engine10k.nodeIndex("k");
    const int in10k = engine10k.nodeIndex("in");

    const double vp_10k = node10k[static_cast<size_t>(plate10k)];
    const double vs_10k = node10k[static_cast<size_t>(screen10k)];
    const double vk_10k = node10k[static_cast<size_t>(k10k)];
    const double vin_10k = node10k[static_cast<size_t>(in10k)];

    // 1. Screen carries physical current under the 3-port model:
    const double is_1k = (300.0 - vs_1k) / 1000.0;
    const double is_10k = (300.0 - vs_10k) / 10000.0;
    const double ip_1k = (350.0 - vp_1k) / 2500.0;
    const double ip_10k = (350.0 - vp_10k) / 2500.0;

    REQUIRE(is_1k > 1e-4);   // Significant positive screen current (~mA range)
    REQUIRE(is_10k > 1e-4);
    REQUIRE(vs_1k < 300.0);   // Voltage drop across 1k screen resistor
    REQUIRE(vs_10k < 300.0);  // Voltage drop across 10k screen resistor

    // 2. Demonstrate that changing screen resistance changes screen voltage, screen current and plate OP:
    // With larger screen resistance:
    // - Screen voltage drops significantly
    REQUIRE(vs_10k < vs_1k - 10.0);
    // - Screen current drops
    REQUIRE(is_10k < is_1k);
    // - Lower screen voltage reduces space-charge current, so plate current drops
    REQUIRE(ip_10k < ip_1k);
    // - Lower plate current means less drop across R_primary, so plate voltage rises
    REQUIRE(vp_10k > vp_1k + 5.0);
    // - Cathode voltage drops due to reduced total cathode current
    REQUIRE(vk_10k < vk_1k);

    // 3. Independent constitutive law check and KCL verification at converged OP:
    {
        const double vpk = vp_1k - vk_1k;
        const double vgk = vin_1k - vk_1k;
        const double vsk = vs_1k - vk_1k;
        const auto cur = rp1k.eval(vgk, vsk, vpk);
        // Constitutive current matches circuit branch current
        REQUIRE(cur.ip == Approx(ip_1k).epsilon(1e-4));
        REQUIRE(cur.is == Approx(is_1k).epsilon(1e-4));
        // KCL at cathode: Ik = Ip + Is + Ig
        const double ik = vk_1k / 250.0;
        REQUIRE(ik == Approx(ip_1k + is_1k + cur.ig).epsilon(1e-4));
    }

    {
        const double vpk = vp_10k - vk_10k;
        const double vgk = vin_10k - vk_10k;
        const double vsk = vs_10k - vk_10k;
        const auto cur = rp10k.eval(vgk, vsk, vpk);
        REQUIRE(cur.ip == Approx(ip_10k).epsilon(1e-4));
        REQUIRE(cur.is == Approx(is_10k).epsilon(1e-4));
        const double ik = vk_10k / 250.0;
        REQUIRE(ik == Approx(ip_10k + is_10k + cur.ig).epsilon(1e-4));
    }
}

TEST_CASE("Reefman Pentode audio transient simulation converges with fresh residuals below tolerance", "[pentode][reefman][transient]") {
    // Test across DerkD and DerkE knee types with audio rate stepping
    for (auto knee : {wdf::PentodeKneeType::DerkD, wdf::PentodeKneeType::DerkE}) {
        auto c = wdf::buildFromString(
            "V_Bplus bplus 0 DC 350.0\n"
            "V_Screen screen_in 0 DC 300.0\n"
            "V_Sig in 0 DC 0.0 AC 1.0 SIN(0.0 5.0 1000)\n"
            "R_primary bplus plate 2.5k\n"
            "R_screen screen_in screen 2.2k\n"
            "R_k k 0 250.0\n"
            "C_k k 0 47u\n"
            "XP1 plate screen in k EL34\n");

        wdf::ReefmanPentode rp;
        rp.knee = knee;
        rp.secondaryEmission = false;
        c.pent[0].model = wdf::Pentode::makeReefman(rp);

        wdf::Engine engine(c, 48000.0);
        engine.reset();

        const int plateIdx = engine.nodeIndex("plate");
        const int screenIdx = engine.nodeIndex("screen");
        const int kIdx = engine.nodeIndex("k");
        REQUIRE(plateIdx >= 0);
        REQUIRE(screenIdx >= 0);
        REQUIRE(kIdx >= 0);

        constexpr int kSamples = 480; // 10 ms at 48 kHz
        for (int n = 0; n < kSamples; ++n) {
            const double t = static_cast<double>(n) / 48000.0;
            const double vin = 5.0 * std::sin(2.0 * M_PI * 1000.0 * t);
            const auto& nodes = engine.step({350.0, 300.0, vin});

            // Requirement 6: Verify convergence and fresh residuals
            REQUIRE(engine.lastConverged());
            REQUIRE(engine.lastResidual() < 1e-9);

            const double vp = nodes[static_cast<size_t>(plateIdx)];
            const double vs = nodes[static_cast<size_t>(screenIdx)];
            const double vk = nodes[static_cast<size_t>(kIdx)];

            // Physical sanity
            REQUIRE(std::isfinite(vp));
            REQUIRE(std::isfinite(vs));
            REQUIRE(std::isfinite(vk));
            REQUIRE(vs > 0.0);
            REQUIRE(vs < 300.0);
            REQUIRE(vp > 0.0);
            REQUIRE(vp < 450.0);
        }
    }
}

// =============================================================================
// Benchmark: Reefman Pentode vs Koren Pentode
// =============================================================================

TEST_CASE("Pentode Benchmark: Reefman vs Koren Model", "[pentode][bench]") {
    using clock = std::chrono::high_resolution_clock;

    // -------------------------------------------------------------------------
    // 1. Raw Evaluator Microbenchmark
    // -------------------------------------------------------------------------
    constexpr int kEvals = 1000000;
    const wdf::Pentode korenModel = wdf::kKorenEL34;
    wdf::ReefmanPentode reefmanD;
    reefmanD.knee = wdf::PentodeKneeType::DerkD;
    reefmanD.secondaryEmission = false;

    wdf::ReefmanPentode reefmanE;
    reefmanE.knee = wdf::PentodeKneeType::DerkE;
    reefmanE.secondaryEmission = false;

    wdf::ReefmanPentode reefmanD_SE;
    reefmanD_SE.knee = wdf::PentodeKneeType::DerkD;
    reefmanD_SE.secondaryEmission = true;

    // Synthetic varying inputs to prevent optimizer loop-hoisting
    std::vector<double> vgk(1024), vsk(1024), vpk(1024);
    for (size_t i = 0; i < 1024; ++i) {
        vgk[i] = -25.0 + 20.0 * std::sin(0.01 * static_cast<double>(i));
        vsk[i] = 250.0 + 30.0 * std::cos(0.013 * static_cast<double>(i));
        vpk[i] = 250.0 + 100.0 * std::sin(0.017 * static_cast<double>(i));
    }

    std::printf("\n=================================================================\n");
    std::printf("  PENTODE PERFORMANCE BENCHMARK (Release Build)\n");
    std::printf("=================================================================\n");
    std::printf("--- 1. Raw Constitutive Evaluator (%d calls) ---\n", kEvals);

    // Warmup
    double sink = 0.0;
    for (int i = 0; i < 10000; ++i) {
        sink += korenModel.eval(vgk[i & 1023], vsk[i & 1023], vpk[i & 1023]).ip;
    }

    // Benchmark Koren
    {
        const auto t0 = clock::now();
        for (int i = 0; i < kEvals; ++i) {
            const auto c = korenModel.eval(vgk[i & 1023], vsk[i & 1023], vpk[i & 1023]);
            sink += c.ip + c.dip_dvpk;
        }
        const auto t1 = clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kEvals;
        std::printf("  Koren Pentode (eval):         %6.2f ns/call  (%6.2f M evals/s)\n", ns, 1000.0 / ns);
    }

    // Benchmark Reefman DerkD
    {
        const auto t0 = clock::now();
        for (int i = 0; i < kEvals; ++i) {
            const auto c = reefmanD.eval(vgk[i & 1023], vsk[i & 1023], vpk[i & 1023]);
            sink += c.ip + c.dip_dvpk + c.dis_dvsk;
        }
        const auto t1 = clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kEvals;
        std::printf("  Reefman DerkD (eval):         %6.2f ns/call  (%6.2f M evals/s)\n", ns, 1000.0 / ns);
    }

    // Benchmark Reefman DerkE
    {
        const auto t0 = clock::now();
        for (int i = 0; i < kEvals; ++i) {
            const auto c = reefmanE.eval(vgk[i & 1023], vsk[i & 1023], vpk[i & 1023]);
            sink += c.ip + c.dip_dvpk + c.dis_dvsk;
        }
        const auto t1 = clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kEvals;
        std::printf("  Reefman DerkE (eval):         %6.2f ns/call  (%6.2f M evals/s)\n", ns, 1000.0 / ns);
    }

    // Benchmark Reefman DerkD + Secondary Emission
    {
        const auto t0 = clock::now();
        for (int i = 0; i < kEvals; ++i) {
            const auto c = reefmanD_SE.eval(vgk[i & 1023], vsk[i & 1023], vpk[i & 1023]);
            sink += c.ip + c.dip_dvpk + c.dis_dvsk;
        }
        const auto t1 = clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kEvals;
        std::printf("  Reefman DerkD+SE (eval):      %6.2f ns/call  (%6.2f M evals/s)\n", ns, 1000.0 / ns);
    }

    // Benchmark Reefman Value-Only (no Jacobian)
    {
        const auto t0 = clock::now();
        for (int i = 0; i < kEvals; ++i) {
            double ig, is, ip;
            reefmanD.evalCurrents(vgk[i & 1023], vsk[i & 1023], vpk[i & 1023], ig, is, ip);
            sink += ip + is;
        }
        const auto t1 = clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kEvals;
        std::printf("  Reefman DerkD (evalCurrents): %6.2f ns/call  (%6.2f M evals/s)\n", ns, 1000.0 / ns);
    }

    // -------------------------------------------------------------------------
    // 2. Engine Transient Simulation Benchmark (48,000 samples @ 48 kHz)
    // -------------------------------------------------------------------------
    constexpr int kSamples = 48000;
    std::printf("\n--- 2. Single-Ended Power Stage Simulation (%d samples @ 48 kHz) ---\n", kSamples);

    auto runStageBench = [&](const char* label, const wdf::Pentode& model) {
        auto c = wdf::buildFromString(
            "V_Bplus bplus 0 DC 350.0\n"
            "V_Screen screen_in 0 DC 300.0\n"
            "V_Sig in 0 DC 0.0 AC 1.0 SIN(0.0 5.0 1000)\n"
            "R_primary bplus plate 2.5k\n"
            "R_screen screen_in screen 1k\n"
            "R_k k 0 250.0\n"
            "C_k k 0 47u\n"
            "XP1 plate screen in k EL34\n");
        c.pent[0].model = model;

        wdf::Engine engine(c, 48000.0);
        engine.reset();

        // Warmup
        for (int i = 0; i < 1000; ++i) {
            engine.step({350.0, 300.0, 0.0});
        }
        engine.reset();

        // Measure
        long long iters = 0;
        const auto t0 = clock::now();
        for (int n = 0; n < kSamples; ++n) {
            const double t = static_cast<double>(n) / 48000.0;
            const double vin = 5.0 * std::sin(2.0 * M_PI * 1000.0 * t);
            engine.step({350.0, 300.0, vin});
            iters += engine.newtonIterations();
        }
        const auto t1 = clock::now();

        const double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double nsPerSample = (totalMs * 1e6) / kSamples;
        const double avgIters = static_cast<double>(iters) / kSamples;
        const double xRealtime = (1.0 / 48000.0) / (nsPerSample * 1e-9);

        std::printf("  %-28s: %6.1f ns/sample  (avg %4.2f iters) -> %6.1fx Realtime\n",
                    label, nsPerSample, avgIters, xRealtime);
    };

    runStageBench("Koren Pentode (3 unknowns)", korenModel);
    runStageBench("Reefman DerkD (3 unknowns)", wdf::Pentode::makeReefman(reefmanD));
    runStageBench("Reefman DerkE (3 unknowns)", wdf::Pentode::makeReefman(reefmanE));
    runStageBench("Reefman DerkD+SE (3 unknowns)", wdf::Pentode::makeReefman(reefmanD_SE));
    std::printf("=================================================================\n\n");

    CHECK(sink != 0.0);
}
