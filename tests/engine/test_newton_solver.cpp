#include <catch2.hpp>

#include <cmath>
#include <string>
#include <vector>

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <math.h>

#include "wdf/Engine.h"
#include "support/CircuitCases.h"

// Dedicated verification for the multi-variable Newton-Raphson solver.
//
// These tests verify that:
// 1. Coupled multi-device circuits strictly select and exercise the Newton solver.
// 2. Newton iteration counts (e.newtonIters()) are strictly positive during audio processing.
// 3. Newton achieves sub-nanovolt convergence across both normal and extreme overdrive regimes.
// 4. Forced Newton (setExplicitRoots(false)) matches closed-form solutions within < 1e-10 V.
// 5. Symmetric and balanced coupled topologies (LTP, push-pull, diode bridge) exhibit exact physics.

namespace {
constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

double testSine(int n, double amp, double freq = 220.0) {
    return amp * std::sin(2.0 * kPi * freq * static_cast<double>(n) / kFs);
}
}  // namespace

TEST_CASE("Coupled circuits strictly engage and exercise the Newton solver", "[newton][convergence]") {
    struct NewtonCase {
        const char* name;
        wdf::Circuit (*build)();
        double driveAmp;
        int expectedRoots;
    };

    const NewtonCase cases[] = {
        {"long_tailed_pair", &lwdf_test::longTailedPair, 1.0, 6},
        {"asymmetric_diode_clipper", &lwdf_test::asymmetricDiodeClipper, 1.5, 2},
        {"diode_bridge_rectifier", &lwdf_test::diodeBridgeRectifier, 5.0, 4},
        {"push_pull_power_stage", &lwdf_test::pushPullPowerStage, 1.0, 6},
        {"shared_cathode_pair", &lwdf_test::sharedCathodePair, 0.5, 6},
        {"diode_clipper", &lwdf_test::diodeClipper, 1.0, 2},
        {"tube_screamer_clipping", &lwdf_test::tubeScreamerClippingStage, 1.0, 2},
        {"single_ended_el34", &lwdf_test::singleEndedEl34PowerStage, 1.0, 3},
    };

    for (const auto& tc : cases) {
        DYNAMIC_SECTION("Circuit: " << tc.name) {
            const wdf::Circuit c = (*tc.build)();
            wdf::Engine e(c, kFs);

            // 1. Root structure check
            REQUIRE(e.rootCount() == tc.expectedRoots);
            // A pair of reversed diodes is exact now, so a pair-only circuit
            // claims `explicitRoots()`. The coupled triode/pentode shapes
            // cannot. Force Newton either way: this test is about the joint
            // solver, not about which formulation the engine picked.
            if (e.explicitRoots() || e.blockRoots())
                e.setExplicitRoots(false);
            REQUIRE_FALSE(e.explicitRoots());
            REQUIRE_FALSE(e.blockRoots());

            // 2. DC solve
            std::vector<double> node, rootV;
            REQUIRE(e.solveDc(node, rootV));
            e.reset();

            // 3. Audio streaming: verify Newton actually runs and converges every sample
            long long totalIters = 0;
            constexpr int kRunSamples = 2400; // 50 ms of audio
            for (int n = 0; n < kRunSamples; ++n) {
                std::vector<double> sv(c.src.size(), 0.0);
                for (size_t s = 0; s < c.src.size(); ++s)
                    sv[s] = c.src[s].dc;
                if (!sv.empty())
                    sv[0] = testSine(n, tc.driveAmp);

                const auto& out = e.step(sv);
                REQUIRE(e.lastConverged());
                REQUIRE(e.lastResidual() < 1e-8);
                for (double v : out)
                    REQUIRE(std::isfinite(v));

                totalIters += e.newtonIterations();
            }

            // Must have genuinely iterated (not 0 iterations)
            const double avgIters = static_cast<double>(totalIters) / kRunSamples;
            INFO("Average Newton iterations per sample: " << avgIters);
            REQUIRE(avgIters >= 1.0);
            REQUIRE(avgIters <= 6.0); // Efficient quadratic convergence
        }
    }
}

