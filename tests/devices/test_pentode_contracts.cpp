#include <catch2.hpp>

#include <cmath>
#include <initializer_list>
#include <vector>

#include "devices/Pentode.h"

// Property guards for the Reefman pentode constitutive evaluator.
// The solver trusts these analytic derivatives; this test checks them against
// central differences at deterministic, physically valid operating points.
// Deterministic points (not random) keep failures reproducible from a SHA.

namespace {
constexpr double kRelTol = 2e-4;

void checkDerivative(const wdf::PentodeCurrents& base,
                     const wdf::ReefmanPentode& m,
                     double vgk, double vsk, double vpk,
                     int which) {
    const double h = 1e-4 * std::max({1.0, std::abs(vgk), std::abs(vsk), std::abs(vpk)});
    double analytic = 0.0;
    double plus = 0.0, minus = 0.0;
    switch (which) {
        case 0:  // d/dvgk
            analytic = base.dip_dvgk;
            plus = m.eval(vgk + h, vsk, vpk).ip;
            minus = m.eval(vgk - h, vsk, vpk).ip;
            break;
        case 1:  // d/dvsk
            analytic = base.dip_dvsk;
            plus = m.eval(vgk, vsk + h, vpk).ip;
            minus = m.eval(vgk, vsk - h, vpk).ip;
            break;
        case 2:  // d/dvpk
            analytic = base.dip_dvpk;
            plus = m.eval(vgk, vsk, vpk + h).ip;
            minus = m.eval(vgk, vsk, vpk - h).ip;
            break;
        case 3:
            analytic = base.dis_dvgk;
            plus = m.eval(vgk + h, vsk, vpk).is;
            minus = m.eval(vgk - h, vsk, vpk).is;
            break;
        case 4:
            analytic = base.dis_dvsk;
            plus = m.eval(vgk, vsk + h, vpk).is;
            minus = m.eval(vgk, vsk - h, vpk).is;
            break;
        case 5:
            analytic = base.dis_dvpk;
            plus = m.eval(vgk, vsk, vpk + h).is;
            minus = m.eval(vgk, vsk, vpk - h).is;
            break;
        default:
            analytic = base.dig_dvgk;
            plus = m.eval(vgk + h, vsk, vpk).ig;
            minus = m.eval(vgk - h, vsk, vpk).ig;
            break;
    }
    const double fd = (plus - minus) / (2.0 * h);
    REQUIRE(std::isfinite(fd));
    REQUIRE(std::abs(fd - analytic) <= kRelTol * (1.0 + std::abs(analytic)));
}
}  // namespace

TEST_CASE("pentode analytic derivatives match central differences", "[pentodecontracts]") {
    const std::vector<wdf::ReefmanPentode> models = {
        wdf::ReefmanPentode::el34(),
        wdf::ReefmanPentode::sixL6GC(),
        wdf::ReefmanPentode::el84(),
    };
    for (int knee = 0; knee < 2; ++knee) {
        for (bool se : {false, true}) {
            for (const auto& preset : models) {
                auto m = preset;
                m.knee = knee == 0 ? wdf::PentodeKneeType::DerkD
                                   : wdf::PentodeKneeType::DerkE;
                m.secondaryEmission = se;
                for (double vgk : {-0.2, 0.0, 0.15}) {
                    for (double vsk : {50.0, 150.0}) {
                        for (double vpk : {20.0, 100.0, 300.0}) {
                            const auto c = m.eval(vgk, vsk, vpk);
                            REQUIRE(std::isfinite(c.ip));
                            REQUIRE(std::isfinite(c.is));
                            REQUIRE(std::isfinite(c.ig));
                            for (int d = 0; d < 7; ++d)
                                checkDerivative(c, m, vgk, vsk, vpk, d);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("pentode matched-pair count scales all currents and derivatives", "[pentodecontracts]") {
    const auto single = wdf::ReefmanPentode::el34().eval(-0.1, 120.0, 250.0);
    auto pair = wdf::ReefmanPentode::el34(2.0);
    const auto doubled = pair.eval(-0.1, 120.0, 250.0);
    REQUIRE(doubled.ip == Approx(2.0 * single.ip));
    REQUIRE(doubled.is == Approx(2.0 * single.is));
    REQUIRE(doubled.ig == Approx(2.0 * single.ig));
    REQUIRE(doubled.dip_dvgk == Approx(2.0 * single.dip_dvgk));
    REQUIRE(doubled.dig_dvgk == Approx(2.0 * single.dig_dvgk));
}
