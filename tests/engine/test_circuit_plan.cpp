#include <catch2.hpp>
#include "format/LiveWDFFile.h"
#include "format/ControlGroups.h"
#include "format/PortImpedance.h"
#include <cmath>

#include "wdf/Engine.h"

TEST_CASE("Packed Preview drive follows the sparse drive through factory knob turns",
          "[engine][circuit-plan][packed-drive]") {
    struct Engine : wdf::Engine {
        using wdf::Engine::Engine;
        void disablePackedDrive() { packedDrive_ = {}; }
        void useNodeTransfer() { affineNodes_ = {}; }
        bool hasScatteredShortPanel() const {
            return std::ranges::any_of(packedDrive_.panels, [&](const auto& panel) {
                const auto* nodes = packedDrive_.nodes.data() + panel.rowStart;
                return panel.width == wdf::detail::smallDrivePanelWidth &&
                       nodes[panel.width - 1] != nodes[0] + panel.width - 1;
            });
        }
    };
    const bool scattered = GENERATE(false, true);
    const auto file = scattered ? "schematics/amps/EVH 5150 III 100S Stealth - Channel 3.livewdf"
                                : "schematics/studio/eq/SSL 4000 E 82E242 Black EQ.livewdf";
    wdf::LiveWDFDocument document;
    std::string error;
    REQUIRE(document.loadFromFile(file, error));
    if (scattered) {
        wdf::LiveWDFDocument preamp;
        REQUIRE(document.makePreampDocument(preamp, error));
        document = std::move(preamp);
    }
    const auto circuit = wdf::circuitWithPortImpedances(document);
    const auto source = std::ranges::find(circuit.src, document.inputId(), &wdf::Src::name);
    REQUIRE(source != circuit.src.end());
    const size_t input = source - circuit.src.begin();
    Engine preview(circuit, 48000.);
    const int output = preview.nodeIndex(document.outputNet());
    preview.prepareLiveControls();
    preview.prepareCircuitPlan();
    preview.prepareCompiledPreview(input, output, false, document.previewPeakVolts(), false);
    preview.useLoadedJunctionPreview(true);
    preview.useCircuitPlan(true);
    preview.useNodeTransfer();
    REQUIRE(preview.hasPackedDrivePlan());
    if (scattered)
        REQUIRE(preview.hasScatteredShortPanel());
    REQUIRE_FALSE(preview.hasLinearDrivePlan());
    Engine reference = preview;
    reference.disablePackedDrive();
    auto sources = preview.dcValues();

    wdf::Engine::LiveTransferSetup stale(circuit);
    for (int sample = 0; sample < 256; ++sample) {
        CAPTURE(sample);
        if (sample >= 128 && sample % 8 == 0) {
            const std::vector<std::pair<int, double>> knob{{0, .35 + .25 * std::abs(std::sin(.05 * sample))}};
            REQUIRE(preview.setLiveControls(knob, {}));
            REQUIRE(reference.setLiveControls(knob, {}));
            REQUIRE(preview.hasAdjustedLiveTransfer());
            REQUIRE(preview.hasPackedDrivePlan());
            if (sample == 128) {
                REQUIRE(preview.captureLiveTransferSetup(stale, false));
                stale.build(input, false);
                REQUIRE(preview.canInstallLiveTransferSetup(stale));
            } else {
                REQUIRE_FALSE(preview.canInstallLiveTransferSetup(stale));
            }
        }
        sources[input] = document.previewPeakVolts() * .4 * std::sin(.13 * sample);
        REQUIRE(preview.step(sources, 60, wdf::Engine::kFastLiveTolerance) ==
                reference.step(sources, 60, wdf::Engine::kFastLiveTolerance));
        REQUIRE_FALSE(preview.lastStateRejected());
    }

    wdf::Engine::LiveTransferSetup settled(circuit);
    REQUIRE(preview.captureLiveTransferSetup(settled, false));
    settled.build(input, false);
    settled.prepareCompiledPreview(input, output, false, document.previewPeakVolts(), false);
    const auto waves = preview.capacitorStates();
    REQUIRE(preview.installLiveTransferSetup(settled));
    REQUIRE(preview.capacitorStates() == waves);
    REQUIRE(preview.hasPackedDrivePlan());
    preview.useNodeTransfer();
    reference = preview;
    reference.disablePackedDrive();
    for (int sample = 0; sample < 128; ++sample) {
        sources[input] = document.previewPeakVolts() * .4 * std::sin(.17 * sample);
        REQUIRE(preview.step(sources, 60, wdf::Engine::kFastLiveTolerance) ==
                reference.step(sources, 60, wdf::Engine::kFastLiveTolerance));
        REQUIRE_FALSE(preview.lastStateRejected());
    }
    REQUIRE(preview.setPotPosition(0, .8));
    REQUIRE_FALSE(preview.hasPackedDrivePlan());
}

TEST_CASE("Compiled RLC drive preserves reactive history, sources and transfer edits", "[engine][circuit-plan]") {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.2);
    c.addRes("in", "tap", 470, "feed");
    c.addCap("tap", "in", 22e-9);
    c.addInd("tap", "out", 18e-3);
    c.addRes("out", "0", 3300);
    c.addIsrc("out", "0", 3e-6);

    for (const double rate : {44100.0, 48000.0, 96000.0}) {
        CAPTURE(rate);
        wdf::Engine compiled(c, rate), reference(c, rate);
        compiled.prepareCircuitPlan();
        REQUIRE(compiled.hasLinearDrivePlan());
        for (int n = 0; n < 512; ++n) {
            if (n == 127) {
                REQUIRE(compiled.setResistance("feed", 870));
                REQUIRE(reference.setResistance("feed", 870));
                REQUIRE_FALSE(compiled.hasLinearDrivePlan());
            }
            if (n == 191) {
                compiled.prepareCircuitPlan();
                REQUIRE(compiled.hasLinearDrivePlan());
            }
            compiled.useCircuitPlan((n / 64) % 2 == 0);
            const double inputs[] = {0.2 + 0.4 * std::sin(0.029 * n), 8e-5 * std::cos(0.013 * n)};
            // Exercise both a missing voltage input and a missing current input with DC fallback.
            const size_t count = n % 17 == 0 ? 0 : n % 3 == 0 ? 1 : 2;
            const auto& expected = reference.step(inputs, count);
            const auto& actual = compiled.step(inputs, count);
            for (size_t i = 0; i < actual.size(); ++i)
                REQUIRE(actual[i] == Approx(expected[i]).margin(1e-11));
            REQUIRE(compiled.getCapacitorState(0) == Approx(reference.getCapacitorState(0)).margin(1e-11));
            REQUIRE(compiled.getInductorState(0) == Approx(reference.getInductorState(0)).margin(1e-11));
        }
    }
}

TEST_CASE("Compiled loaded BJT predictor retains the full law through clipping and recovery", "[engine][circuit-plan]") {
    wdf::Circuit c;
    c.addSrc("vcc", "0", 5.0);
    c.addSrc("in", "0", 0.65);
    c.addRes("in", "b", 4700);
    c.addRes("vcc", "c", 3300);
    c.addRes("e", "0", 220);
    c.addBjt("c", "b", "e", wdf::findBjtByName("2N3904-MiSTer-IKF")->model, "Q1");
    c.addDiode("b", "c", wdf::DiodeModel(2.1e-12, 1.8, 2.0), "D1");
    wdf::Engine compiled(c, 48000.0), reference(c, 48000.0);
    compiled.prepareCircuitPlan();
    compiled.useCircuitPlan(true);
    constexpr double tolerance = 1e-7;
    long long hitsBeforeRecovery = 0;
    for (int n = 0; n < 1024; ++n) {
        const double amplitude = n >= 256 && n < 768 ? 1.5 : 1e-5;
        const double inputs[] = {5., 0.65 + amplitude * std::sin(n * 0.07)};
        const auto hits = compiled.circuitPlanHits();
        const auto& expected = reference.step(inputs, 2, 60, 1e-9);
        const auto& actual = compiled.step(inputs, 2, 60, tolerance);
        REQUIRE_FALSE(reference.lastStateRejected());
        REQUIRE_FALSE(compiled.lastStateRejected());
        for (size_t i = 0; i < actual.size(); ++i)
            REQUIRE(actual[i] == Approx(expected[i]).margin(2e-6));
        if (compiled.circuitPlanHits() != hits)
            REQUIRE(compiled.lastResidual() <= tolerance);
        if (n == 255)
            REQUIRE(compiled.circuitPlanHits() > 0);
        if (n == 767)
            hitsBeforeRecovery = compiled.circuitPlanHits();
    }
    REQUIRE(compiled.circuitPlanHits() > hitsBeforeRecovery);
    REQUIRE(compiled.circuitPlanAttempts() > compiled.circuitPlanHits());
    const auto attempts = compiled.circuitPlanAttempts();
    compiled.useCircuitPlan(false);
    compiled.step(compiled.dcValues());
    REQUIRE(compiled.circuitPlanAttempts() == attempts);
}

