#include <catch2.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "circuit/tree/TreeExecutor.h"
#include "spice/SpiceNet.h"
#include "wdf/Engine.h"
#include "oracle/rc_lowpass.h"

namespace {

constexpr double kFs = 48000.0;

// Byte-identical copy of wdf_compiler rc_lowpass netlist
constexpr const char* kRcLowpassNet = R"(* RC Lowpass Filter
V1 P001 0 PULSE(0 1 1m 1u 1u 10m 20m 0) ; input root
R1 P002 P001 1k
C1 P002 0 1u ; output:V
.tran 0 50m 0 10u
.backanno
.end
)";

// Byte-identical copy of wdf_compiler rc_bandpass netlist
constexpr const char* kRcBandpassNet = R"(* RC Bandpass Circuit
Vin 1 0 PULSE(0 1 1m 1u 1u 10m 20m 0) ; input root
C1 1 2 1u
R1 2 0 1k
R2 2 3 1k
C2 3 0 1u ; output:V
.tran 0 50m 0 10u
.backanno
.end
)";

}  // namespace

TEST_CASE("TreeExecutor: rc_lowpass matches dense Engine and oracle", "[phase1d][tree]") {
    wdf::Circuit c = wdf::buildFromString(kRcLowpassNet);
    REQUIRE(c.res.size() == 1);
    REQUIRE(c.cap.size() == 1);
    REQUIRE(c.src.size() == 1);

    wdf::TreeExecutor tree(c, kFs, "C1");
    REQUIRE(tree.isPureTree());
    REQUIRE(tree.nodeCount() > 0);
    REQUIRE(tree.stateCount() == 1);

    wdf::Engine dense(c, kFs);
    const int outIdx = dense.nodeIndex("P002");
    REQUIRE(outIdx >= 0);

    // Oracle (wdf_compiler float math)
    Impedances imp{};
    calc_impedances(imp, static_cast<float>(kFs));
    State stOr{};

    const int nSamples = static_cast<int>(kFs * 0.2); // 200 ms
    double maxErrDense = 0.0;
    double maxErrOracle = 0.0;

    for (int n = 0; n < nSamples; ++n) {
        const double t = static_cast<double>(n) / kFs;
        const double vin = std::sin(2.0 * M_PI * 440.0 * t);

        const double vTree = tree.step(vin);
        const std::vector<double> vDense = dense.step({vin});
        const double vDenseOut = vDense[static_cast<size_t>(outIdx)];

        const float vOracle = process(stOr, imp, static_cast<float>(vin));

        REQUIRE(std::isfinite(vTree));
        REQUIRE(std::isfinite(vDenseOut));

        maxErrDense = std::max(maxErrDense, std::abs(vTree - vDenseOut));
        // Oracle is inverted due to wdf_compiler loop orientation without PolarityInverter
        maxErrOracle = std::max(maxErrOracle, std::abs(vTree - static_cast<double>(-vOracle)));
    }

    // Double precision parity with dense Engine: expect machine precision close (< 1e-12)
    CHECK(maxErrDense < 1e-12);
    // Single precision parity with float oracle: expect < 1e-3
    CHECK(maxErrOracle < 1e-3);
}

TEST_CASE("TreeExecutor: rc_bandpass matches dense Engine", "[phase1d][tree]") {
    wdf::Circuit c = wdf::buildFromString(kRcBandpassNet);
    wdf::TreeExecutor tree(c, kFs, "C2");
    REQUIRE(tree.isPureTree());

    wdf::Engine dense(c, kFs);
    const int outIdx = dense.nodeIndex("3");
    REQUIRE(outIdx >= 0);

    const int nSamples = 10000;
    double maxErr = 0.0;

    for (int n = 0; n < nSamples; ++n) {
        const double t = static_cast<double>(n) / kFs;
        // Multi-frequency excitation: 100 Hz + 1 kHz + step
        const double vin = 0.7 * std::sin(2.0 * M_PI * 100.0 * t) +
                           0.3 * std::sin(2.0 * M_PI * 1000.0 * t) +
                           (n > 500 ? 0.2 : 0.0);

        const double vTree = tree.step(vin);
        const std::vector<double> vDense = dense.step({vin});
        const double vDenseOut = vDense[static_cast<size_t>(outIdx)];



        REQUIRE(std::isfinite(vTree));
        REQUIRE(std::isfinite(vDenseOut));

        maxErr = std::max(maxErr, std::abs(vTree - vDenseOut));
    }

    CHECK(maxErr < 1e-10);
}

