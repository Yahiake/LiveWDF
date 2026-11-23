#include <catch2.hpp>

#include <cmath>
#include <string>
#include <vector>

#include <math.h>

#include "spice/Net.h"
#include "spice/SpiceNet.h"
#include "wdf/Engine.h"

TEST_CASE("Photoresistor responds to differential light drive with separate attack and release",
          "[photores][engine]") {
    wdf::Circuit circuit;
    circuit.addSrc("input", "0", 1.0);
    circuit.addSrc("positive", "0", 1.75);
    circuit.addSrc("negative", "0", 1.25);
    circuit.addRes("input", "out", 10000.0);
    wdf::PhotoresistorControl photo;
    photo.sensePos = "positive";
    photo.senseNeg = "negative";
    photo.brightOhms = 1000.0;
    photo.attackSeconds = 0.004;
    photo.releaseSeconds = 0.040;
    circuit.addPhotoresistor("out", "0", 100000.0, photo, "LDR");

    constexpr double fs = 48000.0;
    wdf::Engine engine(circuit, fs);
    const int out = engine.nodeIndex("out");
    REQUIRE(out >= 0);
    const std::vector<double> dc{1.0, 1.75, 1.25};
    const double dcConductance = 0.5 * (1.0 / 100000.0 + 1.0 / 1000.0);
    CHECK(engine.step(dc)[out] == Approx(1.0 / (1.0 + 10000.0 * dcConductance)));

    const std::vector<double> bright{1.0, 2.25, 1.25};
    for (int i = 0; i < 192; ++i)
        engine.step(bright);
    const double turnedOn = 1.0 / engine.controlledResistanceValues().at(0);
    CHECK(turnedOn == Approx(0.001 + std::exp(-1.0) * (dcConductance - 0.001)));

    const std::vector<double> dark{1.0, 0.25, 1.25};
    for (int i = 0; i < 192; ++i)
        engine.step(dark);
    const double turnedOff = 1.0 / engine.controlledResistanceValues().at(0);
    CHECK(turnedOff == Approx(1e-5 + std::exp(-0.1) * (turnedOn - 1e-5)));

    engine.reset();
    CHECK(engine.step(dc)[out] == Approx(1.0 / (1.0 + 10000.0 * dcConductance)));
}

// Increment 18: Variable Resistors (VarRes rheostats) & Controlled Resistors (audio-rate root elements).

TEST_CASE("VarRes taper curves and resistance scaling", "[varres]") {
    constexpr double kMaxR = 100e3;

    // Linear taper: r = pos * maxR
    REQUIRE(wdf::VarRes::resistance(kMaxR, 0.5, wdf::PotTaper::Linear) == Approx(50e3));
    REQUIRE(wdf::VarRes::resistance(kMaxR, 0.2, wdf::PotTaper::Linear) == Approx(20e3));
    REQUIRE(wdf::VarRes::resistance(kMaxR, 0.8, wdf::PotTaper::Linear) == Approx(80e3));

    // Log taper: r = (10^pos - 1)/9 * maxR. At pos=0.5 -> (3.162278-1)/9 = 0.240253
    REQUIRE(wdf::VarRes::resistance(kMaxR, 0.5, wdf::PotTaper::Log) == Approx(24.0253e3).epsilon(1e-5));

    // RevLog taper: r = (1 - (10^(1-pos) - 1)/9) * maxR. At pos=0.5 -> 0.759747 * 100k = 75.9747k
    REQUIRE(wdf::VarRes::resistance(kMaxR, 0.5, wdf::PotTaper::RevLog) == Approx(75.9747e3).epsilon(1e-5));

    // Clamping at boundaries: conductances stay finite
    REQUIRE(wdf::VarRes::resistance(kMaxR, 0.0, wdf::PotTaper::Linear) == Approx(wdf::VarRes::kEps * kMaxR));
    REQUIRE(wdf::VarRes::resistance(kMaxR, 1.0, wdf::PotTaper::Linear) == Approx(kMaxR));
}