TEST_CASE("Screened worker plans follow switch revisions without replacing live state", "[engine][circuit-plan]") {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addRes("in", "out", 1000, "feed");
    c.addRes("out", "0", 10000);
    c.addCap("out", "0", 10e-9);
    wdf::Engine live(c, 48000), peer(c, 48000);
    wdf::Engine::LiveTransferSetup stale(c), updated(c), different(c);
    REQUIRE(live.captureLiveTransferSetup(stale, true));
    stale.build(0, false);
    REQUIRE(live.setResistance("feed", 2000));
    REQUIRE_FALSE(live.installLiveTransferSetup(stale));

    REQUIRE(live.captureLiveTransferSetup(updated, true));
    REQUIRE(peer.captureLiveTransferSetup(different, true));
    updated.build(0, false);
    different.build(0, false);
    // Different control positions cannot be used as a paired timing comparison.
    REQUIRE_FALSE(updated.screenCircuitPlan(different, 0, live.nodeIndex("out")));
    const double one = 1.0;
    live.step(&one, 1);
    const auto states = live.capacitorStates();
    const auto revision = live.linearRebuilds();
    REQUIRE(live.installLiveTransferSetup(updated));
    REQUIRE(live.circuitPlanPrepared());
    REQUIRE_FALSE(live.hasLinearDrivePlan());
    REQUIRE(live.capacitorStates() == states);
    REQUIRE(live.linearRebuilds() == revision);
    REQUIRE_FALSE(live.installLiveTransferSetup(updated));
}

TEST_CASE("Prepared op-amp roots follow physical rails and pole memory across solver changes",
          "[engine][circuit-plan][opamp]") {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addRes("in", "inv1", 10000);
    c.addRes("out1", "inv1", 33000);
    c.addRes("out1", "inv2", 10000);
    c.addRes("out2", "inv2", 22000);
    c.addRes("out2", "inv1", 330000);
    c.addCap("out2", "inv2", 100e-12);
    c.addRes("out2", "0", 10000);
    auto model = wdf::OpAmpModel::fromType(wdf::OpAmpType::Tl072);
    model.setVRails(5.0, 5.0);
    c.addOpAmp("out1", "inv1", "0", "0", "0", model, "U1");
    c.addOpAmp("out2", "inv2", "0", "0", "0", model, "U2");

    wdf::Engine compiled(c, 48000), reference(c, 48000);
    compiled.prepareCircuitPlan();
    REQUIRE(compiled.hasAffineRootPlan());
    compiled.useCircuitPlan(true);
    const auto out = compiled.nodeIndex("out2");
    double peak = 0.0;
    long long hitsBeforeRecovery = 0, attemptsWhileDisabled = 0;
    for (int n = 0; n < 2048; ++n) {
        if (n == 1024 || n == 1536) {
            reference.setAllOpAmpSoftRails(n == 1024);
            compiled.setAllOpAmpSoftRails(n == 1024);
        }
        if (n == 1280 || n == 1408)
            compiled.useCircuitPlan(n == 1408);
        const double amplitude = (n >= 512 && n < 1024) ? 1.6 : 0.02;
        const double input = amplitude * std::sin(0.026 * n);
        const auto& expected = reference.step(&input, 1);
        const auto& actual = compiled.step(&input, 1, 60, wdf::Engine::kFastLiveTolerance);
        REQUIRE_FALSE(reference.lastStateRejected());
        REQUIRE_FALSE(compiled.lastStateRejected());
        for (size_t node = 0; node < actual.size(); ++node)
            REQUIRE(actual[node] == Approx(expected[node]).margin(2e-6));
        peak = std::max(peak, std::abs(expected[out]));
        if (n == 511)
            REQUIRE(compiled.affineRootHits() > 0);
        if (n == 1023) {
            hitsBeforeRecovery = compiled.affineRootHits();
            REQUIRE(compiled.affineRootAttempts() > hitsBeforeRecovery);
        }
        if (n == 1279)
            attemptsWhileDisabled = compiled.affineRootAttempts();
        if (n == 1407)
            REQUIRE(compiled.affineRootAttempts() == attemptsWhileDisabled);
    }
    REQUIRE(peak > 4.9);
    REQUIRE(compiled.affineRootHits() > hitsBeforeRecovery);
}

TEST_CASE("Enabling a circuit plan preserves hysteresis through positive feedback",
          "[engine][circuit-plan][opamp]") {
    const bool coupled = GENERATE(false, true);
    for (const double initial : {-1.0, 1.0}) {
        CAPTURE(initial, coupled);
        wdf::Circuit c;
        c.addSrc("in", "0", initial);
        c.addRes("positive", "0", 1000);
        c.addRes("out", "0", 10000);
        auto model = wdf::OpAmpModel::fromType(wdf::OpAmpType::Tl072);
        model.setVRails(5.0, 5.0);
        if (!coupled) {
            c.addRes("out", "positive", 19000);
        } else {
            c.addRes("out2", "positive", 4000);
            c.addRes("out", "positive2", 4000);
            c.addRes("positive2", "0", 1000);
            c.addRes("out2", "0", 10000);
            c.addOpAmp("out2", "0", "positive2", "0", "0", model, "U2");
        }
        c.addOpAmp("out", "in", "positive", "0", "0", model, "U1");
        wdf::Engine candidate(c, 48000), reference(c, 48000);
        const int out = reference.nodeIndex("out");
        candidate.prepareCircuitPlan();
        REQUIRE_FALSE(candidate.prepareAffineNodePreview(0, out));
        for (int n = 0; n < 512; ++n) {
            candidate.useCircuitPlan(n >= 128);
            double input = 0.0;
            if (n < 32)
                input = initial;
            else if (n >= 256 && n < 288)
                input = -initial;
            const auto expected = reference.step(&input, 1, 60, wdf::Engine::kFastLiveTolerance)[out];
            const auto actual = candidate.step(&input, 1, 60, wdf::Engine::kFastLiveTolerance)[out];
            REQUIRE(actual == Approx(expected).margin(2e-6));
            if (n == 255)
                REQUIRE(actual * initial < -4.8);
            if (n == 511)
                REQUIRE(actual * initial > 4.8);
        }
    }
}

