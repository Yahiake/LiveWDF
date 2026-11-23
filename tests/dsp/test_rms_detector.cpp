#include <catch2.hpp>

#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "compiler/Compiler.h"
#include "devices/RmsDetector.h"
#include "format/Document.h"
#include "format/PartLibrary.h"
#include "spice/SpiceExport.h"
#include "wdf/Engine.h"

TEST_CASE("Log RMS detector tracks power and calibrates a sine to RMS volts", "[rms][dsp]") {
    wdf::RmsDetectorModel model;
    model.referenceVolts = 0.775;

    for (const double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        CAPTURE(fs);
        wdf::RmsDetectorState state(model, 1.0 / fs);
        const auto db = [&] { return state.outputCurrent() * model.outputOhms / model.voltsPerDb; };

        state.reset(0.0);
        REQUIRE(db() == Approx(-100.0));
        for (int i = 0; i < static_cast<int>(0.3 * fs); ++i)
            state.step(model.referenceVolts * std::sqrt(2.0) *
                       std::sin(2.0 * wdf::kPi * 1000.0 * i / fs));
        REQUIRE(db() == Approx(0.0).margin(0.025));

        state.reset(model.referenceVolts);
        const int stepFrames = static_cast<int>(0.01 * fs);
        for (int i = 0; i < stepFrames; ++i)
            state.step(2.0 * model.referenceVolts);
        const double stepTime = stepFrames / fs;
        const double power = 4.0 - 3.0 * std::exp(-stepTime / model.timeConstantSeconds);
        REQUIRE(db() == Approx(10.0 * std::log10(power)).margin(1.0e-10));

        state.reset(model.referenceVolts);
        for (int i = 0; i < stepFrames; ++i)
            state.step(0.0);
        REQUIRE(db() == Approx(-10.0 / std::log(10.0) * stepTime / model.timeConstantSeconds)
                            .margin(1.0e-10));
    }
}

TEST_CASE("Engine RMS detector senses a differential port and seeds loaded DC", "[rms][engine]") {
    wdf::RmsDetectorModel model;
    wdf::Circuit circuit;
    circuit.addSrc("input", "common", 2.0);
    circuit.addSrc("common", "0", 5.0);
    circuit.addIsrc("0", "output", 50.0e-6, 0.0, {}, "ExplicitBias");
    circuit.addRmsDetector("input", "output", "common", model, "Level");
    const double load = 1000.0;
    circuit.addRes("output", "common", load);
    wdf::Engine engine(circuit, 48000.0);
    const int output = engine.nodeIndex("output");
    REQUIRE(output >= 0);

    const auto expectedAtTwoVolts = 5.0 +
        (20.0 * std::log10(2.0) * model.voltsPerDb + 50.0e-6 * model.outputOhms) *
        load / (load + model.outputOhms);
    std::vector<double> dc, roots;
    REQUIRE(engine.solveDc(dc, roots));
    REQUIRE(dc[output] == Approx(expectedAtTwoVolts).margin(1e-9));
    for (int pass = 0; pass < 2; ++pass) {
        engine.reset();
        const std::array<double, 3> sources = {1.0, 5.0, 50.0e-6};
        REQUIRE(engine.step(sources.data(), sources.size())[output] ==
                Approx(expectedAtTwoVolts).margin(1e-9));
        REQUIRE(engine.step(sources.data(), sources.size())[output] < expectedAtTwoVolts);
        REQUIRE(engine.stateRejectCount() == 0);
    }

    const auto deck = wdf::spiceExport(circuit, 48000.0, 0.05);
    REQUIRE(deck.ok);
    REQUIRE(deck.diagnostics.find("log RMS detector(s) with continuous SPICE averaging") !=
            std::string::npos);
    REQUIRE(deck.netlist.find("I=min(abs(V(input,common))") != std::string::npos);
}