TEST_CASE("VarRes 2-terminal rheostat adjusts in engine dynamically", "[varres][engine]") {
    // 10 V source feeding a 10k fixed resistor in series with a 10k VarRes rheostat to ground
    const auto c = wdf::buildFromString(
        "V1 in 0 DC 10.0\n"
        "R1 in out 10k\n"
        "VR_Bias out 0 10k pos=0.5 LIN\n");

    REQUIRE(c.varRes.size() == 1);
    REQUIRE(c.varRes[0].name == "VR_Bias");
    REQUIRE(c.varRes[0].maxResistance == 10000.0);
    REQUIRE(c.varRes[0].position == Approx(0.5));

    wdf::Engine engine(c, 48000.0);
    const int outIdx = engine.nodeIndex("out");
    REQUIRE(outIdx >= 0);

    // 1. Initial position 0.5 -> VR_Bias = 5k. Vout = 10 * 5k / (10k + 5k) = 3.333 V
    std::vector<double> node, rootV;
    REQUIRE(engine.solveDc(node, rootV));
    REQUIRE(node[static_cast<size_t>(outIdx)] == Approx(3.3333).epsilon(0.01));

    // 2. Set position to 1.0 -> VR_Bias = 10k. Vout = 10 * 10k / (10k + 10k) = 5.0 V
    REQUIRE(engine.setVarResPosition("VR_Bias", 1.0));
    REQUIRE(engine.solveDc(node, rootV));
    REQUIRE(node[static_cast<size_t>(outIdx)] == Approx(5.0000).epsilon(0.01));

    // 3. Set position to 0.2 -> VR_Bias = 2k. Vout = 10 * 2k / (10k + 2k) = 1.667 V
    REQUIRE(engine.setVarResPosition(0, 0.2));
    REQUIRE(engine.solveDc(node, rootV));
    REQUIRE(node[static_cast<size_t>(outIdx)] == Approx(1.6667).epsilon(0.01));

    // 4. Invalid index and name checks
    REQUIRE_FALSE(engine.setVarResPosition("NONEXISTENT", 0.5));
    REQUIRE_FALSE(engine.setVarResPosition(-1, 0.5));
    REQUIRE_FALSE(engine.setVarResPosition(10, 0.5));
}

TEST_CASE("a control write that did not move does not rebuild the network", "[varres][engine][controls]") {
    const auto c = wdf::buildFromString(
        "V1 in 0 DC 10.0\n"
        "R1 in out 10k\n"
        "VR_Bias out 0 10k pos=0.5 LIN\n");

    wdf::Engine engine(c, 48000.0);
    const long long initial = engine.linearRebuilds();

    // This is what the plugin does once per block for every grouped control.
    // It must be a no-op, not a ~25 us transfer rebuild.
    REQUIRE(engine.setVarResPosition("VR_Bias", 0.5));
    REQUIRE(engine.linearRebuilds() == initial);
    REQUIRE(engine.setResistance("R1", 10000.0));
    REQUIRE(engine.linearRebuilds() == initial);

    REQUIRE(engine.setVarResPosition("VR_Bias", 0.75));
    REQUIRE(engine.linearRebuilds() == initial + 1);

    REQUIRE_FALSE(engine.setVarResPosition("NONEXISTENT", 0.5));
    REQUIRE(engine.linearRebuilds() == initial + 1);
}

TEST_CASE("ControlledRes dynamic audio-rate modulation solved in closed form", "[ctrlres][engine]") {
    // A 5 V DC source feeding a 1k fixed resistor and a ControlledRes to ground
    const auto c = wdf::buildFromString(
        "V1 in 0 DC 5.0\n"
        "R1 in out 1k\n"
        "CR_LDR out 0 1k\n");

    REQUIRE(c.ctrlRes.size() == 1);
    REQUIRE(c.ctrlRes[0].name == "CR_LDR");
    REQUIRE(c.ctrlRes[0].initialOhms == 1000.0);

    wdf::Engine engine(c, 48000.0);
    REQUIRE(engine.controlledResistorCount() == 1);
    REQUIRE(engine.rootCount() == 1);
    REQUIRE(engine.allowsExplicitRoot());

    const int outIdx = engine.nodeIndex("out");
    REQUIRE(outIdx >= 0);

    // Initial operating point at R=1k: Vout = 5 * 1k / (1k + 1k) = 2.5 V
    std::vector<double> node, rootV;
    REQUIRE(engine.solveDc(node, rootV));
    REQUIRE(node[static_cast<size_t>(outIdx)] == Approx(2.5000).epsilon(0.001));

    // Step in closed-form root mode vs forced Newton mode
    engine.reset();
    wdf::Engine engineNewton(c, 48000.0);
    engineNewton.setExplicitRoots(false);
    engineNewton.reset();

    constexpr int kSamples = 4800; // 100 ms at 48 kHz
    for (int n = 0; n < kSamples; ++n) {
        // Modulate resistance between 500 Ohm and 2500 Ohm at 20 Hz (e.g. optical tremolo / Vactrol)
        const double t = static_cast<double>(n) / 48000.0;
        const double R = 1500.0 + 1000.0 * std::sin(2.0 * M_PI * 20.0 * t);

        engine.setControlledResistance(0, R);
        engineNewton.setControlledResistance("CR_LDR", R);

        const auto& yClosed = engine.step({5.0});
        const auto& yNewton = engineNewton.step({5.0});

        const double vExpected = 5.0 * (R / (1000.0 + R));
        REQUIRE(yClosed[static_cast<size_t>(outIdx)] == Approx(vExpected).epsilon(1e-6));
        REQUIRE(yNewton[static_cast<size_t>(outIdx)] == Approx(vExpected).epsilon(1e-6));
        REQUIRE(std::abs(yClosed[static_cast<size_t>(outIdx)] - yNewton[static_cast<size_t>(outIdx)]) < 1e-10);
    }
}

