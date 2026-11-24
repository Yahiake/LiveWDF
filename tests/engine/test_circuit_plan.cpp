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
