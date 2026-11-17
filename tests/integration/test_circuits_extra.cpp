#include <catch2.hpp>

#include <cmath>
#include <string>
#include <vector>
#include <crtdbg.h>

#include "wdf/Engine.h"
#include "support/CircuitCases.h"

// Coverage for the shared `CircuitCases` fixtures.
//
// Why these exist: `rc_lowpass` is N=1, P=2, K=0 -- it has no nonlinear root, no
// Newton, and nothing worth calling a network, so it cannot support a decision
// about the compiler or the engine. Every fixture here must build, reach a DC
// operating point, and run a few thousand samples with every one converging.
// Two multi-device shapes are carried because they are opposites: the cascade
// has two roots that *can* be split (the coupling enters a zero-current grid, so
// the closed forms run in sequence), while the shared-cathode pair has two roots
// that cannot (the coupling enters a current-carrying cathode node), so it is the
// one that prices the joint multiport Newton.

namespace {
constexpr double kFs = 48000.0;
constexpr int kSamples = 4800;
}  // namespace

TEST_CASE("every fixture solves DC and runs every sample converged", "[circuits]") {
    for (const auto& tc : lwdf_test::circuitCases()) {
        CAPTURE(tc.name);

        const wdf::Circuit c = (*tc.build)();
        wdf::Engine e(c, kFs);

        std::vector<double> node, rootV;
        REQUIRE(e.solveDc(node, rootV));

        e.reset();

        std::vector<double> out;
        for (int n = 0; n < kSamples; ++n) {
            out = e.step(lwdf_test::sampleSources(c, tc, n));
            REQUIRE(e.lastConverged());
        }
    }
}

TEST_CASE("fixture shapes are what the note says they are", "[circuits]") {
    // The counts are asserted so the fixtures cannot drift into something other
    // than the topology the comment describes -- a wrong fixture is worse than
    // a missing one, because every measurement built on it inherits the error.
    struct Shape {
        const char* name;
        int nodes, elements, roots;
        bool explicitRoot;
    };
    const Shape expected[] = {
        {"rc_lowpass", 1, 2, 0, false},
        {"rl_highpass", 1, 2, 0, false},
        {"common_cathode_bypassed", 5, 8, 3, true},
        {"common_cathode_unbypassed", 5, 7, 3, true},
        {"cathode_follower", 4, 5, 3, true},
        {"two_stage_cascade", 7, 12, 6, true},
        {"four_stage_preamp", 13, 21, 12, true},
        {"shared_cathode_pair", 6, 8, 6, false},
        {"jfet_common_source", 5, 10, 2, true},
        // The clipper is the shape the note claims: `out` is the only unknown
        // node (`in` is driven), its two passive elements are the `P_` count, and
        // the anti-parallel pair is two root ports. The explicit flag is `false`
        // even though each diode has a closed form, because the two ports span the
        // same node pair: each one's coupling to the other is the *same* `Z`
        // entry, so both directions are live, no order exists, and the joint
        // Newton has to run. `diode_single` below is the one-diode control.
        {"diode_clipper", 1, 2, 2, false},
        // One diode, so one root port and nothing to couple to: the group solve
        // takes it and the closed form is `true`.
        {"diode_single", 1, 2, 1, true},
        {"fender_tone_stack", 5, 11, 0, false},
        {"big_muff_tone_stack", 4, 8, 0, false},
        {"twin_t_notch_filter", 4, 9, 0, false},
        {"tube_screamer_clipping", 3, 6, 2, false},
        {"single_ended_el34", 4, 7, 3, false},
        {"guitar_pickup_cable", 4, 10, 0, false},
        {"baxandall_active_tone", 4, 10, 0, false},
        {"three_way_crossover", 4, 11, 0, false},
        {"long_tailed_pair", 8, 12, 6, false},
        {"asymmetric_diode_clipper", 1, 3, 2, false},
        {"diode_bridge_rectifier", 3, 6, 4, false},
        {"push_pull_power_stage", 8, 15, 6, false},
    };

    const auto& cases = lwdf_test::circuitCases();
    REQUIRE(cases.size() == sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0; i < cases.size(); ++i) {
        CAPTURE(cases[i].name);
        REQUIRE(std::string(expected[i].name) == cases[i].name);

        wdf::Engine e((*cases[i].build)(), kFs);
        REQUIRE(e.nodeCount() == expected[i].nodes);
        REQUIRE(e.elementCount() == expected[i].elements);
        REQUIRE(e.rootCount() == expected[i].roots);
        REQUIRE(e.explicitRoots() == expected[i].explicitRoot);
    }
}
