#include <catch2.hpp>

#include <cmath>
#include <cstring>

#include "devices/CoupledInductors.h"
#include "devices/Pentode.h"
#include "devices/QuadricTriode.h"
#include "devices/Triode.h"

// Phase 1 increment 6 (step A0): the device registry.
//
// Two jobs:
//   1. the triode registry tables (`kQuadricTriodeTable`,
//      `kQuadricParallelTriodeTable`) are transcribed correctly, are usable as
//      devices, and agree with the 12AX7 datasheet at a real operating point;
//   2. the two device headers that were carried over but never included by
//      anything -- `Pentode.h`, `CoupledInductors.h` -- actually compile and
//      produce sane numbers. Before this file existed nothing forced them to
//      build.

TEST_CASE("Quadric triode tables are valid and complete")
{
    REQUIRE(wdf::quadricTriodeTablesValid());
    REQUIRE(wdf::kNumQuadricTriodes == 14);

    // Both tables carry the same 14 names in the same order, so a menu index
    // into one is a valid index into the other.
    for (int i = 0; i < wdf::kNumQuadricTriodes; ++i)
        REQUIRE(std::strcmp(wdf::kQuadricTriodeTable[i].name,
                            wdf::kQuadricParallelTriodeTable[i].name) == 0);

    // Every single-triode fit is distinct from its parallel-pair fit. If one
    // ever matches exactly, the transcription duplicated a row.
    for (int i = 0; i < wdf::kNumQuadricTriodes; ++i)
        REQUIRE(wdf::kQuadricTriodeTable[i].p.kP2 !=
                wdf::kQuadricParallelTriodeTable[i].p.kP2);
}

TEST_CASE("Quadric triode lookup by name")
{
    const auto* ax7 = wdf::quadricTriodeByName("12AX7");
    REQUIRE(ax7 != nullptr);
    // The 12AX7 entry is the DAFx-23 paper's Table 3 fit, so it must match the
    // canonical constant rather than Cathodyne's refit.
    REQUIRE(ax7->kP2 == Approx(wdf::kQuadric12AX7.kP2));
    REQUIRE(ax7->kPg == Approx(wdf::kQuadric12AX7.kPg));
    REQUIRE(ax7->kP == Approx(wdf::kQuadric12AX7.kP));

    const auto* ecc81 = wdf::quadricTriodeByName("ECC81");
    REQUIRE(ecc81 != nullptr);
    REQUIRE(ecc81->kP2 == Approx(1.429e-07));

    // The parallel pair is the same law with twice the current, so its
    // coefficients are exactly 2x the single-triode fit.
    const auto* ax7pair = wdf::quadricTriodeByName("12AX7", /*parallel*/ true);
    REQUIRE(ax7pair != nullptr);
    REQUIRE(ax7pair->kP2 == Approx(2.0 * wdf::kQuadric12AX7.kP2));
    REQUIRE(ax7pair->kPg == Approx(2.0 * wdf::kQuadric12AX7.kPg));
    REQUIRE(ax7pair->kP == Approx(2.0 * wdf::kQuadric12AX7.kP));
    REQUIRE(ax7pair->kP2 != ax7->kP2);

    // Unknown names must fail loudly rather than silently defaulting.
    CHECK(wdf::quadricTriodeByName("12BA7") == nullptr);
    CHECK(wdf::quadricTriodeByName("") == nullptr);
    CHECK(wdf::quadricTriodeByName(nullptr) == nullptr);
}