TEST_CASE("Log RMS detector can use independent attack and release", "[rms][dsp]") {
    wdf::RmsDetectorModel model;
    model.attackTimeConstantSeconds = 0.01;
    model.releaseTimeConstantSeconds = 0.1;
    constexpr double fs = 48000.0;
    wdf::RmsDetectorState state(model, 1.0 / fs);
    state.reset(0.0);

    for (int i = 0; i < 480; ++i)
        state.step(1.0);
    REQUIRE(state.outputCurrent() ==
            Approx(model.current(1.0 - std::exp(-1.0))).margin(1e-10));

    for (int i = 0; i < 4800; ++i)
        state.step(0.0);
    REQUIRE(state.outputCurrent() ==
            Approx(model.current((1.0 - std::exp(-1.0)) * std::exp(-1.0))).margin(1e-10));

    wdf::Circuit circuit;
    circuit.addSrc("input", "0");
    circuit.addRmsDetector("input", "output", "0", model);
    const auto original = livewdf::compiler::Compiler{}.compile(circuit);
    REQUIRE(original.ok());
    for (auto field : {&wdf::RmsDetectorModel::attackTimeConstantSeconds,
                       &wdf::RmsDetectorModel::releaseTimeConstantSeconds}) {
        auto changed = circuit;
        changed.rmsDetectors.front().model.*field *= 2.0;
        const auto result = livewdf::compiler::Compiler{}.compile(changed);
        REQUIRE(result.ok());
        REQUIRE(result.circuit->revision() != original.circuit->revision());
    }
    const auto deck = wdf::spiceExport(circuit, fs);
    REQUIRE(deck.ok);
    REQUIRE(deck.netlist.find("ternary_fcn") != std::string::npos);
}

TEST_CASE("Format exposes a log RMS detector with valid defaults and validates overrides",
          "[rms][format]") {
    auto rms = livewdf::editor::makeComponent("rmsdet", {}, 0, 0);
    rms.id = "RMS1";
    rms.pins = {{"in","in"},{"out","out"},{"common","0"}};
    wdf::LiveWDFDocument doc;
    doc.components.push_back(rms);
    auto input = livewdf::editor::makeComponent("in", doc.components, -10, 0);
    input.pins["sig"] = "in";
    doc.components.push_back(input);
    auto output = livewdf::editor::makeComponent("out", doc.components, 10, 0);
    output.pins["sig"] = "out";
    doc.components.push_back(output);
    REQUIRE(doc.toCircuit().rmsDetectors.size() == 1);
    auto& part = doc.components.front();
    part.values["attackTimeConstantSeconds"] = 0.004;
    REQUIRE(doc.toCircuit().rmsDetectors.front().model.attackSeconds() == 0.004);
    part.values["releaseTimeConstantSeconds"] = -1.0;
    REQUIRE_THROWS_AS(doc.toCircuit(), std::invalid_argument);
    part.values["releaseTimeConstantSeconds"] = 0.0;
    part.values["timeConstantSeconds"] = -1.0;
    REQUIRE_THROWS_AS(doc.toCircuit(), std::invalid_argument);
}

TEST_CASE("Compiler revision covers the log RMS detector's reference and time constant",
          "[rms][compiler]") {
    wdf::Circuit circuit;
    circuit.addSrc("input", "0");
    circuit.addRmsDetector("input", "output", "0", {});
    const auto original = livewdf::compiler::Compiler{}.compile(circuit);
    REQUIRE(original.ok());
    for (auto field : {&wdf::RmsDetectorModel::referenceVolts,
                       &wdf::RmsDetectorModel::timeConstantSeconds}) {
        auto changed = circuit;
        changed.rmsDetectors.front().model.*field *= 2.0;
        const auto result = livewdf::compiler::Compiler{}.compile(changed);
        REQUIRE(result.ok());
        REQUIRE(result.circuit->revision() != original.circuit->revision());
    }
}

TEST_CASE("dbx 209 calibrated current port reproduces negative log slope and 160 C15 release",
          "[dbx209][rms][parts-validation]") {
    constexpr double fs = 48000.0;
    for (double capacitance : {22e-6, 44e-6}) {
        const auto detector = wdf::RmsDetectorModel::dbx209(23.34e-6, capacitance);
        REQUIRE(detector.valid());
        wdf::RmsDetectorState state(detector, 1.0 / fs);
        state.reset(detector.referenceVolts * 10.0);
        const auto output = [&]() { return state.outputCurrent() * detector.outputOhms; };
        CHECK(output() == Approx(-0.120).margin(1e-9));
        const double start = output();
        for (int i = 0; i < 4800; ++i) state.step(0.0);
        CHECK((output() - start) / .006 == Approx(12.0 * 22e-6 / capacitance).margin(1e-6));
    }
    CHECK_FALSE(wdf::RmsDetectorModel::dbx209(0.0, 22e-6).valid());
    CHECK_FALSE(wdf::RmsDetectorModel::dbx209(23.34e-6, -1.0).valid());
}