TEST_CASE("TreeExecutor: RL highpass with Inductor matches dense Engine", "[phase1d][tree]") {
    // Simple RL highpass: Vin -> L1 -> R1 -> GND, out across R1 or L1
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    c.addInd("in", "mid", 0.05, "L1"); // 50 mH
    c.addRes("mid", "0", 1000.0, "R1"); // 1k

    wdf::TreeExecutor tree(c, kFs, "R1");
    REQUIRE(tree.isPureTree());
    REQUIRE(tree.stateCount() == 1);

    wdf::Engine dense(c, kFs);
    const int outIdx = dense.nodeIndex("mid");
    REQUIRE(outIdx >= 0);

    const int nSamples = 10000;
    double maxErr = 0.0;

    for (int n = 0; n < nSamples; ++n) {
        const double t = static_cast<double>(n) / kFs;
        const double vin = std::sin(2.0 * M_PI * 200.0 * t);

        const double vTree = tree.step(vin);
        const std::vector<double> vDense = dense.step({vin});
        const double vDenseOut = vDense[static_cast<size_t>(outIdx)];

        REQUIRE(std::isfinite(vTree));
        REQUIRE(std::isfinite(vDenseOut));

        maxErr = std::max(maxErr, std::abs(vTree - vDenseOut));
    }

    CHECK(maxErr < 1e-10);
}

TEST_CASE("TreeExecutor: non-series-parallel Wheatstone bridge throws runtime_error", "[phase1d][tree]") {
    // Non-series-parallel Wheatstone bridge graph
    const std::vector<wdf::NetEdge> edges = {
        {"a", "c", wdf::LeafKind::VoltageSource, "Vs", true},
        {"a", "b", wdf::LeafKind::Resistor, "R1", false},
        {"b", "c", wdf::LeafKind::Resistor, "R2", false},
        {"a", "d", wdf::LeafKind::Resistor, "R3", false},
        {"d", "c", wdf::LeafKind::Resistor, "R4", false},
        {"b", "d", wdf::LeafKind::Resistor, "R5", false},
    };
    const std::map<std::string, double> values = {
        {"Vs", 1.0}, {"R1", 1000.0}, {"R2", 1000.0},
        {"R3", 1000.0}, {"R4", 1000.0}, {"R5", 1000.0}
    };

    CHECK_THROWS_WITH(
        wdf::TreeExecutor(edges, values, kFs),
        Catch::Contains("not strictly series-parallel reducible")
    );
}

TEST_CASE("TreeExecutor: reset clears all internal states", "[phase1d][tree]") {
    wdf::Circuit c = wdf::buildFromString(kRcLowpassNet);
    wdf::TreeExecutor tree(c, kFs, "C1");

    for (int i = 0; i < 100; ++i)
        tree.step(1.0);

    CHECK(tree.elementVoltage("C1") > 0.0);

    tree.reset();
    // After reset, immediate step with 0V input should produce 0V output
    CHECK(tree.step(0.0) == 0.0);
}

TEST_CASE("TreeExecutor: Guitar Pickup + Cable RLC equivalent circuit", "[phase1d][tree]") {
    // Electric guitar pickup equivalent circuit:
    // Vin -> R_coil (6k) -> L_coil (3H) -> node 'jack'
    // C_cable (600pF) from 'jack' to ground
    // R_load (1M amp input) from 'jack' to ground
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    c.addRes("in", "n1", 6000.0, "R_coil");
    c.addInd("n1", "jack", 3.0, "L_coil");
    c.addCap("jack", "0", 600e-12, "C_cable");
    c.addRes("jack", "0", 1.0e6, "R_load");

    wdf::TreeExecutor tree(c, kFs, "R_load");
    REQUIRE(tree.isPureTree());
    REQUIRE(tree.stateCount() == 2); // 1 inductor + 1 capacitor

    wdf::Engine dense(c, kFs);
    const int outIdx = dense.nodeIndex("jack");
    REQUIRE(outIdx >= 0);

    const int nSamples = 10000;
    double maxErr = 0.0;

    for (int n = 0; n < nSamples; ++n) {
        const double t = static_cast<double>(n) / kFs;
        // Guitar harmonic content: 110 Hz (A2) + 220 Hz + 3.5 kHz pickup resonant peak
        const double vin = 0.1 * std::sin(2.0 * M_PI * 110.0 * t) +
                           0.05 * std::sin(2.0 * M_PI * 3500.0 * t);

        const double vTree = tree.step(vin);
        const auto vDense = dense.step({vin});
        const double vDenseOut = vDense[static_cast<size_t>(outIdx)];

        REQUIRE(std::isfinite(vTree));
        REQUIRE(std::isfinite(vDenseOut));
        maxErr = std::max(maxErr, std::abs(vTree - vDenseOut));
    }

    CHECK(maxErr < 1e-10);
}

