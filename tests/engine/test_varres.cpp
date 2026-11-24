#include <catch2.hpp>

#include <cmath>
#include <string>
#include <vector>

#include <math.h>

#include "format/PortImpedance.h"
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

TEST_CASE("Co-illuminated optical cells preserve independent attack and release across mode changes",
          "[engine][photores]") {
    wdf::Circuit c;
    c.addSrc("signal", "0", 1.0);
    c.addSrc("lamp", "0", 0.0);
    c.addRes("signal", "out", 12000.0);
    wdf::PhotoresistorControl cell;
    cell.sensePos = "lamp";
    cell.senseNeg = "0";
    cell.referenceVolts = 1.2;
    cell.gamma = 1.65;
    c.addPhotoresistor("out", "0", 1e7, cell, "fast");
    cell.attackSeconds = 0.32;
    cell.releaseSeconds = 2.0;
    cell.brightOhms = 2700.0;
    c.addPhotoresistor("out", "0", 1e7, cell, "slow");
    cell.gamma = 2.75;
    c.addPhotoresistor("out", "0", 2e6, cell, "different response");

    for (const double rate : {48000.0, 96000.0}) {
        wdf::Engine exact(c, rate), preview(c, rate);
        REQUIRE(preview.hasCompiledOptical());
        auto drive = exact.dcValues();
        int output = exact.nodeIndex("out");
        double maxAudioError = 0.0, maxOhmsError = 0.0;
        for (int i = 0; i < 2048; ++i) {
            preview.useCompiledOptical((i / 353) % 2 == 0);
            drive[1] = i < 1200 ? 0.8 + 0.7 * std::sin(i * 0.07) : -0.25;
            maxAudioError = std::max(maxAudioError, std::abs(exact.step(drive)[output] -
                                                             preview.step(drive)[output]));
            for (size_t k = 0; k < c.ctrlRes.size(); ++k)
                maxOhmsError = std::max(maxOhmsError, std::abs(exact.controlledResistanceValues()[k] -
                                                               preview.controlledResistanceValues()[k]));
        }
        CHECK(maxAudioError < 1e-12);
        CHECK(maxOhmsError < 1e-8);
    }
}

