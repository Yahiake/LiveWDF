#include <catch2.hpp>

#include <cmath>
#include <vector>

#include "support/CircuitCases.h"
#include "wdf/Engine.h"

// Permanent extended portfolio: each fixture is here for a reason documented
// next to its builder in support/CircuitCases.h. The contracts checked here are
// deliberately basic but strict: the circuit must compile, seed from DC, stay
// finite under its own drive, and any closed-form fast path must agree with the
// joint Newton reference on the same circuit.

namespace {
constexpr double kFs = 48000.0;
constexpr int kSamples = 4800;
}  // namespace

TEST_CASE("extended fixtures solve, stay finite, and keep path parity", "[extendedfixtures]") {
    for (const auto& tc : lwdf_test::extendedCircuitCases()) {
        CAPTURE(tc.name);
        const wdf::Circuit c = (*tc.build)();

        wdf::Engine fast(c, kFs, false);
        REQUIRE(fast.nodeCount() > 0);
        fast.reset();

        int nonConverged = 0;
        for (int n = 0; n < kSamples; ++n) {
            const std::vector<double> sv = lwdf_test::sampleSources(c, tc, n);
            const std::vector<double>& y = fast.step(sv);
            REQUIRE(y.size() == static_cast<size_t>(fast.nodeCount()));
            for (size_t i = 0; i < y.size(); ++i)
                REQUIRE(std::isfinite(y[i]));
            if (!fast.lastConverged())
                ++nonConverged;
        }
        // A closed form converges by contract; the joint path must also settle
        // on a fixture this simple. Allow a handful of guarded rejections.
        REQUIRE(nonConverged < kSamples / 100);

        if (fast.explicitRoots()) {
            wdf::Engine slow((*tc.build)(), kFs, false);
            REQUIRE(slow.setExplicitRoots(false));
            fast.reset();
            slow.reset();
            double worst = 0.0;
            for (int n = 0; n < kSamples; ++n) {
                const std::vector<double> sv = lwdf_test::sampleSources(c, tc, n);
                const auto& a = fast.step(sv);
                const auto& b = slow.step(sv);
                REQUIRE(a.size() == b.size());
                for (size_t i = 0; i < a.size(); ++i)
                    worst = std::max(worst, std::abs(a[i] - b[i]));
            }
            CAPTURE(worst);
            REQUIRE(worst < 1e-7);
        }
    }
}

TEST_CASE("a floating resistor island is refused, not silently grounded", "[extendedfixtures]") {
    const wdf::Circuit c = lwdf_test::floatingIsland();
    REQUIRE_THROWS_AS(wdf::Engine(c, kFs), std::runtime_error);
}
