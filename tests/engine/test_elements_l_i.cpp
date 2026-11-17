#include <catch2.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "support/NgspiceOracle.h"
#include "wdf/Engine.h"
#include "spice/SpiceNet.h"

// Phase 1 increment 4: the `L` (inductor) and `I` (ideal current source)
// elements. The inductor is the wave-dual of the capacitor (port resistance
// 2L/T, reflected wave = -state); the current source is a *fixed-current root
// port* -- an exact unit-current injection column rather than a stiff
// conductance, so it adds no parasitic load.
//
// Sign conventions are pinned against ngspice, not assumed:
//   `I1 out 0 1m` with `R1 out 0 1k` gives v(out) = -1 V.

using lwdf_test::controlBlock;
using lwdf_test::parseOpFile;
using lwdf_test::parseTranFile;
using lwdf_test::runDeck;
using lwdf_test::TranTable;
using lwdf_test::vecFile;

namespace {
constexpr double kFs = 48000.0;
constexpr double kTstep = 1.0 / kFs;
}  // namespace

TEST_CASE("inductor DC is a short and current source drives negative", "[phase1][elements]") {
    // `L1 out 0` at DC: the inductor is a short, so v(out) -> 0 while 1 mA
    // flows. The stiff-resistor model leaves a residual of kIndDcR * I.
    {
        wdf::Circuit c = wdf::buildFromString("V1 in 0 DC 1\nR1 in out 1k\nL1 out 0 1m\n");
        wdf::Engine e(c, kFs);
        REQUIRE(e.inductorCount() == 1);
        std::vector<double> node, rootV;
        REQUIRE(e.solveDc(node, rootV));
        const double vOut = node[static_cast<size_t>(e.nodeIndex("out"))];
        REQUIRE(std::abs(vOut) < 1e-5);  // 1e-4 V/ohm * 1 mA = 1e-7 V expected
    }

    // `I1 out 0 1m` pulls 1 mA out of `out`; through a 1k return path the node
    // sits at -1 V. This is the sign hangar for the whole element.
    {
        wdf::Circuit c = wdf::buildFromString("I1 out 0 1m\nR1 out 0 1k\n");
        wdf::Engine e(c, kFs);
        REQUIRE(e.currentSourceCount() == 1);
        std::vector<double> node, rootV;
        REQUIRE(e.solveDc(node, rootV));
        const double vOut = node[static_cast<size_t>(e.nodeIndex("out"))];
        REQUIRE(std::abs(vOut - (-1.0)) < 1e-9);
    }

    // Two current sources in parallel: injection is linear in the values.
    {
        wdf::Circuit c = wdf::buildFromString("I1 out 0 1m\nI2 out 0 2m\nR1 out 0 1k\n");
        wdf::Engine e(c, kFs);
        std::vector<double> node, rootV;
        REQUIRE(e.solveDc(node, rootV));
        const double vOut = node[static_cast<size_t>(e.nodeIndex("out"))];
        REQUIRE(std::abs(vOut - (-3.0)) < 1e-9);
    }
}

TEST_CASE("ngspice DC oracle: RL short and current source", "[phase1][elements][ngspice][dc]") {
    if (!lwdf_test::requireNgspice("RL/current source DC oracle")) return;
    const std::string deck =
        "* livewdf rl/isrc dc oracle\n"
        "V1 in 0 DC 1\n"
        "R1 in out 1k\n"
        "L1 out 0 1m\n"
        "I1 x 0 2m\n"
        "R2 x 0 500\n" +
        controlBlock({"op"}, {"v(out) v(x)"}, "dcli") + ".end\n";

    const auto run = runDeck(deck, "dcli");
    REQUIRE(run.ok);
    const std::vector<double> ng = parseOpFile(vecFile("dcli", 0));
    REQUIRE(ng.size() == 2);

    wdf::Circuit c =
        wdf::buildFromString("V1 in 0 DC 1\nR1 in out 1k\nL1 out 0 1m\nI1 x 0 2m\nR2 x 0 500\n");
    wdf::Engine e(c, kFs);
    std::vector<double> node, rootV;
    REQUIRE(e.solveDc(node, rootV));

    // verify.py DC threshold
    REQUIRE(std::abs(ng[0] - node[static_cast<size_t>(e.nodeIndex("out"))]) <= 5e-4);
    REQUIRE(std::abs(ng[1] - node[static_cast<size_t>(e.nodeIndex("x"))]) <= 5e-4);
}