TEST_CASE("Loaded junction preview derives antiparallel devices from their ports and cards", "[engine][circuit-plan]") {
    wdf::Circuit c;
    c.addSrc("signal", "0", 0.0);
    c.addRes("signal", "junction", 2200);
    c.addCap("junction", "0", 8.2e-9);
    c.addDiode("junction", "0", wdf::DiodeModel(2e-12, 1.1, 4.0), "left");
    c.addDiode("0", "junction", wdf::DiodeModel(1e-9, 1.6, 12.0), "right");
    for (double fs : {44100.0, 96000.0}) {
        wdf::Engine preview(c, fs), exact(c, fs);
        REQUIRE(preview.prepareLoadedJunctionPreview(0, false));
        const int tap = exact.nodeIndex("junction");
        for (int i = 0; i < 512; ++i) {
            if (i == 32) {
                REQUIRE(preview.loadedJunctionAttempts() == 0);
                preview.useLoadedJunctionPreview(true);
            }
            const double src = 3.0 * std::sin(i * .08);
            const auto& a = exact.step(&src, 1, 60, 1e-9);
            const auto& b = preview.step(&src, 1, 60, 1e-9);
            REQUIRE(b[tap] == Approx(a[tap]).margin(3e-4));
        }
        REQUIRE(preview.loadedJunctionHits() == 480);
    }
}

TEST_CASE("Loaded diode solves high-current steps within the compact correction budget", "[engine][circuit-plan]") {
    wdf::Circuit circuit;
    const wdf::DiodeModel diode(1e-12, 1.4, 1.0);
    circuit.addSrc("in", "0", 0.0);
    circuit.addRes("in", "out", 5.0);
    circuit.addDiode("out", "0", diode, "junction");

    wdf::Engine preview(circuit, 48000.0);
    REQUIRE(preview.prepareLoadedJunctionPreview(0, false));
    preview.useLoadedJunctionPreview(true);
    const int output = preview.nodeIndex("out");
    for (const double input : {1.0, 2.0, -0.2, 4.0, 0.0, 1.5, 0.0}) {
        CAPTURE(input);
        const auto before = preview.loadedJunctionHits();
        const double voltage = preview.step(&input, 1)[output];
        REQUIRE(preview.loadedJunctionHits() == before + 1);
        double current, conductance;
        diode.eval(voltage, current, conductance);
        CHECK(std::abs((input - voltage) / 5.0 - current) < 2e-6);
    }
}

TEST_CASE("Loaded junction preview declines a transistor with extra transport terms", "[engine][circuit-plan]") {
    wdf::Circuit c;
    c.addSrc("supply", "0", 5.0);
    c.addRes("supply", "c", 2200);
    c.addRes("b", "supply", 220000);
    c.addRes("e", "0", 470);
    c.addBjt("c", "b", "e", wdf::findBjtByName("2N3904-MiSTer-IKF")->model, "device");
    c.addDiode("b", "c", wdf::DiodeModel(1e-12, 1.4, 12.0), "junction");
    wdf::Engine preview(c, 48000.0);
    REQUIRE_FALSE(preview.prepareLoadedJunctionPreview());
    REQUIRE_FALSE(preview.hasLoadedJunctionPreview());
}

TEST_CASE("Loaded junction Preview follows a transistor-only fuzz through a depth sweep",
          "[engine][circuit-plan][live-junction]") {
    wdf::LiveWDFDocument doc;
    std::string error;
    REQUIRE(doc.loadFromFile("schematics/pedals_fx/Mosrite Fuzzrite Silicon.livewdf", error));
    const auto circuit = doc.toCircuit();
    const auto source = std::ranges::find(circuit.src, doc.inputId(), &wdf::Src::name);
    REQUIRE(source != circuit.src.end());
    const size_t input = source - circuit.src.begin();
    wdf::Engine reference(circuit, 48000.), preview(circuit, 48000.);
    const int output = reference.nodeIndex(doc.outputNet());
    REQUIRE(output >= 0);
    REQUIRE(circuit.diode.empty());
    REQUIRE(preview.prepareLoadedJunctionPreview(input, false));
    preview.useLoadedJunctionPreview(true);
    reference.prepareLiveControls();
    preview.prepareLiveControls();
    const int depth = reference.potIndexByName("P1");
    REQUIRE(depth >= 0);

    auto drive = reference.dcValues();
    double signalEnergy = 0., errorEnergy = 0.;
    for (int i = 0; i < 1024; ++i) {
        if (i % 8 == 0) {
            const std::vector<std::pair<int, double>> knob{{depth, .2 + .55 * i / 1024.}};
            REQUIRE(reference.setLiveControls(knob, {}));
            REQUIRE(preview.setLiveControls(knob, {}));
        }
        drive[input] = .55 * (.7 * std::sin(.13 * i) + .3 * std::sin(.63 * i));
        const double expected = reference.step(drive)[output];
        const double actual = preview.step(drive, 60, wdf::Engine::kFastLiveTolerance)[output];
        REQUIRE_FALSE(preview.lastStateRejected());
        signalEnergy += expected * expected;
        errorEnergy += (actual - expected) * (actual - expected);
    }
    CHECK(preview.loadedJunctionHits() > 1000);
    CHECK(errorEnergy < 1e-5 * signalEnergy);
    for (size_t i = 0; i < circuit.cap.size(); ++i)
        CHECK(preview.getCapacitorState(i) == Approx(reference.getCapacitorState(i)).margin(.0005));
}

namespace {
wdf::Circuit bipolarPreviewStage() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.);
    c.addSrc("supply", "0", 9.);
    c.addRes("in", "base", 330000., "feed");
    c.addRes("supply", "base", 22000.);
    c.addRes("base", "0", 4700.);
    c.addRes("supply", "collector", 3300.);
    c.addRes("emitter", "0", 820.);
    c.addCap("collector", "0", 22.e-9);
    c.addBjt("collector", "base", "emitter", wdf::findBjtByName("2N3904")->model, "Q1");
    return c;
}
}

TEST_CASE("Stereo bipolar Preview qualifies matching snapshots once and keeps channel histories independent",
          "[engine][circuit-plan]") {
    auto c = bipolarPreviewStage();
    wdf::Engine left(c, 48000.), right(c, 48000.), reference(c, 48000.);
    wdf::Engine::LiveTransferSetup a(c);
    auto b = a.makePeer();
    bool shouldShare = true;
    SECTION("same snapshot") {}
    SECTION("different base circuit identity") {
        b = wdf::Engine::LiveTransferSetup(c);
        shouldShare = false;
    }
    SECTION("different resistor values") {
        REQUIRE(right.setResistance("feed", 390000.));
        REQUIRE(reference.setResistance("feed", 390000.));
        shouldShare = false;
    }
    REQUIRE(left.captureLiveTransferSetup(a, true));
    REQUIRE(right.captureLiveTransferSetup(b, true));
    a.build(0, false);
    b.build(0, false);
    const int output = left.nodeIndex("collector");
    REQUIRE(a.prepareCompiledPreviewPair(b, 0, output, true, 1.2, false) == shouldShare);
    REQUIRE(left.installLiveTransferSetup(a));
    REQUIRE(right.installLiveTransferSetup(b));
    REQUIRE(reference.prepareBipolarPreview(0, output));
    for (int n = 0; n < 128; ++n) {
        double yLeft, yRight, expected;
        const auto loud = n < 32 ? 2. : 0.;
        if (!left.stepBipolarPreview(loud, yLeft)) {
            const double sources[] = {loud, 9.};
            left.step(sources, 2);
        }
        const auto quiet = .3 * std::sin(n * .039);
        REQUIRE(right.stepBipolarPreview(quiet, yRight));
        REQUIRE(reference.stepBipolarPreview(quiet, expected));
        REQUIRE(yRight == expected);
    }
}

