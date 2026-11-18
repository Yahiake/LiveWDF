#include <catch2.hpp>
#include "wdf/Engine.h"
#include "support/CircuitCases.h"

namespace {
constexpr double kFs = 48000.0;

// Minimal BJT topology (mirrors the source-held-base shape used by the BJT
// engine tests).
wdf::Circuit loneBjt() {
    wdf::Circuit c;
    c.addSrc("vcc", "0", 5.0);
    c.addRes("vcc", "c", 1000.0);
    c.addBjt("c", "vcc", "0", wdf::BjtModel(), "Q1");
    return c;
}

// Minimal true op-amp topology. A VCVS is linear and does not create a nonlinear
// root; `addOpAmp` owns the engine's three-port op-amp law.
wdf::Circuit loneOpAmp() {
    wdf::Circuit c;
    c.addSrc("vcc", "0", 12.0);
    c.addSrc("vee", "0", -12.0);
    c.addSrc("in", "0", 0.0);
    c.addRes("in", "neg", 1000.0);
    c.addRes("pos", "0", 1000.0);
    c.addOpAmp("out", "neg", "pos", "vcc", "vee", wdf::OpAmpModel(), "U1");
    c.addRes("out", "0", 1000.0);
    return c;
}
}

TEST_CASE("Root accounting and scalar solver eligibility", "[accounting]") {
    SECTION("single_ended_el34 (Pentode)") {
        const wdf::Circuit c = lwdf_test::singleEndedEl34PowerStage();
        wdf::Engine e(c, kFs);

        // Pentode has 3 root ports.
        REQUIRE(e.rootCount() == 3);

        // Pentode is not scalar-supported, so explicitRoots must be false.
        // Before the fix, it was incorrectly true.
        CHECK_FALSE(e.explicitRoots());

        // But it still solves and converges.
        std::vector<double> sv(c.src.size(), 0.0);
        sv[1] = 350.0; // bplus
        sv[2] = 300.0; // vscreen
        (void)e.step(sv);
        CHECK(e.lastConverged());
    }

    SECTION("lone_bjt (BJT)") {
        const wdf::Circuit c = loneBjt();
        wdf::Engine e(c, kFs);

        // BJT root is three ports (collector, base, emitter).
        REQUIRE(e.rootCount() == 3);

        // BJT is not scalar-supported.
        CHECK_FALSE(e.explicitRoots());

        e.reset();
        (void)e.step({5.0});
        CHECK(e.lastConverged());
    }

    SECTION("lone_opamp (Op-Amp)") {
        const wdf::Circuit c = loneOpAmp();
        wdf::Engine e(c, kFs);

        // Op-Amp has 3 root ports.
        REQUIRE(e.rootCount() == 3);

        // Op-Amp is not scalar-supported.
        CHECK_FALSE(e.explicitRoots());

        e.reset();
        std::vector<double> sv(c.src.size(), 0.0);
        (void)e.step(sv);
        CHECK(e.lastConverged());
    }

    SECTION("shared_cathode_pair (Coupled Triodes)") {
        const wdf::Circuit c = lwdf_test::sharedCathodePair();
        wdf::Engine e(c, kFs);

        // 2 triodes = 2 * 3 = 6 root ports.
        REQUIRE(e.rootCount() == 6);

        // Coupled triodes must use Newton.
        CHECK_FALSE(e.explicitRoots());
    }

    SECTION("common_cathode_bypassed (Uncoupled Triode)") {
        const wdf::Circuit c = lwdf_test::commonCathodeBypassed();
        wdf::Engine e(c, kFs);

        // 1 triode = 3 root ports.
        REQUIRE(e.rootCount() == 3);

        // Singleton triode is scalar-supported.
        CHECK(e.explicitRoots());
    }
}

// Regression for the invalid-row access that corrupted the heap: the runtime
// active-set reduction can delete rows and `redToActive_[rBase + 1]` then
// becomes -1, so the assembly must not index the matrix with it. The layout
// half is static: a triode declares its plate and internal grid as unknowns,
// whatever the grid voltage is.
TEST_CASE("Inactive triode grid rows cannot be indexed", "[layout][newton]") {
    const auto runSamples = [](wdf::Engine& e, const wdf::Circuit& c, double bias,
                               int samples) {
        e.reset();
        std::vector<double> sv(c.src.size(), 0.0);
        for (size_t s = 0; s < c.src.size(); ++s)
            sv[s] = c.src[s].dc;
        for (int n = 0; n < samples; ++n) {
            if (!sv.empty())
                sv[0] = bias;
            const auto& out = e.step(sv);
            REQUIRE(e.lastConverged());
            for (double v : out)
                REQUIRE(std::isfinite(v));
        }
    };

    SECTION("triode has a plate row and an internal grid row") {
        const wdf::Circuit c = lwdf_test::commonCathodeBypassed();
        wdf::Engine e(c, kFs);

        REQUIRE(e.rootLayout().devices.size() == 1);
        CHECK(e.rootLayout().devices[0].numActiveRows == 2);
        CHECK(e.rootLayout().nRed == 2);
        CHECK(e.rootLayout().redRow[1] == -1);
        CHECK(e.rootLayout().redRow[2] == 1);

        runSamples(e, c, 0.5, 2400);
    }

    SECTION("a biased triode keeps both rows whatever the grid's sign is") {
        wdf::Circuit c;
        c.addSrc("bias", "0", -4.0);
        c.addRes("bias", "g", 1e3);
        c.addRes("k", "0", 1e3);
        c.addRes("e", "p", 100e3);
        c.addSrc("e", "0", 250.0);
        wdf::TriodeModel model = wdf::k12ax7Quadric();
        c.addTri("g", "p", "k", "g", model);

        wdf::Engine e(c, kFs);
        REQUIRE(e.rootLayout().devices.size() == 1);
        CHECK(e.rootLayout().devices[0].numActiveRows == 2);
        CHECK(e.rootLayout().nRed == 2);

        runSamples(e, c, -4.0, 1200);
        runSamples(e, c, 4.0, 1200);
    }
}