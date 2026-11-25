#include <catch2.hpp>

#include <cmath>
#include <string>

#include "wdf/Engine.h"
#include "spice/SpiceNet.h"

// Phase 1 increment 1: prove the provided engine machinery (SpiceNet parser +
// multiport-Newton Engine) works inside the LiveWDF tree on basic R/C/V nets.
// Oracle targets for later increments: ngspice + wdf_compiler generated code.

namespace {
constexpr double kFs = 48000.0;
}  // namespace

TEST_CASE("dc voltage divider: parser + Engine solveDc", "[phase1][dc]") {
    const std::string net = R"(
* simple resistive divider
V1 in 0 DC 5
R1 in out 1k
R2 out 0 1k
)";
    wdf::Circuit c = wdf::buildFromString(net);
    REQUIRE(c.res.size() == 2);
    REQUIRE(c.src.size() == 1);

    wdf::Engine e(c, kFs);
    std::vector<double> node, rootV;
    REQUIRE(e.solveDc(node, rootV));
    const int outIdx = e.nodeIndex("out");
    REQUIRE(outIdx >= 0);
    REQUIRE(std::abs(node[static_cast<size_t>(outIdx)] - 2.5) < 1e-6);
}

TEST_CASE("rc lowpass: DC through cap is open, transient stays finite", "[phase1][dc][tran]") {
    const std::string net = R"(
V1 in 0 DC 1
R1 in out 1k
C1 out 0 1u
)";
    wdf::Circuit c = wdf::buildFromString(net);
    REQUIRE(c.cap.size() == 1);

    wdf::Engine e(c, kFs);
    std::vector<double> node, rootV;
    REQUIRE(e.solveDc(node, rootV));
    const int outIdx = e.nodeIndex("out");
    REQUIRE(outIdx >= 0);
    // DC: capacitor is an open circuit -> Vout = Vin.
    REQUIRE(std::abs(node[static_cast<size_t>(outIdx)] - 1.0) < 1e-6);

    // Seed capacitor states from the DC operating point, then drive with a sine.
    e.reset();
    const double freq = 100.0;  // well below fc = 1/(2*pi*RC) ~ 159 Hz
    int convergedCount = 0;
    const int nSamples = static_cast<int>(kFs / 10.0);  // 100 ms
    for (int n = 0; n < nSamples; ++n) {
        const double t = static_cast<double>(n) / kFs;
        const double vIn = std::sin(2.0 * M_PI * freq * t);
        const std::vector<double> vOut = e.step({vIn});
        REQUIRE(std::isfinite(vOut[static_cast<size_t>(outIdx)]));
        if (e.lastConverged())
            ++convergedCount;
        // LTI sanity: output can't exceed input magnitude for this passive RC.
        REQUIRE(std::abs(vOut[static_cast<size_t>(outIdx)]) <= 1.0 + 1e-9);
    }
    REQUIRE(convergedCount == nSamples);  // passive linear circuit: every sample converges

    // Amplitude at 100 Hz for fc ~ 159 Hz: |H| = 1/sqrt(1+(f/fc)^2) ~ 0.847 of input.
    // Skip settling: compare peak over last 50 ms against analytic steady state.
    double peakOut = 0.0;
    for (int n = nSamples / 2; n < nSamples; ++n) {
        const double t = static_cast<double>(n) / kFs;
        const double vIn = std::sin(2.0 * M_PI * freq * t);
        const std::vector<double> vOut = e.step({vIn});
        peakOut = std::max(peakOut, std::abs(vOut[static_cast<size_t>(outIdx)]));
    }
    const double fc = 1.0 / (2.0 * M_PI * 1000.0 * 1e-6);
    const double expected = 1.0 / std::sqrt(1.0 + std::pow(freq / fc, 2.0));
    REQUIRE(std::abs(peakOut - expected) < 0.01);
}

