#include <catch2.hpp>

#include <cmath>
#include <string>
#include <vector>

#include "devices/Diode.h"
#include "spice/Net.h"
#include "spice/SpiceNet.h"
#include "wdf/Engine.h"

// Increment 16: Cathodyne diode presets + 3-terminal potentiometer expansion.

TEST_CASE("named diode registry provides Cathodyne's 6 presets", "[diode][registry]") {
    REQUIRE(wdf::kNumDiodes == 6);

    const auto* d4148 = wdf::findDiodeByName("1N4148");
    REQUIRE(d4148 != nullptr);
    REQUIRE(d4148->isat == Approx(4.352e-9));
    REQUIRE(d4148->n == Approx(1.905));
    REQUIRE(d4148->rs == Approx(0.56));
    REQUIRE(d4148->rp == Approx(1e6));

    const auto* d914 = wdf::findDiodeByName("1N914");
    REQUIRE(d914 != nullptr);
    REQUIRE(d914->isat == Approx(4.352e-9));

    const auto* d4007 = wdf::findDiodeByName("1N4007");
    REQUIRE(d4007 != nullptr);
    REQUIRE(d4007->isat == Approx(1.0e-6));
    REQUIRE(d4007->rs == Approx(0.04));

    const auto* d34a = wdf::findDiodeByName("1N34A");
    REQUIRE(d34a != nullptr);
    REQUIRE(d34a->isat == Approx(1.0e-6));
    REQUIRE(d34a->rs == Approx(5.0));

    const auto* d5819 = wdf::findDiodeByName("1N5819");
    REQUIRE(d5819 != nullptr);
    REQUIRE(d5819->isat == Approx(1.0e-6));
    REQUIRE(d5819->rs == Approx(0.05));

    const auto* dgz34 = wdf::findDiodeByName("GZ34");
    REQUIRE(dgz34 != nullptr);
    REQUIRE(dgz34->isat == Approx(2.52e-9));
    REQUIRE(dgz34->rs == Approx(15.0));

    // Case-insensitivity and SPICE leading 'D' tolerance
    REQUIRE(wdf::findDiodeByName("1n4148") == d4148);
    REQUIRE(wdf::findDiodeByName("D1N4148") == d4148);
    REQUIRE(wdf::findDiodeByName("d1n4007") == d4007);
    REQUIRE(wdf::findDiodeByName("dgz34") == dgz34);

    REQUIRE(wdf::findDiodeByName("NONEXISTENT_DIODE") == nullptr);
}

TEST_CASE("SPICE parser resolves named diode presets without boilerplate .model", "[diode][spice]") {
    // 1. Direct use of built-in preset
    const auto c = wdf::buildFromString(
        "V1 in 0 DC 1.0\n"
        "R1 in out 1k\n"
        "D1 out 0 1N4148\n");
    REQUIRE(c.diode.size() == 1);
    REQUIRE(c.diode[0].model.isat == Approx(4.352e-9));
    REQUIRE(c.diode[0].model.rs == Approx(0.56));
    REQUIRE(c.diode[0].model.rp == Approx(1e6));

    // 2. Leading 'D' variant in netlist
    const auto c2 = wdf::buildFromString(
        "V1 in 0 DC 1.0\n"
        "R1 in out 1k\n"
        "D1 out 0 D1N4007\n");
    REQUIRE(c2.diode.size() == 1);
    REQUIRE(c2.diode[0].model.isat == Approx(1.0e-6));
    REQUIRE(c2.diode[0].model.rs == Approx(0.04));

    // 3. Explicit .model line overrides the built-in preset
    const auto c3 = wdf::buildFromString(
        "V1 in 0 DC 1.0\n"
        "R1 in out 1k\n"
        "D1 out 0 1N4148\n"
        ".model 1N4148 D (IS=2.5e-9 N=1.1 RS=1.2)\n");
    REQUIRE(c3.diode.size() == 1);
    REQUIRE(c3.diode[0].model.isat == Approx(2.5e-9));
    REQUIRE(c3.diode[0].model.n == Approx(1.1));
    REQUIRE(c3.diode[0].model.rs == Approx(1.2));

    // 4. Unknown diode model still throws
    REQUIRE_THROWS_AS(wdf::buildFromString("D1 a b UNKNOWN_DIODE\n"), std::runtime_error);
}

TEST_CASE("potentiometer taper formulas compute correct divider ratios", "[pot][taper]") {
    constexpr double kTotal = 100e3;

    // Linear: 50% position -> exactly 50k / 50k
    {
        auto [r1, r2] = wdf::Pot::resistances(kTotal, 0.5, wdf::PotTaper::Linear);
        REQUIRE(r1 == Approx(50e3));
        REQUIRE(r2 == Approx(50e3));
    }
    // Linear at 25%
    {
        auto [r1, r2] = wdf::Pot::resistances(kTotal, 0.25, wdf::PotTaper::Linear);
        REQUIRE(r1 == Approx(75e3));
        REQUIRE(r2 == Approx(25e3));
    }
    // Log taper (audio): r = (10^pos - 1) / 9. At pos=0.5 -> r = (3.162278-1)/9 = 0.240253
    {
        auto [r1, r2] = wdf::Pot::resistances(kTotal, 0.5, wdf::PotTaper::Log);
        REQUIRE(r1 == Approx(75.9747e3).epsilon(1e-5));
        REQUIRE(r2 == Approx(24.0253e3).epsilon(1e-5));
    }
    // RevLog taper (anti-log): r = 1 - (10^(1-pos) - 1)/9. At pos=0.5 -> r = 0.759747
    {
        auto [r1, r2] = wdf::Pot::resistances(kTotal, 0.5, wdf::PotTaper::RevLog);
        REQUIRE(r1 == Approx(24.0253e3).epsilon(1e-5));
        REQUIRE(r2 == Approx(75.9747e3).epsilon(1e-5));
    }
    // Extreme positions: clamped to epsilon, conductances stay finite
    {
        auto [r1, r2] = wdf::Pot::resistances(kTotal, 0.0, wdf::PotTaper::Linear);
        REQUIRE(r1 > 99e3);
        REQUIRE(r2 > 0.0);
        REQUIRE(r2 == Approx(1e-4 * kTotal));
    }
    {
        auto [r1, r2] = wdf::Pot::resistances(kTotal, 1.0, wdf::PotTaper::Linear);
        REQUIRE(r2 > 99e3);
        REQUIRE(r1 > 0.0);
        REQUIRE(r1 == Approx(1e-4 * kTotal));
    }
}