TEST_CASE("Physical stepping and reset resume the reactive state of bipolar Preview", "[engine][circuit-plan]") {
    auto c = bipolarPreviewStage();
    wdf::Engine candidate(c, 48000.), reference(c, 48000.);
    const int output = candidate.nodeIndex("collector");
    REQUIRE(candidate.prepareBipolarPreview(0, output));
    auto sources = candidate.dcValues();
    for (int n = 0; n < 128; ++n) {
        sources[0] = .3 * std::sin(n * .039);
        double voltage;
        REQUIRE(candidate.stepBipolarPreview(sources[0], voltage));
        REQUIRE(voltage == Approx(reference.step(sources)[output]).margin(2e-6));
    }
    REQUIRE(candidate.bipolarPreviewActive());
    REQUIRE(candidate.step(sources)[output] == Approx(reference.step(sources)[output]).margin(2e-6));
    REQUIRE_FALSE(candidate.bipolarPreviewActive());

    double voltage;
    REQUIRE(candidate.stepBipolarPreview(sources[0], voltage));
    candidate.reset();
    reference.reset();
    REQUIRE_FALSE(candidate.bipolarPreviewActive());
    sources[0] = 0.;
    REQUIRE(candidate.stepBipolarPreview(sources[0], voltage));
    REQUIRE(voltage == Approx(reference.step(sources)[output]).margin(1e-8));
    REQUIRE_FALSE(candidate.stepBipolarPreview(5., voltage));
    sources[0] = 5.;
    candidate.step(sources);
    reference.step(sources);
    sources[0] = 0.;
    for (int n = 0; n < 95; ++n) {
        REQUIRE_FALSE(candidate.stepBipolarPreview(0., voltage));
        candidate.step(sources);
        reference.step(sources);
    }
    REQUIRE(candidate.stepBipolarPreview(0., voltage));
    CHECK(voltage == Approx(reference.step(sources)[output]).margin(2e-6));
    candidate.reset();
    REQUIRE(candidate.stepBipolarPreview(0., voltage));
}

TEST_CASE("A resonant EQ control keeps its physical response over a wide live sweep", "[engine][circuit-plan]") {
    wdf::LiveWDFDocument doc;
    std::string error;
    REQUIRE(doc.loadFromFile("schematics/studio/eq/Neve 1081 Gold Label Equalizer.livewdf", error));
    const auto band = std::ranges::find(doc.components, "A4_SW_PRES-2", &wdf::SchematicComponent::id);
    REQUIRE(band != doc.components.end());
    band->properties["select"] = "1"; // Upper presence at 1.5 kHz.
    const auto circuit = doc.toCircuit();
    const auto source = std::ranges::find(circuit.src, doc.inputId(), &wdf::Src::name);
    REQUIRE(source != circuit.src.end());
    const auto input = size_t(source - circuit.src.begin());
    wdf::Engine preview(circuit, 48000), reference(circuit, 48000);
    const int output = preview.nodeIndex(doc.outputNet()), gain = preview.potIndexByName("A4_RV_PRES");
    REQUIRE(output >= 0);
    REQUIRE(gain >= 0);
    preview.prepareLiveControls();
    reference.prepareLiveControls();
    REQUIRE(preview.prepareBipolarPreview(input, output));
    auto sources = reference.dcValues();
    double actual;
    for (int n = 0; n < 1024; ++n) {
        if (n == 256) {
            REQUIRE(preview.setLiveControls({{gain, .85}}, {}));
            REQUIRE(reference.setLiveControls({{gain, .85}}, {}));
        }
        sources[input] = .52 * std::sin(.19635 * n) + .19 * std::sin(.3927 * n);
        REQUIRE(preview.stepBipolarPreview(sources[input], actual));
        REQUIRE(actual == Approx(reference.step(sources)[output]).margin(2e-4));
    }
    REQUIRE_FALSE(preview.stepBipolarPreview(1.5, actual));
}

TEST_CASE("An isolated series diode path retains its bleed resistor and midpoint voltage", "[engine][circuit-plan]") {
    struct InspectPlan : wdf::Engine {
        using Engine::Engine;
        size_t seriesPaths() const { return loadedJunction_.strings.size(); }
    };
    wdf::Circuit c;
    c.addSrc("audio", "0", 0.0);
    c.addRes("audio", "out", 10000);
    c.addCap("out", "0", 8.2e-9);
    c.addDiode("out", "0", wdf::DiodeModel(1e-10, 1.8, 4.0), "single");
    c.addDiode("0", "mid", wdf::DiodeModel(3e-9, 1.3, 2.0), "first");
    c.addDiode("mid", "out", wdf::DiodeModel(2e-12, 1.9, 18.0), "second");
    c.addRes("mid", "out", 1e8);
    for (bool loadedMidpoint : {false, true}) {
        if (loadedMidpoint) c.addCap("mid", "0", 10e-9);
        InspectPlan preview(c, 48000), exact(c, 48000);
        REQUIRE(preview.prepareLoadedJunctionPreview(0, false));
        REQUIRE(preview.seriesPaths() == (loadedMidpoint ? 0 : 1));
        preview.useLoadedJunctionPreview(true);
        for (int i = 0; i < 512; ++i) {
            const double src = 1.2 * std::sin(.037*i);
            const auto& a = exact.step(&src, 1, 60, 1e-9);
            const auto& b = preview.step(&src, 1, 60, 1e-9);
            for (const char* node : {"mid", "out"}) {
                const int j = exact.nodeIndex(node);
                REQUIRE(b[j] == Approx(a[j]).margin(.002));
            }
        }
        REQUIRE(preview.loadedJunctionHits() > 400);
    }
}

TEST_CASE("Compiled op-amp nodes keep every probe and reactive history through overload",
          "[engine][circuit-plan][opamp]") {
    struct Candidate : wdf::Engine {
        using Engine::Engine;
        using EngineStep::buildAffineNodePreview;
    };
    wdf::Circuit c;
    c.addSrc("in", "0", 0.05);
    c.addSrc("bias", "0", .03);
    c.addRes("in", "sum", 10000, "feed");
    c.addRes("out", "sum", 47000);
    c.addCap("sum", "out", 2.2e-9);
    c.addInd("out", "tap", .001);
    c.addRes("tap", "0", 800);
    auto model = wdf::OpAmpModel::fromType(wdf::OpAmpType::Tl072);
    model.setVRails(5.0, 5.0);
    c.addOpAmp("out", "sum", "bias", "0", "0", model, "U");
    Candidate preview(c, 48000);
    wdf::Engine reference(c, 48000);
    REQUIRE(preview.buildAffineNodePreview());
    preview.useLoadedJunctionPreview(true);
    long long beforeDisable = 0, beforeEdit = 0;
    for (int i = 0; i < 2050; ++i) {
        if (i == 512) {
            beforeDisable = preview.affineNodePreviewHits();
            REQUIRE(beforeDisable > 0);
            preview.setAllOpAmpSoftRails(true);
            reference.setAllOpAmpSoftRails(true);
        }
        if (i == 768) {
            REQUIRE(preview.affineNodePreviewHits() == beforeDisable);
            preview.setAllOpAmpSoftRails(false);
            reference.setAllOpAmpSoftRails(false);
        }
        if (i == 1536) {
            beforeEdit = preview.affineNodePreviewHits();
            REQUIRE(beforeEdit > beforeDisable);
            REQUIRE(reference.setResistance("feed", 15000));
            REQUIRE(preview.setResistance("feed", 15000));
        }
        const double drive = .05 + (i >= 768 && i < 1024 ? 2. : .025) * std::sin(.03 * i);
        const double sources[]{drive, .03 + .01 * std::cos(.07 * i)};
        const auto& expected = reference.step(sources, 2);
        const auto& actual = preview.step(sources, 2, 60, wdf::Engine::kFastLiveTolerance);
        REQUIRE(preview.lastStateRejected() == reference.lastStateRejected());
        for (int node = 0; node < preview.nodeCount(); ++node)
            REQUIRE(actual[node] == Approx(expected[node]).margin(2e-6));
        REQUIRE(preview.capacitorStates()[0] == Approx(reference.capacitorStates()[0]).margin(2e-6));
        REQUIRE(preview.inductorStates()[0] == Approx(reference.inductorStates()[0]).margin(2e-6));
    }
    REQUIRE(preview.affineNodePreviewHits() == beforeEdit);
}

