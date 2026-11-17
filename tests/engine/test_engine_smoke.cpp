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