TEST_CASE("SPICE parser accepts VarRes and ControlledRes keywords", "[varres][ctrlres][spice]") {
    const auto c = wdf::buildFromString(
        "VR_Presence nodeA nodeB 25k pos=0.8 REVLOG\n"
        "VR_Mid nodeB nodeC 10k\n"
        "CR_Vactrol nodeC 0 2500\n"
        "CR_Default nodeC 0\n");

    REQUIRE(c.varRes.size() == 2);
    REQUIRE(c.varRes[0].name == "VR_Presence");
    REQUIRE(c.varRes[0].maxResistance == 25000.0);
    REQUIRE(c.varRes[0].position == Approx(0.8));
    REQUIRE(c.varRes[0].taper == wdf::PotTaper::RevLog);

    REQUIRE(c.varRes[1].name == "VR_Mid");
    REQUIRE(c.varRes[1].maxResistance == 10000.0);
    REQUIRE(c.varRes[1].position == Approx(0.5));
    REQUIRE(c.varRes[1].taper == wdf::PotTaper::Linear);

    REQUIRE(c.ctrlRes.size() == 2);
    REQUIRE(c.ctrlRes[0].name == "CR_Vactrol");
    REQUIRE(c.ctrlRes[0].initialOhms == 2500.0);
    REQUIRE(c.ctrlRes[1].name == "CR_Default");
    REQUIRE(c.ctrlRes[1].initialOhms == 1000.0);
}

TEST_CASE("linear VCA follows differential control and external load", "[vca][engine]") {
    for (const double load : {2200.0, 4400.0, 8800.0}) {
        wdf::Circuit circuit;
        circuit.addSrc("in", "0", 4.6, 0.0, {}, "input");
        circuit.addSrc("ctl", "0", 5.5, 0.0, {}, "control");
        circuit.addSrc("ref", "0", 4.5, 0.0, {}, "reference");
        circuit.addLinearVca("in", "out", 2200.0, {"ctl", "ref", 1.0}, "IC1");
        circuit.addRes("out", "ref", load, "Rload");
        wdf::Engine engine(circuit, 48000.0);
        const int out = engine.nodeIndex("out");
        REQUIRE(out >= 0);
        engine.reset();
        const auto linearRebuilds = engine.linearRebuilds();
        CHECK(engine.step({4.6, 5.5, 4.5})[out] == Approx(4.5 - 0.1 * load / 4400.0).margin(1e-7));
        for (const double delta : {-0.2, 0.0, 0.1, 0.5, 1.0, 1.4, 1.8}) {
            const std::vector<double> sources{4.6, 4.5 + delta, 4.5};
            engine.step(sources);
            const double gain = std::clamp(delta, 1e-5, 1.6);
            const double expected = 4.5 - 0.1 * load * gain / 4400.0;
            CHECK(engine.step(sources)[out] == Approx(expected).margin(1e-7));
            CHECK(engine.lastConverged());
        }
        CHECK(engine.linearRebuilds() == linearRebuilds);
    }
}

TEST_CASE("THAT2180 tracks both control pins without rebuilding the signal path",
          "[vca][that2180][engine]") {
    constexpr double inputOhms = 25.0;
    constexpr double feedOhms = 20000.0;
    const double signalVolts = 0.015;
    const double referenceVolts = 0.25;
    for (const double load : {1000.0, 20000.0}) {
        wdf::Circuit circuit;
        circuit.addSrc("signal", "0", signalVolts + referenceVolts);
        circuit.addSrc("ecPlus", "0", 0.0305);
        circuit.addSrc("ecMinus", "0", 0.0305);
        circuit.addSrc("reference", "0", referenceVolts);
        circuit.addRes("signal", "in", feedOhms);
        circuit.addThat2180("in", "out", "ecPlus", "ecMinus", "reference", inputOhms, "IC1");
        circuit.addRes("out", "reference", load);

        wdf::Engine engine(circuit, 48000.0);
        const auto out = engine.nodeIndex("out");
        REQUIRE(out >= 0);
        const auto rebuilds = engine.linearRebuilds();
        struct GainCase { double plus, minus, decibels; };
        for (const auto& [plus, minus, decibels] : {
                 GainCase{.0305, .0305, 0}, {.061, 0, 10}, {0, .061, -10},
                 {.0915, .0305, 10}, {0, .366, -60}, {.183, 0, 30},
                 {.8, 0, 40}, {0, .8, -110}}) {
            const std::vector<double> sources{signalVolts + referenceVolts, plus, minus,
                                              referenceVolts};
            engine.step(sources);
            CHECK(engine.lastConverged());
            const double expected = signalVolts / (feedOhms + inputOhms) *
                                    std::pow(10.0, decibels / 20.0);
            const double actual = (referenceVolts - engine.step(sources)[out]) / load;
            CHECK(actual == Approx(expected).epsilon(0.001).margin(1e-12));
            CHECK(engine.lastConverged());
        }
        CHECK(engine.stateRejectCount() == 0);
        CHECK(engine.linearRebuilds() == rebuilds);
    }
}