TEST_CASE("Bipolar Preview follows a qualified control and preserves state on fallback",
          "[engine][circuit-plan][live-controls]") {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.);
    c.addSrc("supply", "0", 9.);
    c.addPot("in", "tap", "0", 10000.);
    c.addPot("tap", "level", "0", 47000.);
    c.addRes("level", "feed", 330000.);
    c.addCap("feed", "base", 100e-9);
    c.addRes("supply", "base", 22000.);
    c.addRes("base", "0", 4700.);
    c.addRes("supply", "collector", 3300.);
    c.addVarRes("emitter", "0", 1640., .5);
    c.addCap("collector", "0", 22e-9);
    c.addBjt("collector", "base", "emitter", wdf::findBjtByName("2N3904")->model, "Q1");

    wdf::Engine preview(c, 48000.), physical(c, 48000.);
    preview.prepareLiveControls();
    const int output = preview.nodeIndex("collector");
    REQUIRE(preview.prepareBipolarPreview(0, output));
    auto sources = physical.dcValues();
    const auto movePot = [&](int index, double position) {
        REQUIRE(preview.setLiveControls({{index, position}}, {}));
        REQUIRE(physical.setPotPosition(index, position));
    };
    for (int i = 0; i < 640; ++i) {
        CAPTURE(i);
        if (i == 128) movePot(0, .13);
        if (i == 256) movePot(1, .8);  // Held at the compiled position while the first control is live.
        if (i == 320) movePot(0, .5);
        if (i == 384 || i == 448) {
            const double setting = i == 384 ? .50001 : .5;  // This rheostat changes the DC bias.
            REQUIRE(preview.setLiveControls({}, {{0, setting}}));
            REQUIRE(physical.setVarResPosition(0, setting));
        }
        if (i == 512) {
            REQUIRE(preview.setPotPosition(1, .35));  // A new base transfer invalidates the old plan.
            REQUIRE(physical.setPotPosition(1, .35));
        }
        sources[0] = .06 * std::sin(.02 * i);
        const double expected = physical.step(sources)[output];
        double voltage = 0.;
        const bool active = preview.stepBipolarPreview(sources[0], voltage);
        REQUIRE(active == (i < 384 || (i >= 448 && i < 512)));
        if (!active) voltage = preview.step(sources)[output];
        if (i < 256 || i >= 320) REQUIRE(voltage == Approx(expected).margin(50e-6));
    }
}

TEST_CASE("Loaded junction preview follows a live pot and its return under clipping",
          "[engine][circuit-plan][live-junction]") {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.);
    c.addRes("in", "a", 2200);
    c.addPot("a", "mid", "b", 4700, .5, wdf::PotTaper::Linear, "Gain");
    c.addRes("b", "0", 3300);
    c.addCap("b", "0", 4.7e-9);
    for (auto n : {"a", "mid", "b"})
        c.addDiode(n, "0", wdf::DiodeModel(2e-12, 1.1, 4.0), n);
    struct PreviewEngine : wdf::Engine {
        using Engine::Engine;
        using EngineStep::swapLoadedJunctionPreview;
    };
    PreviewEngine preview(c, 48000);
    wdf::Engine worker(c, 48000), accurate(c, 48000);
    preview.prepareLiveControls();
    accurate.prepareLiveControls();
    REQUIRE(worker.prepareLoadedJunctionPreview(0, false));
    REQUIRE(preview.setLiveControls({{0, .7}}, {}));
    REQUIRE(preview.setLiveControls({{0, .5}}, {}));
    REQUIRE(preview.liveControlRevision() != worker.liveControlRevision());
    preview.swapLoadedJunctionPreview(worker);
    REQUIRE(preview.hasLoadedJunctionPreview());
    preview.useLoadedJunctionPreview(true);
    int frame = 0;
    for (const auto position : {.5, .86, .2, .5, .86, .5}) {
        CAPTURE(position);
        REQUIRE(preview.setLiveControls({{0, position}}, {}));
        REQUIRE(accurate.setLiveControls({{0, position}}, {}));
        const auto before = preview.loadedJunctionHits();
        for (int i = 0; i < 256; ++i) {
            const double input = 3.2 * std::sin(.07 * frame++) + .9 * std::sin(.31 * i);
            const auto& ref = accurate.step(&input, 1, 60, 1e-9);
            const auto& signal = preview.step(&input, 1, 60, 1e-9);
            for (auto n : {"a", "mid", "b"}) {
                const auto j = accurate.nodeIndex(n);
                REQUIRE(signal[j] == Approx(ref[j]).margin(.001));
            }
        }
        CHECK(preview.loadedJunctionHits() - before > 230);
    }
}

TEST_CASE("Loaded junction updates accept four independent resistor changes and decline five",
          "[engine][circuit-plan][live-junction]") {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.);
    for (auto n : {"a", "b", "c", "d", "e"}) {
        c.addVarRes("in", n, 10000, .5, wdf::PotTaper::Linear, n);
        c.addRes(n, "0", 2200);
        c.addDiode(n, "0", wdf::DiodeModel(2e-12, 1.1, 4.0), n);
    }
    for (int rank : {3, 4, 5}) {
        CAPTURE(rank);
        wdf::Engine preview(c, 48000), accurate(c, 48000);
        preview.prepareLiveControls();
        accurate.prepareLiveControls();
        REQUIRE(preview.prepareLoadedJunctionPreview(0, false));
        preview.useLoadedJunctionPreview(true);
        std::vector<std::pair<int, double>> settings;
        for (int i = 0; i < rank; ++i)
            settings.emplace_back(i, .15 + .15 * i);
        for (auto* engine : {&preview, &accurate})
            REQUIRE(engine->setLiveControls({}, settings));
        const double input = 2.;
        const auto& ref = accurate.step(&input, 1);
        const auto& signal = preview.step(&input, 1);
        CHECK(preview.hasLoadedJunctionPreview() == (rank <= 4));
        CHECK(preview.loadedJunctionHits() == (rank <= 4 ? 1 : 0));
        for (auto n : {"a", "b", "c", "d", "e"})
            CHECK(signal[accurate.nodeIndex(n)] == Approx(ref[accurate.nodeIndex(n)]).margin(1e-5));
    }
}

TEST_CASE("The Klon dual-gang gain control preserves the coupled junction solve",
          "[engine][circuit-plan][live-junction]") {
    wdf::LiveWDFDocument doc;
    std::string error;
    REQUIRE(doc.loadFromFile("schematics/pedals_fx/Klon Centaur.livewdf", error));
    auto circuit = doc.toCircuit();
    wdf::Engine preview(circuit, 48000), accurate(circuit, 48000);
    preview.prepareLiveControls();
    accurate.prepareLiveControls();
    const auto source = std::ranges::find(circuit.src, doc.inputId(), &wdf::Src::name);
    REQUIRE(source != circuit.src.end());
    const auto input = size_t(source - circuit.src.begin());
    const auto output = preview.nodeIndex(doc.outputNet());
    REQUIRE(output >= 0);
    REQUIRE(preview.prepareLoadedJunctionPreview(input, false));
    preview.useLoadedJunctionPreview(true);
    const auto gains = wdf::potControlGroups(doc);
    const auto gain = std::ranges::find(gains, "GAIN", &wdf::LiveControlGroup::name);
    REQUIRE(gain != gains.end());
    REQUIRE(gain->members.size() == 2);
    auto sources = preview.dcValues();
    int frame = 0;
    for (const double position : {.75, .15, 1., 0., .5}) {
        CAPTURE(position);
        std::vector<std::pair<int, double>> gang;
        for (const auto& member : gain->members)
            gang.emplace_back(preview.potIndexByName(member.id), position);
        REQUIRE(accurate.setLiveControls(gang, {}));
        REQUIRE(preview.setLiveControls(gang, {}));
        auto hits = preview.loadedJunctionHits();
        for (int i = 0; i < 256; ++i, ++frame) {
            sources[input] = .55 * std::sin(.11 * frame) + .25 * std::sin(.039 * frame);
            const auto voltage = preview.step(sources)[output];
            REQUIRE(voltage == Approx(accurate.step(sources)[output]).margin(2e-4));
        }
        REQUIRE(preview.loadedJunctionHits() - hits > 240);
    }
}