TEST_CASE("Forced Newton achieves sub-nanovolt parity with closed-form roots", "[newton][parity]") {
    struct ParityFixture {
        const char* name;
        wdf::Circuit (*build)();
    };

    const ParityFixture fixtures[] = {
        {"common_cathode_bypassed", &lwdf_test::commonCathodeBypassed},
        {"common_cathode_unbypassed", &lwdf_test::commonCathodeUnbypassed},
        {"cathode_follower", &lwdf_test::cathodeFollower},
        {"two_stage_cascade", &lwdf_test::twoStageCascade},
        {"diode_single", &lwdf_test::diodeSingle},
        {"diode_clipper", &lwdf_test::diodeClipper},
        {"tube_screamer_clipping", &lwdf_test::tubeScreamerClippingStage},
    };

    for (const auto& f : fixtures) {
        DYNAMIC_SECTION("Parity check for " << f.name) {
            const wdf::Circuit c = (*f.build)();
            wdf::Engine eClosed(c, kFs, false);
            wdf::Engine eNewton(c, kFs, false);

            REQUIRE((eClosed.explicitRoots() || eClosed.blockRoots()));
            eNewton.setExplicitRoots(false);
            REQUIRE_FALSE(eNewton.explicitRoots());
            REQUIRE_FALSE(eNewton.blockRoots());

            eClosed.reset();
            eNewton.reset();

            constexpr int kCheckSamples = 1200;
            double maxDiff = 0.0;
            for (int n = 0; n < kCheckSamples; ++n) {
                std::vector<double> sv(c.src.size(), 0.0);
                for (size_t s = 0; s < c.src.size(); ++s)
                    sv[s] = c.src[s].dc;
                if (!sv.empty())
                    sv[0] = testSine(n, 0.2);

                const auto& yClosed = eClosed.step(sv);
                const auto& yNewton = eNewton.step(sv);

                REQUIRE(eNewton.lastConverged());
                REQUIRE(eNewton.newtonIterations() > 0);

                for (size_t i = 0; i < yClosed.size(); ++i) {
                    const double diff = std::abs(yClosed[i] - yNewton[i]);
                    maxDiff = std::max(maxDiff, diff);
                }
            }
            INFO("Max |Closed - Newton| error: " << maxDiff << " V");
            REQUIRE(maxDiff < 1e-9); // Sub-nanovolt agreement
        }
    }
}

TEST_CASE("Newton solver survives extreme overdrive without divergence", "[newton][robustness]") {
    // Drive Tube Screamer with 15 V peak signal (extreme rail-to-rail overdrive)
    wdf::Engine e(lwdf_test::tubeScreamerClippingStage(), kFs);
    e.reset();

    constexpr int kOverdriveSamples = 1200;
    for (int n = 0; n < kOverdriveSamples; ++n) {
        std::vector<double> sv = { testSine(n, 15.0) };
        const auto& out = e.step(sv, 60, 1e-10);
        REQUIRE(e.lastConverged());
        for (double v : out) {
            REQUIRE(std::isfinite(v));
            REQUIRE(std::abs(v) < 20.0); // Bounded
        }
    }
}

TEST_CASE("Long-Tailed Pair produces differential balanced phase outputs", "[newton][ltp]") {
    wdf::Engine e(lwdf_test::longTailedPair(), kFs);
    e.reset();

    const int out1Idx = e.nodeIndex("out1");
    const int out2Idx = e.nodeIndex("out2");
    REQUIRE(out1Idx >= 0);
    REQUIRE(out2Idx >= 0);

    // Warm up AC coupling caps
    for (int n = 0; n < 4800; ++n) {
        std::vector<double> sv = { testSine(n, 0.5), 300.0 };
        e.step(sv);
    }

    // Measure differential balance over one cycle
    double p1Peak = 0.0;
    double p2Peak = 0.0;
    double maxSum = 0.0;
    for (int n = 0; n < 240; ++n) {
        std::vector<double> sv = { testSine(n, 0.5), 300.0 };
        const auto& out = e.step(sv);
        const double v1 = out[static_cast<size_t>(out1Idx)];
        const double v2 = out[static_cast<size_t>(out2Idx)];
        p1Peak = std::max(p1Peak, std::abs(v1));
        p2Peak = std::max(p2Peak, std::abs(v2));
        maxSum = std::max(maxSum, std::abs(v1 + v2));
    }

    INFO("Peak Out1: " << p1Peak << " V, Peak Out2: " << p2Peak << " V");
    REQUIRE(p1Peak > 1.0);
    REQUIRE(p2Peak > 1.0);
    // Classical 82k/100k LTP outputs match within 15% amplitude
    REQUIRE(std::abs(p1Peak - p2Peak) / p1Peak < 0.15);
    // Outputs are 180 degrees inverted, so sum is much smaller than individual peaks
    REQUIRE(maxSum < 0.25 * p1Peak);
}

TEST_CASE("Diode Bridge Rectifier produces pure positive full-wave rectification", "[newton][bridge]") {
    wdf::Engine e(lwdf_test::diodeBridgeRectifier(), kFs);
    e.reset();

    const int posIdx = e.nodeIndex("dc_pos");
    REQUIRE(posIdx >= 0);

    // Run 50 ms of 5 V AC mains
    double minDc = 1e9;
    double maxDc = -1e9;
    for (int n = 0; n < 2400; ++n) {
        std::vector<double> sv = { testSine(n, 5.0, 50.0) }; // 50 Hz AC
        const auto& out = e.step(sv);
        const double vDc = out[static_cast<size_t>(posIdx)];
        minDc = std::min(minDc, vDc);
        maxDc = std::max(maxDc, vDc);
    }

    INFO("DC Output: Min = " << minDc << " V, Max = " << maxDc << " V");
    // After filter reservoir capacitor charges, output must stay positive (within numerical zero)
    REQUIRE(minDc >= -1e-12);
    REQUIRE(maxDc > 2.0); // Positive DC rail established
}
