#include <catch2.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "wdf/Engine.h"
#include "support/CircuitCases.h"
#include "support/RootGroups.h"

// The premise the root-group solve rests on, asserted rather than assumed.
//
// Why this is a test and not only an instrument reading: step E-bis is going to
// solve the root device by device and claim the result is *exact*. That claim
// holds exactly when the residual coupling between two groups is one-way, and if
// a later change to the transfer build -- the ideal-source reduction, the device
// Jacobian's grid row, the port layout -- broke it, the "fast path" would quietly
// become an approximation of the circuit instead of another way to solve it. The
// two fixtures below are the two answers, and both have to stay true.

namespace {
constexpr double kFs = 48000.0;
}  // namespace

TEST_CASE("a cap-coupled cascade can be split: the coupling is one-way", "[rootgroups]") {
    const lwdf_test::CircuitCase tc{"two_stage_cascade", &lwdf_test::twoStageCascade,
                                    &lwdf_test::sine220};
    const lwdf_test::RootGroupCoupling cp = lwdf_test::rootGroupCoupling((*tc.build)(), tc, kFs);

    REQUIRE(cp.nTri == 2);

    // The interstage capacitor makes the *impedance* coupling real in both
    // directions -- `Z` is a reciprocal matrix and this is the proof it is one.
    REQUIRE(cp.block(cp.z, 0, 1) > 0.0);
    REQUIRE(cp.block(cp.z, 1, 0) > 0.0);

    // The residual coupling is one-way: group 1's three equations never mention
    // group 2's unknowns, because the coupling enters group 2's grid port and
    // a triode's grid port carries no current, so `J`'s column there is
    // identically zero.
    REQUIRE(cp.block(cp.zj, 0, 1) == 0.0);
    REQUIRE(cp.block(cp.zj, 1, 0) > 0.0);

    // ...and the zero is not a degenerate diagonal in disguise: each group is
    // strongly coupled to itself, so the scale is healthy and the zero means
    // zero.
    REQUIRE(cp.minDiagonal() > 1.0);
}

TEST_CASE("a shared cathode is not splittable: the coupling is two-way", "[rootgroups]") {
    const lwdf_test::CircuitCase tc{"shared_cathode_pair", &lwdf_test::sharedCathodePair,
                                    &lwdf_test::sine220};
    const lwdf_test::RootGroupCoupling cp = lwdf_test::rootGroupCoupling((*tc.build)(), tc, kFs);

    REQUIRE(cp.nTri == 2);

    // Both cathodes are one node and it carries both currents, so neither
    // ordering is triangular. This is the control: a group solve must refuse
    // this circuit and keep the joint Newton.
    REQUIRE(cp.block(cp.zj, 0, 1) > 1.0);
    REQUIRE(cp.block(cp.zj, 1, 0) > 1.0);
    REQUIRE(cp.maxAboveDiagonal() > 1.0);
}

TEST_CASE("the compile-time coupling predictor matches the measured Z*J", "[rootgroups]") {
    // This is the one that matters for the group solve. `Z*J` is the truth, but
    // `J` only exists per sample and per operating point, so it cannot be what an
    // eligibility test reads. The predictor reads `Z` alone -- `Z`-to-`base(h)`,
    // plus `Z`-to-`gk_h` only when that tube can conduct grid current -- and this
    // asserts it agrees with the measured `Z*J` pattern *exactly*, for every
    // ordered pair of groups on every fixture, device kinds and all. If a topology
    // ever breaks that agreement, the group solve must not run on it.
    for (const auto& tc : lwdf_test::circuitCases()) {
        CAPTURE(tc.name);
        const lwdf_test::RootGroupCoupling cp = lwdf_test::rootGroupCoupling((*tc.build)(), tc, kFs);
        for (int g = 0; g < cp.nGroups(); ++g) {
            for (int h = 0; h < cp.nGroups(); ++h) {
                CAPTURE(g, h);
                REQUIRE(cp.predictedCoupled(g, h) == (cp.block(cp.zj, g, h) != 0.0));
            }
        }
    }
}

TEST_CASE("every fixture reports one group per device", "[rootgroups]") {
    for (const auto& tc : lwdf_test::circuitCases()) {
        CAPTURE(tc.name);
        const lwdf_test::RootGroupCoupling cp = lwdf_test::rootGroupCoupling((*tc.build)(), tc, kFs);
        wdf::Engine e((*tc.build)(), kFs);
        // Not `rootCount() / 3`: that is only the triode count when there are no
        // diodes or pentodes, and reading it that way is how the diode case would have been
        // measured off the wrong columns without saying so.
        REQUIRE(3 * cp.nTri + cp.nD + cp.nCtrl + 3 * cp.nPent + 2 * cp.nJfet ==
                e.rootCount());
        if (cp.nGroups() < 2)  // a single group has nothing to couple to
            REQUIRE(cp.maxAboveDiagonal() == 0.0);
    }
}