namespace {
struct ReversePlanProbe : wdf::Engine {
    using Engine::Engine;
    size_t cutoffCount() const { return loadedJunction_.reverseCutoffs.size(); }
    double cutoffVoltage() const { return loadedJunction_.reverseCutoffs.at(0).maxVbc; }
    double omittedVoltageResidual() const {
        const auto& guard = loadedJunction_.reverseCutoffs.at(0);
        const auto& bjt = bjts_[guard.device];
        const double reverse = bjt.Is * std::exp(guard.maxVbc / bjt.nrVt);
        double worst = 0.;
        for (int i = 0; i < K_; ++i) {
            const auto* row = Z_.data() + i * K_;
            const double omitted = reverse * (row[guard.port] / bjt.alphaR - row[guard.port + 1] / bjt.Br);
            worst = std::max(worst, std::abs(omitted));
        }
        return worst;
    }
    std::vector<std::uint8_t> crossReverseCutoffs(size_t input) {
        std::vector<std::uint8_t> crossed(nBjt_);
        screenJunctionBenefit(input, crossed);
        return crossed;
    }
    bool retryWithFullReverse(size_t input, std::span<const std::uint8_t> crossed) {
        std::vector<bool> linearAmps(nOpAmp_), buffers(nBjt_);
        screenJunctionDevices(input, linearAmps, buffers);
        bool negativeReverse = false;
        return buildLoadedJunctionPlan(linearAmps, buffers, true, negativeReverse, crossed);
    }
    bool prepareGuardedVariant() {
        bool negativeReverse = false;
        return buildLoadedJunctionPlan(std::vector<bool>(nOpAmp_, false),
                                       std::vector<bool>(nBjt_, false), true, negativeReverse);
    }
};
}

TEST_CASE("Guarded reverse transport returns to physical solving before BJT saturation",
          "[engine][circuit-plan][reverse-cutoff]") {
    for (double sign : {1.0, -1.0}) {
        CAPTURE(sign);
        wdf::Circuit c;
        c.addSrc("rail", "0", sign * 5.);
        c.addSrc("drive", "0", sign * .53);
        c.addRes("rail", "collector", 470.);
        c.addRes("drive", "base", 330.);
        c.addRes("emitter", "0", 220.);
        c.addCap("collector", "0", 1e-9);
        const auto& model = wdf::findBjtByName(sign > 0. ? "2N3904" : "2N3906")->model;
        c.addBjt("collector", "base", "emitter", model, "Q");
        c.addDiode("base", "emitter", wdf::DiodeModel(1e-14, 1.6, 4.0), "D");

        ReversePlanProbe preview(c, 48000.);
        wdf::Engine physical(c, 48000.);
        REQUIRE(preview.prepareGuardedVariant());
        REQUIRE(preview.cutoffCount() == 1);
        preview.useLoadedJunctionPreview(true);
        const int b = physical.nodeIndex("base");
        const int collector = physical.nodeIndex("collector");
        auto sources = physical.dcValues();
        int forwardHits = 0, saturationFallbacks = 0, recoveryHits = 0;
        for (int i = 0; i < 384; ++i) {
            sources[0] = sign * (i >= 128 && i < 192 ? -.1 : 5.);
            sources[1] = sign * (.53 + .015 * std::sin(.07 * i));
            const auto& expected = physical.step(sources);
            const auto hits = preview.loadedJunctionHits();
            const auto& actual = preview.step(sources);
            const bool used = preview.loadedJunctionHits() != hits;
            REQUIRE_FALSE(preview.lastStateRejected());
            REQUIRE(actual[collector] == Approx(expected[collector]).margin(3e-4));
            const double vbc = sign * (expected[b] - expected[collector]);
            if (vbc > 0.) {
                REQUIRE_FALSE(used);
                ++saturationFallbacks;
            } else if (i < 128 && used) {
                ++forwardHits;
            } else if (i > 224 && used) {
                ++recoveryHits;
            }
        }
        REQUIRE(forwardHits > 100);
        REQUIRE(saturationFallbacks > 40);
        REQUIRE(recoveryHits > 100);
    }
}

TEST_CASE("High leakage and resistance narrow the permissible reverse-junction range",
          "[engine][circuit-plan][reverse-cutoff]") {
    wdf::Circuit c;
    c.addSrc("rail", "0", 9.);
    c.addVarRes("rail", "collector", 2e6, .5);
    c.addRes("base", "0", 1000.);
    const wdf::BjtModel model{1e-7, 100., 1., 1., 1.};
    c.addBjt("collector", "base", "0", model, "Q");
    c.addDiode("base", "0", wdf::DiodeModel(1e-14, 1.6, 4.0), "D");
    ReversePlanProbe preview(c, 48000.);
    preview.prepareLiveControls();
    REQUIRE(preview.prepareGuardedVariant());
    REQUIRE(preview.cutoffCount() == 1);
    // Even through the 1 MΩ collector, the omitted IC contribution stays
    // below the Fast Live root residual budget.
    const double cutoff = preview.cutoffVoltage();
    REQUIRE(cutoff < -12. * model.Nr * model.Vt);
    REQUIRE(preview.omittedVoltageResidual() <= wdf::Engine::kFastLiveTolerance + 1e-18);
    preview.useLoadedJunctionPreview(true);
    REQUIRE(preview.setLiveControls({}, {{0, 1.}}));
    preview.step(preview.dcValues());
    REQUIRE(preview.cutoffVoltage() < cutoff);
    REQUIRE(preview.omittedVoltageResidual() <= wdf::Engine::kFastLiveTolerance + 1e-18);
}

TEST_CASE("The loaded solver retries a negative reverse-branch resistance on a compressor",
          "[engine][circuit-plan][reverse-cutoff]") {
    wdf::LiveWDFDocument doc;
    std::string err;
    REQUIRE(doc.loadFromFile("schematics/studio/dynamics/UREI-LA-3A.livewdf", err));
    const auto circuit = doc.toCircuit();
    ReversePlanProbe preview(circuit, 48000.);
    wdf::Engine physical(circuit, 48000.);
    REQUIRE(preview.prepareLoadedJunctionPreview(0, false));
    REQUIRE(preview.cutoffCount() == 0);
    for (auto* engine : std::array<wdf::Engine*, 2>{&preview, &physical}) {
        REQUIRE(engine->setPotPosition("GAIN", .45));
        REQUIRE(engine->setPotPosition("SENS", .90));
        engine->reset();
    }
    REQUIRE(preview.prepareLoadedJunctionPreview(0, false));
    REQUIRE(preview.cutoffCount() > 0);
    preview.useLoadedJunctionPreview(true);
    auto sources = physical.dcValues();
    const auto inputSource = std::find_if(circuit.src.begin(), circuit.src.end(),
        [&](const auto& s) { return s.name == doc.inputId(); });
    REQUIRE(inputSource != circuit.src.end());
    const auto input = size_t(inputSource - circuit.src.begin());
    const int output = physical.nodeIndex(doc.outputNet());
    double signalEnergy = 0., errorEnergy = 0.;
    for (int i = 0; i < 2048; ++i) {
        sources[input] = .35 * std::sin(.07 * i);
        const double actual = preview.step(sources)[output];
        const double expected = physical.step(sources)[output];
        REQUIRE_FALSE(preview.lastStateRejected());
        signalEnergy += expected * expected;
        errorEnergy += (actual - expected) * (actual - expected);
    }
    REQUIRE(preview.loadedJunctionHits() > 1900);
    REQUIRE(errorEnergy < 1e-6 * signalEnergy);
}