TEST_CASE("12AX7 fit reproduces the datasheet at its bias point")
{
    // Vgk = -2 V, Vpk = 250 V is the operating point the paper's Table 3 fit
    // was checked against, and it is where a 12AX7 datasheet quotes ~1.2 mA.
    // The 12AX7 entry uses that fit; check that it lands in the same place --
    // this is what proves the transcription kept kP/kP2/kPg in the right slots
    // rather than merely preserving the digits.
    const auto* p = wdf::quadricTriodeByName("12AX7");
    REQUIRE(p != nullptr);

    const auto ss = wdf::quadricSmallSignal(*p, 250.0, -2.0);
    REQUIRE(ss.ip == Approx(1.2e-3).margin(2.0e-4));  // ~1.2 mA
    REQUIRE(ss.gm > 0.0);                            // dIp/dVgk > 0
    REQUIRE(ss.gds > 0.0);                           // dIp/dVpk > 0

    // `mu = gm/gds` is *exactly* kPg/(2*kP2) for the quadric law (gm = kPg*q,
    // gds = 2*kP2*q, and q cancels), so the table's coefficients must reproduce
    // each tube's published amplification factor. This is the check that proves
    // kP/kP2/kPg landed in the right slots -- far stronger than comparing raw
    // digits, and it catches any transposed pair.
    struct KnownMu {
        const char* name;
        double published;
    };
    const KnownMu known[] = {
        {"12AX7", 100.0}, {"12AT7", 60.0}, {"12AU7", 17.5}, {"12AY7", 44.0},
    };
    for (const auto& k : known) {
        const auto* t = wdf::quadricTriodeByName(k.name);
        REQUIRE(t != nullptr);

        const auto kss = wdf::quadricSmallSignal(*t, 250.0, -2.0);
        REQUIRE(kss.gds > 0.0);
        const double mu = kss.gm / kss.gds;

        // The identity holds exactly, at any bias point.
        REQUIRE(mu == Approx(t->kPg / (2.0 * t->kP2)));
        // And it lands on the datasheet figure.
        REQUIRE(mu == Approx(k.published).margin(0.20 * k.published));
    }

    // mu is monotone across the family: 12AX7 (100) > 12AT7 (60) > 12AU7 (17.5).
    const double muAX7 = wdf::quadricTriodeByName("12AX7")->kPg / (2.0 * wdf::quadricTriodeByName("12AX7")->kP2);
    const double muAT7 = wdf::quadricTriodeByName("12AT7")->kPg / (2.0 * wdf::quadricTriodeByName("12AT7")->kP2);
    const double muAU7 = wdf::quadricTriodeByName("12AU7")->kPg / (2.0 * wdf::quadricTriodeByName("12AU7")->kP2);
    REQUIRE(muAX7 > muAT7);
    REQUIRE(muAT7 > muAU7);

    // Below cutoff the law is flat and both derivatives vanish.
    const auto off = wdf::quadricSmallSignal(*p, 250.0, -300.0);
    REQUIRE(off.ip == 0.0);
    REQUIRE(off.gm == 0.0);
    REQUIRE(off.gds == 0.0);
}

TEST_CASE("Pentode device law compiles and conducts")
{
    // Compile gate for the law and its registry, plus a bias-point sanity check.
    const auto c = wdf::kEL34.eval(-2.0, 100.0, 100.0);

    REQUIRE(c.ip > 0.0);        // plate conducts
    REQUIRE(c.is == 0.0);       // imported three-port law: no screen current
    REQUIRE(c.dip_dvgk > 0.0);  // gm > 0
    REQUIRE(c.dip_dvpk > 0.0);  // plate conductance > 0

    // The grid diode is forward-conducting when the grid goes positive. At the
    // -2 V operating point above it is fully off, so its conductance there is
    // ~1e-43 siemens -- nonzero but not a useful thing to assert on.
    const auto driven = wdf::kEL34.eval(1.0, 100.0, 100.0);
    REQUIRE(driven.ig > 0.0);
    REQUIRE(driven.dig_dvgk > 0.0);

    // Deep cutoff: the control grid is far negative, so the tube is off. The
    // softplus tail never reaches exactly zero (`base = (vg2/kp)*softplus(E1)`
    // is small but positive), so this is a magnitude check, not `== 0.0`.
    const auto off = wdf::kEL34.eval(-300.0, 100.0, 100.0);
    REQUIRE(off.ip < 1e-20);
    REQUIRE(off.is == 0.0);
    REQUIRE(off.ig <= 0.0);

    // `count` scales every current and derivative (a matched pair).
    const auto one = wdf::kEL34.eval(-2.0, 100.0, 100.0);
    const auto two = wdf::kEL34Pair.eval(-2.0, 100.0, 100.0);
    REQUIRE(two.ip == Approx(2.0 * one.ip));
    REQUIRE(two.is == Approx(2.0 * one.is));
    REQUIRE(two.dip_dvgk == Approx(2.0 * one.dip_dvgk));
}

TEST_CASE("Coupled inductors compile and stamp a valid conductance matrix")
{
    const std::array<double, 3> L { 1.0e-3, 4.0e-3, 9.0e-3 };
    const double k = 0.6;
    wdf::CoupledInductors<3> ci(L, k);
    ci.prepare(48000.0);

    for (int i = 0; i < 3; ++i) {
        REQUIRE(ci.conductance(i, i) > 0.0);  // positive diagonal
        for (int j = 0; j < 3; ++j) {
            // The matrix must be symmetric: the coupling is the same in both
            // directions, and an asymmetric stamp would break passivity.
            REQUIRE(ci.conductance(i, j) == Approx(ci.conductance(j, i)));
        }
    }

    // setDcCurrents puts the windings at their DC operating point with zero
    // port voltage: with a = R_L*i stored and b = -a reflected, the port
    // voltage (a + b)/2 must come out at exactly 0.
    const std::array<double, 3> i0 { 0.1, 0.0, -0.05 };
    ci.setDcCurrents(i0);
    for (int r = 0; r < 3; ++r) {
        const double a = ci.w[r].wdf.a;
        const double b = ci.w[r].reflected();
        REQUIRE(b == Approx(-a));
        REQUIRE(0.5 * (a + b) == Approx(0.0).margin(1e-12));
    }
}