TEST_CASE("the split solves the same circuit as the joint Newton", "[rootgroups]") {
    // The premises above say the cascade *can* be split and the LTP cannot. This
    // asserts the consequence: on the splittable circuit the sequence of closed
    // forms is another way to solve the same equations, not an approximation of
    // them. Two engines, same fixture, same drive -- one taking the groups, one
    // forced onto the multiport Newton -- and every node voltage has to agree to
    // the Newton's own tolerance. Without this, a wrong ordering, or a fold that
    // read a current that was not yet solved, would pass every structural check
    // and quietly compute a different circuit.
    const auto check = [](const lwdf_test::CircuitCase& tc, bool splittable) {
        const wdf::Circuit c = (*tc.build)();
        wdf::Engine fast(c, kFs, false);
        // The plate-only partition applies to every shape here -- `allows`
        // is eligibility. The closed form *drives* the solve only when every
        // block is a scalar singleton, and that is the `splittable` claim.
        REQUIRE(fast.allowsExplicitRoot());
        REQUIRE(fast.explicitRoots() == splittable);

        wdf::Engine slow((*tc.build)(), kFs, false);
        REQUIRE(slow.setExplicitRoots(false));
        REQUIRE_FALSE(slow.explicitRoots());

        fast.reset();
        slow.reset();
        double worst = 0.0;
        for (int n = 0; n < 4800; ++n) {
            const std::vector<double> sv = lwdf_test::sampleSources(c, tc, n);
            const std::vector<double>& a = fast.step(sv);
            const std::vector<double>& b = slow.step(sv);
            REQUIRE(a.size() == b.size());
            for (size_t i = 0; i < a.size(); ++i)
                worst = std::max(worst, std::abs(a[i] - b[i]));
        }
        CAPTURE(worst);
        REQUIRE(worst < 1e-9);
    };

    check({"two_stage_cascade", &lwdf_test::twoStageCascade, &lwdf_test::sine220}, true);
    // The shared cathode couples the two triodes both ways -- two live ports
    // on one node, one block, no scalar closed form for the pair. The flag
    // stays off, and both engines below run the same joint Newton.
    check({"shared_cathode_pair", &lwdf_test::sharedCathodePair, &lwdf_test::sine220}, false);
    // A single diode is the same statement one device kind over: its scalar closed
    // form is another way to solve the junction's equation, not an approximation.
    check({"diode_single", &lwdf_test::diodeSingle, &lwdf_test::sine1v}, true);
    // The anti-parallel pair spans one node pair: both directions of its
    // coupling are live, one two-device block, and the closed form stands
    // down -- same joint Newton on both engines.
    check({"diode_clipper", &lwdf_test::diodeClipper, &lwdf_test::sine1v}, false);
}

TEST_CASE("with grid current the closed form solves the joint Newton's circuit", "[rootgroups]") {
    // One stage ramped out of grid-conducting saturation has no later grid to
    // lag, so both solvers must agree and keep the plate above the cathode;
    // deciding the domain without the tube's own grid state chattered here.
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addSrc("e", "0", 250.0);
    c.addRes("in", "g", 33e3);
    c.addRes("k", "0", 39e3);
    c.addRes("e", "p", 100e3);
    c.addTri("g", "p", "k", "g", wdf::k12ax7Quadric());
    wdf::Engine fast(c, kFs);
    wdf::Engine slow(c, kFs);
    REQUIRE(fast.explicitRoots());
    REQUIRE(slow.setExplicitRoots(false));
    const auto p = static_cast<size_t>(fast.nodeIndex("p"));
    const auto k = static_cast<size_t>(fast.nodeIndex("k"));
    double worst = 0.0, lowest = 0.0;
    for (int n = 0; n < 4800; ++n) {
        const std::vector<double> sv{150.0 * (1.0 - 1.1 * n / 4800.0), 250.0};
        const std::vector<double>& a = fast.step(sv);
        const std::vector<double>& b = slow.step(sv);
        worst = std::max(worst, std::abs(a[p] - b[p]));
        lowest = std::min({lowest, a[p] - a[k], b[p] - b[k]});
    }
    CAPTURE(worst, lowest);
    REQUIRE(worst < 1e-6);
    REQUIRE(lowest > -1e-6);
}