TEST_CASE("Capacitor startup voltage preserves polarity and repeats on reset",
          "[engine][cap_seed]") {
    wdf::Circuit circuit;
    circuit.addSrc("in", "0", 1.0);
    circuit.addRes("in", "out", 1000.0);
    circuit.addCap("0", "out", 1e-6, "C1", 0.25);
    const std::vector<double> sources{1.0};

    wdf::Engine seeded(circuit, 48000.0);
    const int out = seeded.nodeIndex("out");
    REQUIRE(out >= 0);
    CHECK(seeded.getCapacitorState(0) == 0.25);
    const double first = seeded.step(sources).at(out);
    CHECK(first == Approx((-0.25 + 1.0 / 96.0) / (1.0 + 1.0 / 96.0)));
    for (int n = 0; n < 64; ++n)
        seeded.step(sources);
    seeded.reset();
    CHECK(seeded.step(sources).at(out) == first);

    circuit.cap[0].initialVolts.reset();
    wdf::Engine dc(circuit, 48000.0);
    CHECK(dc.getCapacitorState(0) == Approx(-1.0));
    CHECK(dc.step(sources).at(out) == Approx(1.0));
}

TEST_CASE("Rejected-state recovery matches a fresh DC solve across live changes", "[engine][dc-recovery]") {
    class Probe : public wdf::Engine {
    public:
        using Engine::Engine;
        bool prepared() const { return hasRecoveryState(); }
        const auto& rootCurrents() const { return ii_; }
        double opAmpPole() const { return opAmpVp_.front(); }
        void stall(bool discard) {
            consecutiveStateRejects_ = 8;
            if (discard) recovery_.valid = false;
        }
    };

    wdf::Circuit c;
    c.addSrc("in", "0", 0.25);
    c.addPot("in", "tap", "0", 5000., 0.5);
    c.addOpAmp("out", "out", "tap", "0", "0",
               wdf::OpAmpModel::fromType(wdf::OpAmpType::Tl072), "U1");
    c.addRes("out", "load", 100.);
    c.addCap("load", "0", 1e-6);
    c.addInd("load", "return", 1e-3);
    c.addRes("return", "0", 330.);
    Probe live(c, 48000.), reference(c, 48000.);
    live.useLoadedJunctionPreview(true);
    reference.useLoadedJunctionPreview(true);
    live.prepareLiveControls();
    reference.prepareLiveControls();

    const auto recover = [&](bool expectedPrepared) {
        REQUIRE(live.prepared() == expectedPrepared);
        live.stall(false);
        reference.stall(true);
        for (int n = 0; n < 16; ++n) {
            const std::vector<double> input{0.9 * std::cos(0.13 * n)};
            REQUIRE(live.step(input) == reference.step(input));
            REQUIRE(live.capacitorStates() == reference.capacitorStates());
            REQUIRE(live.inductorStates() == reference.inductorStates());
            REQUIRE(live.rootCurrents() == reference.rootCurrents());
            REQUIRE(live.opAmpPole() == reference.opAmpPole());
        }
    };
    recover(true);
    for (auto* engine : {&live, &reference})
        REQUIRE(engine->setLiveControls({{0, 0.73}}, {}));
    recover(false);
    recover(true);
    for (auto* engine : {&live, &reference}) {
        REQUIRE(engine->setLiveControls({{0, 0.15}}, {}));
        wdf::Engine::LiveTransferSetup setup(c);
        REQUIRE(engine->captureLiveTransferSetup(setup));
        setup.build(0, false);
        REQUIRE_FALSE(engine->prepared());
        REQUIRE(engine->installLiveTransferSetup(setup));
    }
    recover(true);
    for (auto* engine : {&live, &reference})
        engine->setOpAmpSoftRails(0, !engine->isOpAmpSoftRails(0));
    recover(false);
    recover(true);
    for (auto* engine : {&live, &reference})
        engine->enableOpAmpRailSag(0);
    recover(false);
    REQUIRE_FALSE(live.prepared());
}
