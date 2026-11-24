#include <catch2.hpp>
#include <cmath>

#include "wdf/Engine.h"

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
        if (i == 256) movePot(1, .8);  // Both control networks differ from the qualified plan.
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
        REQUIRE(active == (i < 256 || (i >= 320 && i < 384) || (i >= 448 && i < 512)));
        if (!active) voltage = preview.step(sources)[output];
        REQUIRE(voltage == Approx(expected).margin(50e-6));
    }
}