TEST_CASE("A contracting BJT root stays on Preview across a fresh compressor transient",
          "[engine][circuit-plan][live-junction]") {
    wdf::LiveWDFDocument doc;
    std::string error;
    REQUIRE(doc.loadFromFile("schematics/studio/dynamics/UREI-LA-3A.livewdf", error));
    auto circuit = doc.toCircuit();
    for (auto& pot : circuit.pots) {
        if (pot.name == "GAIN") pot.position = .45;
        if (pot.name == "SENS") pot.position = .85;
    }
    wdf::Engine preview(circuit, 48000.), accurate(circuit, 48000.);
    const auto source = std::ranges::find(circuit.src, doc.inputId(), &wdf::Src::name);
    REQUIRE(source != circuit.src.end());
    const int input = static_cast<int>(source - circuit.src.begin());
    const int output = preview.nodeIndex(doc.outputNet());
    REQUIRE(output >= 0);
    REQUIRE(preview.prepareLoadedJunctionPreview(input, false));
    preview.useLoadedJunctionPreview(true);

    auto sources = accurate.dcValues();
    auto previousHits = preview.loadedJunctionHits();
    double energy = 0., errorEnergy = 0.;
    for (int i = 0; i < 1280; ++i) {
        if (i == 1024) previousHits = preview.loadedJunctionHits();
        const double envelope = std::exp(-double(i % 1024) / 192.);
        sources[input] = envelope * (std::sin(.158*i) + .35*std::sin(.47*i));
        const double actual = preview.step(sources, wdf::Engine::kDefaultMaxIterations,
                                           wdf::Engine::kFastLiveTolerance)[output];
        const double expected = accurate.step(sources)[output];
        energy += expected * expected;
        errorEnergy += (actual - expected) * (actual - expected);
    }
    CHECK(preview.loadedJunctionHits() - previousHits == 256);
    CHECK(errorEnergy < 1.e-5 * energy);
}

TEST_CASE("A compressor sidechain retains the full reverse laws that cross cutoff on transients",
          "[engine][circuit-plan][reverse-cutoff]") {
    wdf::LiveWDFDocument doc;
    std::string reason;
    REQUIRE(doc.loadFromFile("schematics/studio/dynamics/UREI-1176LN-Rev-D.livewdf", reason));
    const auto setting = std::find_if(doc.components.begin(), doc.components.end(),
                                      [](const auto& component) { return component.id == "R1"; });
    REQUIRE(setting != doc.components.end());
    setting->values["pos"] = .65;
    const auto circuit = doc.toCircuit();
    const auto input = std::find_if(circuit.src.begin(), circuit.src.end(),
                                    [&](const auto& source) { return source.name == doc.inputId(); });
    REQUIRE(input != circuit.src.end());
    const auto source = size_t(input - circuit.src.begin());
    ReversePlanProbe preview(circuit, 48000.);
    wdf::Engine reference(circuit, 48000.);
    REQUIRE(preview.prepareLoadedJunctionPreview(source, false));
    const auto before = preview.cutoffCount();
    const auto crossed = preview.crossReverseCutoffs(source);
    const auto count = std::count(crossed.begin(), crossed.end(), std::uint8_t{1});
    REQUIRE(count > 0);
    REQUIRE(size_t(count) < before);
    REQUIRE(preview.retryWithFullReverse(source, crossed));
    REQUIRE(preview.cutoffCount() == before - size_t(count));
    preview.useLoadedJunctionPreview(true);
    auto values = reference.dcValues();
    const int output = reference.nodeIndex(doc.outputNet());
    double refEnergy = 0., errEnergy = 0.;
    for (int i = 0; i < 3072; ++i) {
        const double time = double(i) / 48000.;
        const double envelope = std::exp(-double(i % 512) / 115.);
        values[source] = 1.3 * envelope * (.7 * std::sin(5100.*time) + .3 * std::sin(45800.*time));
        const double actual = preview.step(values, 60, wdf::Engine::kFastLiveTolerance)[output];
        const double expected = reference.step(values)[output];
        REQUIRE_FALSE(preview.lastStateRejected());
        REQUIRE(std::abs(actual - expected) < .002);
        refEnergy += expected * expected;
        errEnergy += (actual - expected) * (actual - expected);
    }
    REQUIRE(errEnergy < 1e-6 * refEnergy);
    REQUIRE(preview.loadedJunctionHits() > 3000);
}

namespace {
struct AffineControlProbe : wdf::Engine {
    using Engine::Engine;
    bool enableForTest() {
        prepareLiveControls();
        if (!buildAffineNodePreview()) return false;
        prepareAffineMotion();
        useLoadedJunctionPreview(true);
        return bool(affineNodes_.motion);
    }
    bool ownsSeparateWorkspace(const AffineControlProbe& other) const {
        return affineNodes_.motion->root.inverse.data() != other.affineNodes_.motion->root.inverse.data() &&
               affineNodes_.motion->scratch.full.data() != other.affineNodes_.motion->scratch.full.data();
    }
};
}

TEST_CASE("Affine live controls preserve frequency and gain on the SSL EQ while both move",
          "[engine][circuit-plan][affine-motion]") {
    wdf::LiveWDFDocument doc;
    std::string reason;
    REQUIRE(doc.loadFromFile("schematics/studio/eq/SSL 4000 E 82E242 Black EQ.livewdf", reason));
    const auto circuit = doc.toCircuit();
    AffineControlProbe preview(circuit, 48000.);
    wdf::Engine reference(circuit, 48000.);
    REQUIRE(preview.enableForTest());
    const AffineControlProbe copy = preview;
    REQUIRE(preview.ownsSeparateWorkspace(copy));
    reference.prepareLiveControls();
    const auto src = std::find_if(circuit.src.begin(), circuit.src.end(),
                                  [&](const auto& s) { return s.name == doc.inputId(); });
    REQUIRE(src != circuit.src.end());
    const int gain = preview.potIndexByName("G_HMF");
    const int freqA = preview.potIndexByName("F_HMF_A");
    const int freqB = preview.potIndexByName("F_HMF_B");
    REQUIRE(std::min({gain, freqA, freqB}) >= 0);
    auto sources = reference.dcValues();
    for (int i = 0; i < 1536; ++i) {
        if (i % 8 == 0) {
            const double phase = std::sin(i * .007);
            const double f = .4 + .23 * phase;
            const std::vector<std::pair<int,double>> pots{{gain, .54 - .25 * phase}, {freqA, f}, {freqB, f}};
            REQUIRE(preview.setLiveControls(pots, {}));
            REQUIRE(reference.setLiveControls(pots, {}));
        }
        sources[size_t(src - circuit.src.begin())] = .35 * std::sin(.13*i) + .13 * std::sin(.027*i);
        const auto& expected = reference.step(sources);
        const auto& actual = preview.step(sources, 60, wdf::Engine::kFastLiveTolerance);
        REQUIRE_FALSE(preview.lastStateRejected());
        for (int node = 0; node < preview.nodeCount(); ++node)
            REQUIRE(actual[node] == Approx(expected[node]).margin(2e-6));
    }
    REQUIRE(preview.affineRootHits() > 1400);
}