TEST_CASE("ngspice transient oracle: RL lowpass with DC bias", "[phase1][elements][ngspice][tran]") {
    if (!lwdf_test::requireNgspice("RL lowpass oracle")) return;
    // R/(R + sL) lowpass at fc = R/(2*pi*L) = 159 Hz, with tau = L/R = 1 ms
    // (h/tau = 1/48) so the two second-order schemes agree tightly; plus a
    // 0.5 V / 5 mA DC bias so the inductor's DC state seeding is exercised (a
    // wrong seed shows up as a settling error orders of magnitude above the
    // tolerance).
    const std::string net =
        "V1 in 0 DC 0.5 SIN(0.5 0.1 159.15)\n"
        "L1 in out 100m\n"
        "R1 out 0 100\n";

    const double tstop = 0.02;  // 20 ms = 960 samples (~3 cycles)
    char tranLine[128];
    std::snprintf(tranLine, sizeof tranLine, ".tran %.17g %.17g 0 %.17g\n", kTstep, tstop, kTstep);

    const std::string deck = "* livewdf rl tran oracle\n" + std::string(tranLine) + net +
                             controlBlock({"run", "linearize v(out)"}, {"time v(out)"}, "rlt") +
                             ".end\n";

    const auto run = runDeck(deck, "rlt");
    REQUIRE(run.ok);
    const TranTable ng = parseTranFile(vecFile("rlt", 0), 1);
    REQUIRE(ng.nRows() >= 900);

    wdf::Circuit c = wdf::buildFromString(net);
    wdf::Engine e(c, kFs);
    std::vector<double> node, rootV;
    REQUIRE(e.solveDc(node, rootV));
    e.reset();  // seed states from the DC operating point (ngspice does too)
    const int outIdx = e.nodeIndex("out");
    REQUIRE(outIdx >= 0);

    double maxErr = 0.0;
    int compared = 0;
    for (size_t row = 0; row < ng.nRows(); ++row) {
        const double t = ng.time[row];
        const double vin = 0.5 + 0.1 * std::sin(2.0 * M_PI * 159.15 * t);
        const std::vector<double> vEng = e.step({vin});
        const double engOut = vEng[static_cast<size_t>(outIdx)];
        REQUIRE(std::isfinite(engOut));
        REQUIRE(e.lastConverged());
        maxErr = std::max(maxErr, std::abs(engOut - ng.cols[0][row]));
        ++compared;
    }
    REQUIRE(compared >= 900);
    WARN("ngspice RL tran oracle: maxErr=" << maxErr << " over " << compared << " samples");
    REQUIRE(maxErr <= 2e-4);
}

TEST_CASE("ngspice transient oracle: current source into RC", "[phase1][elements][ngspice][tran]") {
    if (!lwdf_test::requireNgspice("current source RC oracle")) return;
    // Current sources are driven through the extra `sourceValues` slots (after
    // the nsrc voltage-source values); this checks the per-sample path.
    // tau = RC = 1 ms, omega*tau = 1 at 159.15 Hz: the capacitor is fully
    // engaged while h/tau = 1/48 keeps the scheme scatter small.
    const std::string net =
        "I1 out 0 SIN(0 1m 159.15)\n"
        "R1 out 0 1k\n"
        "C1 out 0 1u\n";

    const double tstop = 0.02;  // 20 ms
    char tranLine[128];
    std::snprintf(tranLine, sizeof tranLine, ".tran %.17g %.17g 0 %.17g\n", kTstep, tstop, kTstep);

    const std::string deck = "* livewdf isrc tran oracle\n" + std::string(tranLine) + net +
                             controlBlock({"run", "linearize v(out)"}, {"time v(out)"}, "it") +
                             ".end\n";

    const auto run = runDeck(deck, "it");
    REQUIRE(run.ok);
    const TranTable ng = parseTranFile(vecFile("it", 0), 1);
    REQUIRE(ng.nRows() >= 900);

    wdf::Circuit c = wdf::buildFromString(net);
    wdf::Engine e(c, kFs);
    REQUIRE(e.sourceCount() == 0);
    REQUIRE(e.currentSourceCount() == 1);
    e.reset();

    const int outIdx = e.nodeIndex("out");
    REQUIRE(outIdx >= 0);

    double maxErr = 0.0;
    int compared = 0;
    for (size_t row = 0; row < ng.nRows(); ++row) {
        const double t = ng.time[row];
        const double iSrc = 1e-3 * std::sin(2.0 * M_PI * 159.15 * t);
        const std::vector<double> vEng = e.step({iSrc});
        const double engOut = vEng[static_cast<size_t>(outIdx)];
        REQUIRE(std::isfinite(engOut));
        maxErr = std::max(maxErr, std::abs(engOut - ng.cols[0][row]));
        ++compared;
    }
    REQUIRE(compared >= 900);
    WARN("ngspice isrc tran oracle: maxErr=" << maxErr << " over " << compared << " samples");
    REQUIRE(maxErr <= 2e-4);
}