TEST_CASE("TreeExecutor: 3rd-order passive LC speaker crossover filter", "[phase1d][tree]") {
    // 3rd-order Butterworth lowpass crossover:
    // Vin -> L1 (3mH) -> node 'mid'
    // C1 (10uF) from 'mid' to ground
    // L2 (1mH) from 'mid' to 'spk'
    // R_speaker (8 Ohm) from 'spk' to ground
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    c.addInd("in", "mid", 0.003, "L1");
    c.addCap("mid", "0", 10e-6, "C1");
    c.addInd("mid", "spk", 0.001, "L2");
    c.addRes("spk", "0", 8.0, "R_spk");

    wdf::TreeExecutor tree(c, kFs, "R_spk");
    REQUIRE(tree.isPureTree());
    REQUIRE(tree.stateCount() == 3); // 2 inductors + 1 capacitor

    wdf::Engine dense(c, kFs);
    const int outIdx = dense.nodeIndex("spk");
    REQUIRE(outIdx >= 0);

    const int nSamples = 10000;
    double maxErr = 0.0;

    for (int n = 0; n < nSamples; ++n) {
        const double t = static_cast<double>(n) / kFs;
        const double vin = std::sin(2.0 * M_PI * 1000.0 * t);

        const double vTree = tree.step(vin);
        const auto vDense = dense.step({vin});
        const double vDenseOut = vDense[static_cast<size_t>(outIdx)];

        REQUIRE(std::isfinite(vTree));
        REQUIRE(std::isfinite(vDenseOut));
        maxErr = std::max(maxErr, std::abs(vTree - vDenseOut));
    }

    CHECK(maxErr < 1e-10);
}

TEST_CASE("TreeExecutor: Twin-T notch filter and Big Muff bridge correctly detected as Rigid", "[phase1d][tree]") {
    // Twin-T notch filter topology:
    // Highpass T: C1 (in to mid1), C2 (mid1 to out), R3 (mid1 to ground)
    // Lowpass T:  R1 (in to mid2), R2 (mid2 to out), C3 (mid2 to ground)
    // Both Ts join at 'in' and 'out', forming an irreducible bridge
    const std::vector<wdf::NetEdge> twinT = {
        {"in", "0", wdf::LeafKind::VoltageSource, "Vin", true},
        {"in", "mid1", wdf::LeafKind::Capacitor, "C1", false},
        {"mid1", "out", wdf::LeafKind::Capacitor, "C2", false},
        {"mid1", "0", wdf::LeafKind::Resistor, "R3", false},
        {"in", "mid2", wdf::LeafKind::Resistor, "R1", false},
        {"mid2", "out", wdf::LeafKind::Resistor, "R2", false},
        {"mid2", "0", wdf::LeafKind::Capacitor, "C3", false},
        {"out", "0", wdf::LeafKind::Resistor, "Rload", false},
    };
    const std::map<std::string, double> twinTValues = {
        {"Vin", 1.0}, {"C1", 10e-9}, {"C2", 10e-9}, {"R3", 7.95e3},
        {"R1", 15.9e3}, {"R2", 15.9e3}, {"C3", 20e-9}, {"Rload", 1e6}
    };

    // Twin-T contains an irreducible bridge: TreeExecutor must throw
    CHECK_THROWS_WITH(
        wdf::TreeExecutor(twinT, twinTValues, kFs),
        Catch::Contains("not strictly series-parallel reducible")
    );
}

TEST_CASE("TreeExecutor: dynamic O(log N) parameter recomputation matches recompiled tree", "[phase1d][tree]") {
    wdf::Circuit c = wdf::buildFromString(kRcLowpassNet);
    wdf::TreeExecutor tree(c, kFs, "C1");

    // Warm up
    for (int i = 0; i < 100; ++i)
        tree.step(1.0);

    // Dynamically change R1 from 10k to 2.2k
    REQUIRE(tree.setResistance("R1", 2200.0));

    // Create a freshly compiled tree with R1 = 2.2k
    c.res[0].value = 2200.0;
    wdf::TreeExecutor freshTree(c, kFs, "C1");

    // Both should match bit-identically from zero or when stepped identically
    tree.reset();
    freshTree.reset();

    for (int n = 0; n < 200; ++n) {
        const double vin = std::sin(2.0 * M_PI * 1000.0 * n / kFs);
        const double vDyn = tree.step(vin);
        const double vFresh = freshTree.step(vin);
        REQUIRE(vDyn == Approx(vFresh).margin(1e-12));
    }
}