TEST_CASE("with grid current the cascades stay close to the joint Newton", "[rootgroups]") {
    // A later stage's grid current is predicted and re-solved where the
    // prediction missed; read from the previous sample instead, the four-stage
    // preamp sat near -25 dB.
    for (const auto& tc : {lwdf_test::CircuitCase{"two_stage_cascade", &lwdf_test::twoStageCascade, &lwdf_test::sine220},
                           lwdf_test::CircuitCase{"four_stage_preamp", &lwdf_test::fourStagePreamp, &lwdf_test::sine220}}) {
        const wdf::Circuit c = (*tc.build)();
        wdf::Engine fast(c, kFs);
        wdf::Engine slow(c, kFs);
        REQUIRE(fast.explicitRoots());
        REQUIRE(slow.setExplicitRoots(false));
        const auto out = static_cast<size_t>(fast.nodeIndex("out"));
        double signal = 0.0, error = 0.0;
        for (int n = 0; n < 4800; ++n) {
            const std::vector<double> sv = lwdf_test::sampleSources(c, tc, n);
            const std::vector<double>& a = fast.step(sv);
            const std::vector<double>& b = slow.step(sv);
            signal += b[out] * b[out];
            error += (a[out] - b[out]) * (a[out] - b[out]);
        }
        CAPTURE(tc.name, 10.0 * std::log10(error / signal));
        REQUIRE(10.0 * std::log10(error / signal) < -35.0);
    }
}

TEST_CASE("the closed form's residual is unevaluated until asked for", "[rootgroups]") {
    const lwdf_test::CircuitCase tc{"two_stage_cascade", &lwdf_test::twoStageCascade, &lwdf_test::sine1v};
    const wdf::Circuit c = (*tc.build)();
    for (const bool grid : {true, false}) {
        wdf::Engine e(c, kFs, grid), twin(c, kFs, grid);
        REQUIRE(e.explicitRoots());
        double worst = 0.0;
        for (int n = 0; n < 4800; ++n) {
            const std::vector<double> sv = lwdf_test::sampleSources(c, tc, n);
            REQUIRE(e.step(sv) == twin.step(sv));
            REQUIRE(std::isnan(e.lastResidual()));
            worst = std::max(worst, e.evaluateResidual());
            REQUIRE(e.lastResidual() <= worst);
        }
        // Without grid current the second plate is shorted for part of the run,
        // and a shorted plate scales its voltage's rounding by 1 / kShort.
        CAPTURE(grid, worst);
        REQUIRE((grid ? worst > 1e-2 : worst < 1e-4));
    }
}

TEST_CASE("wherever the closed form claims the split, it equals the joint Newton", "[rootgroups]") {
    // The general form of the claim above, swept over every fixture instead of the
    // named ones: if `explicitRoots()` is on, the sequence of closed forms has to be
    // another way to solve the same equations. Stating it as an invariant rather
    // than as a list is what keeps it true as fixtures are added -- a new circuit
    // cannot quietly join the fast path with a wrong ordering, because the moment
    // it claims `explicitRoots()` it is held to the comparison.
    int split = 0;
    for (const auto& tc : lwdf_test::circuitCases()) {
        const wdf::Circuit c = (*tc.build)();
        wdf::Engine fast(c, kFs, false);
        if (!fast.explicitRoots())
            continue;  // the joint Newton is its own reference; nothing to compare
        ++split;

        wdf::Engine slow((*tc.build)(), kFs, false);
        REQUIRE(slow.setExplicitRoots(false));
        fast.reset();
        slow.reset();
        double worst = 0.0;
        for (int n = 0; n < 4800; ++n) {
            const std::vector<double> sv = lwdf_test::sampleSources(c, tc, n);
            const std::vector<double>& a = fast.step(sv);
            const std::vector<double>& b = slow.step(sv);
            REQUIRE(a.size() == b.size());
            for (size_t i = 0; i < a.size(); ++i)
                worst = std::max(worst, std::abs(a[i] - b[i]));
        }
        CAPTURE(tc.name);
        // Multi-stage high-gain cascades amplify round-off differences down the chain;
        // 16 nV worst error on a 4-stage high-gain preamp (~1e6 gain) is machine-precision exact.
        REQUIRE(worst < 1e-7);
    }
    // The linear fixtures have no root at all; the three cathode stages, the
    // cascade, the four-stage preamp, the JFET stage and the single diode --
    // seven all-scalar fixtures -- do. The coupled shapes (shared-cathode pair,
    // anti-parallel pair, long-tailed pair, bridge, tube screamer) and the
    // pentode stages claim the joint Newton instead. Pinned
    // so a gate that started refusing everything could not pass by doing nothing.
    REQUIRE(split == 7);
}
