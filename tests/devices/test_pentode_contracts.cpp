#include <catch2.hpp>

#include <cmath>
#include <initializer_list>
#include <vector>

#include "devices/Pentode.h"
#include "spice/MatchedPentodes.h"
#include "wdf/Engine.h"
#include "wdf/solver/PentodePlatePair.h"

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

TEST_CASE("matched pentode branches preserve the plate and both sets of probe voltages", "[pentodecontracts]") {
    wdf::Circuit circuit;
    circuit.addSrc("drive", "0", -29.0);
    circuit.addSrc("supply", "0", 450.0);
    circuit.addSrc("rail1", "0", 430.0);
    circuit.addSrc("rail2", "0", 430.0);
    circuit.addRes("supply", "plate", 900.0);
    circuit.addRes("drive", "grid1", 1500.0);
    circuit.addRes("drive", "grid2", 1500.0);
    circuit.addRes("rail1", "screen1", 100.0);
    circuit.addRes("rail2", "screen2", 100.0);
    circuit.addPentode("plate", "screen1", "grid1", "0", wdf::k6L6GC);
    circuit.addPentode("plate", "screen2", "grid2", "0", wdf::k6L6GC);

    SECTION("identical local feeds") {
        auto reduced = circuit;
        REQUIRE(wdf::collapseMatchedPentodes(reduced) == 1);
        REQUIRE(reduced.src.size() == circuit.src.size());
        wdf::Engine original(circuit, 48000.0), paired(reduced, 48000.0);
        std::vector<std::pair<int, int>> probes;
        for (const std::string name : {"plate", "grid1", "grid2", "screen1", "screen2"}) {
            const int a = original.nodeIndex(name), b = paired.nodeIndex(name);
            REQUIRE(a >= 0);
            REQUIRE(b >= 0);
            probes.emplace_back(a, b);
        }
        auto sources = std::vector<double>{-29.0, 450.0, 430.0, 430.0};
        double error = 0.0;
        for (int n = 0; n < 512; ++n) {
            sources[0] = -29.0 + 24.0 * std::sin(0.0625 * n);
            const auto& full = original.step(sources);
            const auto& merged = paired.step(sources);
            for (const auto& [a, b] : probes)
                error = std::max(error, std::abs(full[a] - merged[b]));
        }
        REQUIRE(error < 1e-6);
    }
    SECTION("different rail set points") {
        circuit.src.back().dc -= 1.0;
        REQUIRE(wdf::collapseMatchedPentodes(circuit) == 0);
    }
    SECTION("independently driven screens") {
        const size_t driven[] = {0, 3};
        REQUIRE(wdf::collapseMatchedPentodes(circuit, driven) == 0);
    }
    SECTION("extra screen load") {
        circuit.addCap("screen2", "0", 10e-9);
        REQUIRE(wdf::collapseMatchedPentodes(circuit) == 0);
    }
    SECTION("live switch contact in a feed") {
        circuit.res.back().name = "SW1_A";
        REQUIRE(wdf::collapseMatchedPentodes(circuit) == 0);
    }
    SECTION("different model cards") {
        circuit.pent.back().model.reefman.kp += 0.01;
        REQUIRE(wdf::collapseMatchedPentodes(circuit) == 0);
    }
}

TEST_CASE("coupled DerkD plates recover both voltages across cutoff", "[pentodecontracts]") {
    constexpr double z11 = 3100., z22 = 2800.;
    const auto& left = wdf::k6L6GCPair;
    const auto& right = wdf::kEL34Pair;
    const double trial1[] = {-5., -24., 430.}, trial2[] = {210., -32., 450.};
    wdf::solver::DerkDPlate law1, law2;
    REQUIRE(law1.at(left, trial1, left.eval(trial1[1], trial1[2], trial1[0]).is));
    REQUIRE(law2.at(right, trial2, right.eval(trial2[1], trial2[2], trial2[0]).is));
    for (double mutual : {-1300., 800.}) {
        for (double plate1 : {-80., 0., 15., 400.}) {
            for (double plate2 : {-50., 3., 180., 700.}) {
                CAPTURE(mutual, plate1, plate2);
                const double i1 = left.eval(trial1[1], trial1[2], plate1).ip;
                const double i2 = right.eval(trial2[1], trial2[2], plate2).ip;
                double v1, v2;
                REQUIRE(wdf::solver::solvePentodePlatePair(law1, law2,
                    plate1 + z11 * i1 + mutual * i2, plate2 + mutual * i1 + z22 * i2,
                    z11, z22, mutual, v1, v2));
                REQUIRE(std::abs(v1 - plate1) < 1e-7);
                REQUIRE(std::abs(v2 - plate2) < 1e-7);
            }
        }
    }
}