TEST_CASE("The loaded junction solve follows a feedback-driven optical resistance", "[engine][photores]") {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addRes("in", "out", 4700.0);
    c.addDiode("out", "0", wdf::DiodeModel{4.35e-9, 1.906, 0.6458}, "clip");
    wdf::PhotoresistorControl cell;
    cell.sensePos = "out";
    cell.senseNeg = "0";
    cell.referenceVolts = 0.25;
    cell.gamma = 1.65;
    cell.brightOhms = 250.0;
    cell.attackSeconds = 0.001;
    cell.releaseSeconds = 0.010;
    c.addPhotoresistor("out", "0", 1e6, cell, "feedback");

    for (double fs : {48000.0, 96000.0}) {
        wdf::Engine native(c, fs), preview(c, fs);
        REQUIRE(preview.prepareLoadedJunctionPreview());
        preview.useLoadedJunctionPreview(true);
        auto source = native.dcValues();
        const int out = native.nodeIndex("out");
        double maxVoltageError = 0.0, maxResistanceError = 0.0, minResistance = 1e6;
        for (int i = 0; i < 3072; ++i) {
            source[0] = i < 1536 ? 1.6 * std::sin(i * 0.035) : 0.0;
            const double reference = native.step(source)[out];
            const double candidate = preview.step(source)[out];
            const double actualR = preview.controlledResistanceValues().front();
            const double expectedR = native.controlledResistanceValues().front();
            maxVoltageError = std::max(maxVoltageError, std::abs(candidate - reference));
            maxResistanceError = std::max(maxResistanceError, std::abs(actualR / expectedR - 1.0));
            minResistance = std::min(minResistance, actualR);
        }
        CHECK(preview.loadedJunctionHits() > 3000);
        CHECK(minResistance < 2000.0);
        CHECK(maxVoltageError < 1e-4);
        CHECK(maxResistanceError < 1e-3);
    }
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

TEST_CASE("prepared controls match rebuilt transfers without resetting waves", "[varres][engine][controls]") {
    wdf::Circuit circuit;
    circuit.addSrc("in", "0", 0.1);
    circuit.addPot("in", "wiper", "0", 100e3, .37, wdf::PotTaper::Log, "Gain");
    circuit.addVarRes("wiper", "out", 22e3, .71, wdf::PotTaper::RevLog, "Tone");
    circuit.addRes("out", "0", 33e3, "Load");
    circuit.addCap("out", "0", 22e-9);
    circuit.addDiode("out", "0", wdf::DiodeModel(1e-12, 1.5, 1.0));
    wdf::Engine reference(circuit, 48000.0), prepared(circuit, 48000.0);
    prepared.prepareLiveControls();
    const auto initialRebuilds = prepared.linearRebuilds();
    const int output = reference.nodeIndex("out");
    REQUIRE(output >= 0);
    double worst = 0.0;
    for (int sample = 0; sample < 1024; ++sample) {
        if (sample == 512) {
            REQUIRE(reference.setResistance("Load", 100e3));
            REQUIRE(prepared.setResistance("Load", 100e3));
        }
        const double ramp = static_cast<double>(sample % 256) / 255.0;
        const std::vector<std::pair<int, double>> pots{{0, ramp}};
        const std::vector<std::pair<int, double>> rheostats{{0, 1.0 - ramp}};
        REQUIRE(reference.setParametersByIndex(pots, rheostats, {}));
        REQUIRE(prepared.setLiveControls(pots, rheostats));
        const double input = .7 * std::sin(.081 * sample);
        const double expected = reference.step(&input, 1)[output];
        const double actual = prepared.step(&input, 1)[output];
        REQUIRE(std::isfinite(actual));
        REQUIRE_FALSE(prepared.lastStateRejected());
        worst = std::max(worst, std::abs(expected - actual));
    }
    REQUIRE(prepared.linearRebuilds() == initialRebuilds + 1);
    REQUIRE(worst < 1e-7);
}

TEST_CASE("live emitter resistor reconstructs the same BJT voltages as a rebuild", "[varres][engine][controls][bjt]") {
    wdf::Circuit c;
    c.addSrc("supply", "0", 9.0);
    c.addRes("supply", "base", 1e6);
    c.addRes("supply", "collector", 4700.0);
    c.addVarRes("emitter", "0", 2200.0, 0.5, wdf::PotTaper::Linear, "Re");
    const auto* transistor = wdf::findBjtByName("2N3904");
    REQUIRE(transistor);
    c.addBjt("collector", "base", "emitter", transistor->model, "Q1");

    wdf::Engine live(c, 48000.0), rebuilt(c, 48000.0);
    live.prepareLiveControls();
    REQUIRE(live.setLiveControls({}, {{0, 0.75}}));
    REQUIRE(rebuilt.setVarResPosition(0, 0.75));
    const auto& expected = rebuilt.step(rebuilt.dcValues());
    const auto& actual = live.step(live.dcValues());
    REQUIRE_FALSE(live.lastStateRejected());
    REQUIRE_FALSE(rebuilt.lastStateRejected());
    for (const auto* name : {"collector", "emitter"}) {
        const auto n = live.nodeIndex(name);
        REQUIRE(n >= 0);
        REQUIRE(expected[n] > 0.1);
        CHECK(actual[n] == Approx(expected[n]).margin(1e-7));
    }
}

TEST_CASE("settled control transfer keeps running state and rejects stale work", "[varres][engine][controls]") {
    wdf::Circuit circuit;
    circuit.addSrc("in", "0", .1);
    circuit.addPot("in", "out", "0", 100e3, .37, wdf::PotTaper::Log, "Gain");
    circuit.addCap("out", "0", 22e-9);
    circuit.addDiode("out", "0", wdf::DiodeModel(1e-12, 1.5, 1.0));
    wdf::Engine live(circuit, 48000), reference(circuit, 48000);
    live.prepareLiveControls();
    reference.prepareLiveControls();
    REQUIRE(live.setLiveControls({{0, .42}}, {}));
    wdf::Engine::LiveTransferSetup stale(circuit);
    REQUIRE(live.captureLiveTransferSetup(stale));
    stale.build(0, false);

    REQUIRE(live.setLiveControls({{0, .61}}, {}));
    REQUIRE_FALSE(live.installLiveTransferSetup(stale));
    REQUIRE(live.hasAdjustedLiveTransfer());
    REQUIRE(reference.setLiveControls({{0, .61}}, {}));
    wdf::Engine::LiveTransferSetup settled(circuit);
    REQUIRE(live.captureLiveTransferSetup(settled));
    settled.build(0, false);
    REQUIRE_FALSE(reference.installLiveTransferSetup(settled));
    REQUIRE(reference.rebuild());
    const int output = live.nodeIndex("out");
    REQUIRE(output >= 0);
    for (int i = 0; i < 256; ++i) {
        const double input = .8 * std::sin(i * .13);
        if (i == 128) {
            const auto before = live.capacitorStates();
            REQUIRE(live.installLiveTransferSetup(settled));
            CHECK_FALSE(live.hasAdjustedLiveTransfer());
            CHECK(live.capacitorStates() == before);
        }
        const auto expected = reference.step(&input, 1)[output];
        CHECK(live.step(&input, 1)[output] == Approx(expected).margin(1e-7));
    }
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

TEST_CASE("Port impedance stamps series and shunt fixtures without duplicating an existing termination",
          "[impedance][engine]") {
    wdf::LiveWDFDocument doc;
    wdf::SchematicComponent in, out, series, load;
    in.id = "IN"; in.type = "in"; in.pins = {{"sig", "signal"}};
    out.id = "OUT"; out.type = "out"; out.pins = {{"sig", "output"}};
    in.values = out.values = {{"externalMinOhms", 100}, {"externalMaxOhms", 1000}};
    series.id = "RS"; series.type = "res"; series.pins = {{"a","signal"}, {"b","output"}};
    series.values = {{"value", 100}};
    load.id = "RL"; load.type = "res"; load.pins = {{"a","0"}, {"b","output"}};
    load.values = {{"value", 1000}};
    doc.components = {in, out, series, load};
    auto& source = doc.components[0];
    auto& output = doc.components[1];
    SECTION("Existing source and load have their actual values replaced; live changes preserve the transfer") {
        source.properties["externalResistor"] = "RS";
        output.properties["externalResistor"] = "RL";
        auto circuit = doc.toCircuit();
        CHECK(wdf::applyPortImpedance(circuit, doc, true, 0.).empty());
        REQUIRE(circuit.varRes.empty());
        REQUIRE(wdf::applyPortImpedance(circuit, doc, true, 1.) == "RS");
        REQUIRE(wdf::applyPortImpedance(circuit, doc, false, .001) == "RL");
        REQUIRE(circuit.res.size() == 2);
        wdf::Engine engine(circuit, 48000.);
        const int index = engine.nodeIndex("output");
        REQUIRE(index >= 0);
        CHECK(engine.step({1.})[index] == Approx(100. / 1100.));
        engine.prepareLiveControls();
        const auto rebuilds = engine.linearRebuilds();
        REQUIRE(engine.setLiveControls({}, {{engine.varResIndexByName("RS"), .1},
                                             {engine.varResIndexByName("RL"), 1.}}));
        CHECK(engine.step({1.})[index] == Approx(1000. / 1100.));
        CHECK(engine.linearRebuilds() == rebuilds);
    }
    SECTION("An added source and added load form the expected series and parallel network") {
        auto circuit = doc.toCircuit();
        circuit.addVarRes("output", "0", 1000., 1., wdf::PotTaper::Linear, "$external_source");
        circuit.addRes("$external_source__drive", "0", 1000.);
        const auto sourceName = wdf::applyPortImpedance(circuit, doc, true, .001);
        REQUIRE(sourceName == "$external_source_");
        CHECK(circuit.src.front().a != "$external_source__drive");
        REQUIRE_FALSE(wdf::applyPortImpedance(circuit, doc, false, 1.).empty());
        wdf::Engine engine(circuit, 48000.);
        const int index = engine.nodeIndex("output");
        REQUIRE(index >= 0);
        const double parallel = 1000. / 3.;
        CHECK(engine.step({1.})[index] == Approx(parallel / (100. + 100. + parallel)));
    }
    SECTION("A stale reference, internal shunt on the driven pin, or loaded source branch is rejected") {
        source.properties["externalResistor"] = "missing";
        CHECK_FALSE(wdf::portImpedanceRange(doc, true));
        output.properties["externalResistor"] = "RS";
        CHECK_FALSE(wdf::portImpedanceRange(doc, false));
        source.properties["externalResistor"] = "RS";
        load.id = "Rtap"; load.pins["b"] = "signal";
        doc.components.push_back(load);
        CHECK_FALSE(wdf::portImpedanceRange(doc, true));
    }
}

TEST_CASE("Settled bipolar preview continues a moving control through a live transient", "[engine][controls][preview]") {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.);
    c.addSrc("supply", "0", 9.);
    c.addRes("in", "base", 330000.);
    c.addRes("supply", "base", 22000.);
    c.addRes("base", "0", 4700.);
    c.addRes("supply", "collector", 3300.);
    c.addRes("emitter", "0", 820.);
    c.addBjt("collector", "base", "emitter", wdf::findBjtByName("2N3904")->model, "Q1");
    c.addCap("collector", "level", 1.e-6);
    c.addPot("level", "wiper", "0", 10000., .5, wdf::PotTaper::Linear, "Level");
    c.addInd("wiper", "out", 10.e-3);
    c.addRes("out", "0", 4700.);
    wdf::Engine updated(c, 48000.), uninterrupted(c, 48000.), physical(c, 48000.);
    const int out = updated.nodeIndex("out");
    for (auto* e : {&updated, &uninterrupted, &physical})
        e->prepareLiveControls();
    for (auto* e : {&updated, &uninterrupted})
        REQUIRE(e->prepareBipolarPreview(0, out));

    wdf::Engine::LiveTransferSetup settled(c);
    auto sources = physical.dcValues();
    double atChange = 0.;
    for (int n = 0; n < 320; ++n) {
        if (n == 64) {
            for (auto* e : {&updated, &uninterrupted, &physical})
                REQUIRE(e->setLiveControls({{0, .65}}, {}));
            REQUIRE(updated.captureLiveTransferSetup(settled));
            settled.build(0, false);
            settled.prepareCompiledPreview(0, out, true, 1.2, false);
        }
        if (n == 192) {
            REQUIRE(updated.bipolarPreviewActive());
            REQUIRE(updated.installLiveTransferSetup(settled));
        }

        // The input changes on the first sample processed by the new coefficients.
        sources[0] = .12 * std::sin(n * .063) + (n >= 192 ? .18 : 0.);
        double actual, prior;
        REQUIRE(updated.stepBipolarPreview(sources[0], actual));
        REQUIRE(uninterrupted.stepBipolarPreview(sources[0], prior));
        const double reference = physical.step(sources)[out];
        REQUIRE(actual == Approx(prior).margin(1e-7));
        REQUIRE(actual == Approx(reference).margin(2e-5));
        if (n == 191)
            atChange = actual;
        if (n == 192)
            CHECK(std::abs(actual - atChange) > 1e-4);
    }
}