TEST_CASE("potentiometer expands at parse time and updates dynamically", "[pot][engine]") {
    // A 10 V source feeding a 100k potentiometer, output taken from wiper
    const auto c = wdf::buildFromString(
        "V1 in 0 DC 10.0\n"
        "P1 in out 0 100k pos=0.5 LIN\n"
        "Rload out 0 1Meg\n");

    REQUIRE(c.pots.size() == 1);
    REQUIRE(c.pots[0].name == "P1");
    REQUIRE(c.pots[0].position == Approx(0.5));
    REQUIRE(c.pots[0].taper == wdf::PotTaper::Linear);

    // Two synthetic resistors were created for the potentiometer
    REQUIRE(c.res.size() == 3);  // P1_1, P1_2, Rload

    wdf::Engine engine(c, 48000.0);
    const int outIdx = engine.nodeIndex("out");
    REQUIRE(outIdx >= 0);

    // 1. Initial operating point at pos=0.5: wiper should sit close to 5.0 V
    // (slightly loaded by 1M load: 50k || 1M divider -> ~4.88 V)
    std::vector<double> node, rootV;
    REQUIRE(engine.solveDc(node, rootV));
    REQUIRE(node[static_cast<size_t>(outIdx)] == Approx(4.878).epsilon(0.01));

    // 2. Move pot to pos=0.8 (closer to top rail 'in' = 10 V)
    REQUIRE(engine.setPotPosition("P1", 0.8));
    REQUIRE(engine.solveDc(node, rootV));
    // At pos=0.8: r1=20k, r2=80k. With 1M load: 80k || 1M = 74.07k -> Vout = 10 * 74.07 / (20 + 74.07) = 7.87 V
    REQUIRE(node[static_cast<size_t>(outIdx)] == Approx(7.87).epsilon(0.01));

    // 3. Move pot to pos=0.2 (closer to ground)
    REQUIRE(engine.setPotPosition("P1", 0.2));
    REQUIRE(engine.solveDc(node, rootV));
    // At pos=0.2: r1=80k, r2=20k. With 1M load: 20k || 1M = 19.61k -> Vout = 10 * 19.61 / (80 + 19.61) = 1.97 V
    REQUIRE(node[static_cast<size_t>(outIdx)] == Approx(1.97).epsilon(0.01));

    // 4. Out-of-bounds name or index returns false
    REQUIRE_FALSE(engine.setPotPosition("NONEXISTENT_POT", 0.5));
    REQUIRE_FALSE(engine.setPotPosition(-1, 0.5));
    REQUIRE_FALSE(engine.setPotPosition(5, 0.5));

    // 5. Advancing samples while dynamically modulating the pot
    engine.reset();
    for (int n = 0; n < 480; ++n) {
        const double p = 0.5 + 0.4 * std::sin(2.0 * 3.14159265358979323846 * 10.0 * n / 48000.0);
        REQUIRE(engine.setPotPosition(0, p));
        const auto& y = engine.step({10.0});
        REQUIRE(engine.lastConverged());
        REQUIRE(std::isfinite(y[static_cast<size_t>(outIdx)]));
        REQUIRE(y[static_cast<size_t>(outIdx)] > 0.5);
        REQUIRE(y[static_cast<size_t>(outIdx)] < 9.5);
    }
}

TEST_CASE("SPICE parser accepts pot with various taper keywords and rejects malformed shapes", "[pot][spice]") {
    // Audio / Log
    const auto cLog = wdf::buildFromString("P1 in out 0 250k pos=0.3 LOG\n");
    REQUIRE(cLog.pots[0].taper == wdf::PotTaper::Log);
    REQUIRE(cLog.pots[0].position == Approx(0.3));

    // Reverse log
    const auto cRev = wdf::buildFromString("P1 in out 0 500k pos=0.7 REVLOG\n");
    REQUIRE(cRev.pots[0].taper == wdf::PotTaper::RevLog);
    REQUIRE(cRev.pots[0].position == Approx(0.7));

    // Default parameters (pos=0.5, Linear)
    const auto cDef = wdf::buildFromString("P1 in out 0 100k\n");
    REQUIRE(cDef.pots[0].taper == wdf::PotTaper::Linear);
    REQUIRE(cDef.pots[0].position == Approx(0.5));

    // Malformed: missing resistance
    REQUIRE_THROWS_AS(wdf::buildFromString("P1 in out 0\n"), std::runtime_error);
    // Malformed: missing terminals
    REQUIRE_THROWS_AS(wdf::buildFromString("P1 in out\n"), std::runtime_error);
}