TEST_CASE("An op-amp feedback control can revoke uniqueness and recover it",
          "[engine][circuit-plan][affine-motion]") {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.);
    c.addRes("in", "negative", 10000.);
    c.addRes("out", "negative", 10000.);
    c.addRes("positive", "0", 10000.);
    c.addVarRes("out", "positive", 100000., .99);
    c.addRes("out", "0", 10000.);
    c.addCap("out", "negative", 100e-12);
    auto op = wdf::OpAmpModel::fromType(wdf::OpAmpType::Tl072);
    op.setVRails(5., 5.);
    c.addOpAmp("out", "negative", "positive", "0", "0", op, "U");

    AffineControlProbe preview(c, 48000.);
    wdf::Engine reference(c, 48000.);
    REQUIRE(preview.enableForTest());
    reference.prepareLiveControls();
    const int out = reference.nodeIndex("out");
    const std::array positions{.90, .001, .90, .85, .95};
    std::array<long long, positions.size()> hits{};
    for (size_t section = 0; section < positions.size(); ++section) {
        REQUIRE(preview.setLiveControls({}, {{0, positions[section]}}));
        REQUIRE(reference.setLiveControls({}, {{0, positions[section]}}));
        const auto before = preview.affineRootHits();
        for (int i = 0; i < 512; ++i) {
            const auto t = int(section) * 512 + i;
            const double input = section == 1 ? (i < 50 ? 5. : 0.) :
                                 section == 3 ? 18. * std::sin(.03 * t) : .1 * std::sin(.09 * t);
            const auto& expected = reference.step(&input, 1);
            const auto& actual = preview.step(&input, 1, 60, wdf::Engine::kFastLiveTolerance);
            REQUIRE(preview.lastStateRejected() == reference.lastStateRejected());
            REQUIRE(actual[out] == Approx(expected[out]).margin(2e-6));
        }
        hits[section] = preview.affineRootHits() - before;
    }
    REQUIRE(hits[0] > 480);
    REQUIRE(hits[1] == 0);
    REQUIRE(hits[2] > 480);
    REQUIRE(hits[3] < 480);
    REQUIRE(hits[4] > 480);
}

namespace {

wdf::Circuit largeLoadedOpAmpChain(int amps) {
    wdf::Circuit circuit;
    circuit.addSrc("in", "0", 0.0);
    circuit.addPot("in", "drive", "0", 10000.0, .5, wdf::PotTaper::Linear, "Level");
    auto model = wdf::OpAmpModel::fromType(wdf::OpAmpType::Tl072);
    model.setVRails(5.0, 5.0);
    std::string previous = "drive";
    for (int index = 0; index < amps; ++index) {
        const auto output = "stage" + std::to_string(index);
        circuit.addOpAmp(output, output, previous, "0", "0", model,
                         "U" + std::to_string(index));
        circuit.addRes(output, "0", 100000.0);
        previous = output;
    }
    circuit.addRes(previous, "out", 2200.0);
    circuit.addCap("out", "0", 10e-9);
    circuit.addDiode("out", "0", wdf::DiodeModel(2e-12, 1.1, 4.0), "D1");
    circuit.addDiode("0", "out", wdf::DiodeModel(2e-12, 1.1, 4.0), "D2");
    return circuit;
}

} // namespace

TEST_CASE("Large loaded junction plans retain live controls and op-amp overload recovery",
          "[engine][circuit-plan][live-junction][large-junction]") {
    auto circuit = largeLoadedOpAmpChain(21);
    wdf::Engine preview(circuit, 48000.0), accurate(circuit, 48000.0);
    REQUIRE(preview.rootCount() == 65);
    preview.prepareLiveControls();
    accurate.prepareLiveControls();
    REQUIRE(preview.prepareLoadedJunctionPreview(0, false));
    preview.useLoadedJunctionPreview(true);
    const auto output = preview.nodeIndex("out");
    REQUIRE(output >= 0);
    long long quietHits = 0, overloadFallbacks = 0, recoveryHits = 0;
    for (int frame = 0; frame < 2048; ++frame) {
        if (frame == 256 || frame == 768 || frame == 1536) {
            const double position = frame == 768 ? .8 : .3;
            REQUIRE(preview.setLiveControls({{0, position}}, {}));
            REQUIRE(accurate.setLiveControls({{0, position}}, {}));
        }
        const bool overload = frame >= 768 && frame < 1024;
        const double input = (overload ? 20.0 : .08) * std::sin(.079 * frame);
        const auto previousHits = preview.loadedJunctionHits();
        const auto& expectedNodes = accurate.step(&input, 1, 60, 1e-9);
        const auto& actualNodes = preview.step(&input, 1, 60, 1e-9);
        const double expected = expectedNodes[output];
        const double actual = actualNodes[output];
        for (size_t node = 0; node < actualNodes.size(); ++node)
            REQUIRE(actualNodes[node] == Approx(expectedNodes[node]).margin(.001));
        REQUIRE(preview.getCapacitorState(0) == Approx(accurate.getCapacitorState(0)).margin(.002));
        REQUIRE(std::isfinite(actual));
        REQUIRE_FALSE(preview.lastStateRejected());
        REQUIRE_FALSE(accurate.lastStateRejected());
        REQUIRE(actual == Approx(expected).margin(.001));
        const bool hit = preview.loadedJunctionHits() != previousHits;
        if (frame < 768) quietHits += hit;
        if (overload) overloadFallbacks += !hit;
        if (frame >= 1536) recoveryHits += hit;
    }
    CHECK(quietHits > 600);
    CHECK(overloadFallbacks > 100);
    CHECK(recoveryHits > 400);
    CHECK(preview.hasLoadedJunctionPreview());
}

TEST_CASE("Loaded junction port capacity is bounded consistently",
          "[engine][circuit-plan][large-junction]") {
    auto supported = largeLoadedOpAmpChain(42);
    wdf::Engine boundary(supported, 48000.0);
    REQUIRE(boundary.rootCount() == wdf::LoadedJunctionPlan::maxPorts);
    REQUIRE(boundary.prepareLoadedJunctionPreview(0, false));
    auto oversized = largeLoadedOpAmpChain(43);
    wdf::Engine declined(oversized, 48000.0);
    REQUIRE(declined.rootCount() > wdf::LoadedJunctionPlan::maxPorts);
    CHECK_FALSE(declined.prepareLoadedJunctionPreview(0, false));
}

TEST_CASE("Tube Preview follows the Soldano normal gain changes without rejected samples",
          "[engine][circuit-plan][triode-preview]") {
    wdf::LiveWDFDocument full, document;
    std::string error;
    REQUIRE(full.loadFromFile("schematics/amps/Soldano SLO-100 - Overdrive.livewdf", error));
    REQUIRE(full.makePreampDocument(document, error));
    const auto circuit = wdf::circuitWithPortImpedances(document);
    const auto source = std::ranges::find(circuit.src, document.inputId(), &wdf::Src::name);
    REQUIRE(source != circuit.src.end());
    const auto input = static_cast<size_t>(source - circuit.src.begin());
    const auto controls = wdf::potControlGroups(document);
    const auto normal = std::ranges::find(controls, "Normal Gain", &wdf::LiveControlGroup::name);
    REQUIRE(normal != controls.end());
    REQUIRE(normal->members.size() == 1);

    wdf::Engine preview(circuit, 48000.), accurate(circuit, 48000.);
    preview.useTriodePreview(true);
    preview.prepareLiveControls();
    accurate.prepareLiveControls();
    const int knob = preview.potIndexByName(normal->members.front().id);
    const int output = preview.nodeIndex(document.outputNet());
    REQUIRE(knob >= 0);
    REQUIRE(output >= 0);
    auto sources = preview.dcValues();
    double signalEnergy = 0., errorEnergy = 0., peakError = 0.;
    for (int sample = 0; sample < 8192; ++sample) {
        if (sample == 512 || sample == 1792 || sample == 2560) {
            const double position = sample == 1792 ? .05 : 1.;
            REQUIRE(preview.setLiveControls({{knob, position}}, {}));
            REQUIRE(accurate.setLiveControls({{knob, position}}, {}));
        }
        const double signal = .75 * std::sin(.055 * sample) + .17 * std::sin(.13 * sample);
        sources[input] = sample < 4096 ? document.inputVoltsPerFullScale() * signal : 0.;
        const double expected = accurate.step(sources)[output];
        const double actual = preview.step(sources)[output];
        const double delta = actual - expected;
        signalEnergy += expected * expected;
        errorEnergy += delta * delta;
        peakError = std::max(peakError, std::abs(delta));
    }
    CHECK(preview.triodePreviewHits() > 8000);
    CHECK(preview.stateRejectCount() == 0);
    CHECK(errorEnergy < 1.e-6 * signalEnergy);
    CHECK(peakError / document.outputFullScaleVolts() < .001);
}
