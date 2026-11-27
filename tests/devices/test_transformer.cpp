#include <catch2.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "devices/CoupledInductors.h"
#include "format/Document.h"
#include "spice/Net.h"
#include "spice/SpiceNet.h"
#include "spice/Circuit.h"
#include "wdf/Engine.h"

TEST_CASE("CoupledInductorMatrices dynamic implementation matches CoupledInductors<3>", "[transformer]") {
    const std::vector<double> L { 1.0e-3, 4.0e-3, 9.0e-3 };
    const double k = 0.6;
    constexpr double kFs = 48000.0;

    wdf::CoupledInductorMatrices dyn(L, k, kFs);
    wdf::CoupledInductors<3> ci(std::array<double, 3>{L[0], L[1], L[2]}, k);
    ci.prepare(kFs);

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            REQUIRE(dyn.G[static_cast<size_t>(i)][static_cast<size_t>(j)] ==
                    Approx(ci.conductance(i, j)).epsilon(1.0e-10));
        }
    }
}

TEST_CASE("Explicit coupled-inductor matrices keep the supplied L in R", "[transformer]") {
    const std::vector<std::vector<double>> L {
        {1.0, 0.5, 0.2},
        {0.5, 2.0, 0.3},
        {0.2, 0.3, 4.0},
    };
    constexpr double kFs = 48000.0;
    wdf::CoupledInductorMatrices mats(L, kFs);

    REQUIRE(mats.G.size() == 3);
    REQUIRE(mats.G[0].size() == 3);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            CHECK(mats.R[i][j] == Approx((2.0 / (1.0 / kFs)) * L[i][j]).epsilon(1.0e-10));
}

TEST_CASE("Audio transformer achieves expected turns ratio voltage step-up", "[transformer]") {
    // Manual model: one primary turn to two secondary turns, with a small
    // leakage floor so the coupled matrix is stiff but still very strongly
    // coupled. DCR is deliberately zero so this test measures the step-up
    // itself, not the resistor at the winding.
    constexpr double kFs = 48000.0;
    constexpr double kFreq = 1000.0;

    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addRes("in", "p1", 50.0);

    wdf::TransformerSpec manual;
    manual.turnsPrimary = 1.0;
    manual.turnsSecondary = 2.0;
    manual.model = "Manual";
    manual.Lm = 1.0;
    manual.leakage = 1.0e-3;
    c.addTransformerElement("p1", "0", "s1", "0", manual, "T1");
    c.addRes("s1", "0", 100e3);

    wdf::Engine engine(c, kFs);
    const int s1Idx = engine.nodeIndex("s1");
    REQUIRE(s1Idx >= 0);

    double maxSec = 0.0;
    std::vector<double> sv(1, 0.0);
    for (int n = 0; n < 480; ++n) {
        sv[0] = 1.0 * std::sin(2.0 * M_PI * kFreq * static_cast<double>(n) / kFs);
        const auto& nodes = engine.step(sv);
        if (n > 100)
            maxSec = std::max(maxSec, std::abs(nodes[static_cast<size_t>(s1Idx)]));
    }

    REQUIRE(maxSec == Approx(2.0).epsilon(0.01));
}

TEST_CASE("SPICE K directive parses and simulates an audio transformer end-to-end", "[transformer]") {
    const std::string netlist =
        "* SPICE Transformer Test\n"
        "V1 in 0 DC 0\n"
        "Rin in pri 100\n"
        "L1 pri 0 0.5\n"
        "L2 sec 0 2.0\n"
        "K_audio L1 L2 0.9995\n"
        "Rload sec 0 50k\n";

    const wdf::Circuit c = wdf::buildFromString(netlist);
    REQUIRE(c.mutual.size() == 1);
    REQUIRE(c.mutual[0].name == "K_audio");
    REQUIRE(c.mutual[0].k == 0.9995);
    REQUIRE(c.mutual[0].L.empty());

    wdf::Engine engine(c, 48000.0);
    const int secIdx = engine.nodeIndex("sec");
    REQUIRE(secIdx >= 0);

    double maxSec = 0.0;
    std::vector<double> sv(1, 0.0);
    for (int n = 0; n < 480; ++n) {
        sv[0] = 1.0 * std::sin(2.0 * M_PI * 1000.0 * static_cast<double>(n) / 48000.0);
        const auto& nodes = engine.step(sv);
        if (n > 100)
            maxSec = std::max(maxSec, std::abs(nodes[static_cast<size_t>(secIdx)]));
    }

    REQUIRE(maxSec == Approx(2.0).epsilon(0.01));
}

TEST_CASE("Multi-winding transformer (W = 3) center-tapped secondary", "[transformer]") {
    constexpr double kFs = 48000.0;

    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addRes("in", "pri", 50.0);
    c.addInd("pri", "0", 1.0, "Lpri");
    c.addInd("sec1", "0", 1.0, "Lsec1");
    c.addInd("sec2", "0", 1.0, "Lsec2");
    c.addMutualInd("K_triple", {"Lpri", "Lsec1", "Lsec2"}, 0.999);
    c.addRes("sec1", "0", 10e3);
    c.addRes("sec2", "0", 10e3);

    wdf::Engine engine(c, kFs);
    const int s1 = engine.nodeIndex("sec1");
    const int s2 = engine.nodeIndex("sec2");
    REQUIRE(s1 >= 0);
    REQUIRE(s2 >= 0);

    std::vector<double> sv(1, 0.0);
    double maxS1 = 0.0, maxS2 = 0.0;
    for (int n = 0; n < 480; ++n) {
        sv[0] = 1.0 * std::sin(2.0 * M_PI * 1000.0 * static_cast<double>(n) / kFs);
        const auto& nodes = engine.step(sv);
        if (n > 100) {
            maxS1 = std::max(maxS1, std::abs(nodes[static_cast<size_t>(s1)]));
            maxS2 = std::max(maxS2, std::abs(nodes[static_cast<size_t>(s2)]));
        }
    }

    REQUIRE(maxS1 == Approx(1.0).epsilon(0.01));
    REQUIRE(maxS2 == Approx(1.0).epsilon(0.01));
}

TEST_CASE("Plain transformer element compiles to one coupled matrix", "[transformer]") {
    wdf::Circuit c;
    wdf::TransformerSpec spec;
    spec.turnsPrimary = 2.0;
    spec.turnsSecondary = 1.0;
    spec.model = "Manual";
    spec.Lm = 4.0;
    spec.leakage = 1.0e-3;
    c.addTransformerElement("p1", "p2", "s1", "s2", spec, "T7");

    REQUIRE(c.ind.size() == 2);
    CHECK(c.ind[0].name == "T7$pri");
    CHECK(c.ind[1].name == "T7$sec");
    CHECK(c.ind[0].a == "p1");
    CHECK(c.ind[0].b == "p2");
    CHECK(c.ind[1].a == "s1");
    CHECK(c.ind[1].b == "s2");
    CHECK(c.ind[0].value == Approx(4.001).epsilon(1.0e-12));
    CHECK(c.ind[1].value == Approx(1.00025).epsilon(1.0e-9));

    REQUIRE(c.mutual.size() == 1);
    CHECK(c.mutual[0].name == "T7");
    CHECK(c.mutual[0].L.empty() == false);
    REQUIRE(c.mutual[0].L.size() == 2);
    CHECK(c.mutual[0].L[0][0] == Approx(4.001).epsilon(1.0e-12));
    CHECK(c.mutual[0].L[1][1] == Approx(1.00025).epsilon(1.0e-9));
    CHECK(c.mutual[0].L[0][1] == Approx(2.0).epsilon(1.0e-12));
}

TEST_CASE("Manual transformer keeps a leakage below the automatic estimate", "[transformer]") {
    wdf::TransformerSpec spec;
    spec.turnsPrimary = 28.7;
    spec.model = "Real — Manual";
    spec.Lm = 21.39;
    spec.leakage = 6.77e-3;
    CHECK(wdf::resolveTransformerSpec(spec).leakage == spec.leakage);
}

TEST_CASE("Center-tapped transformer element compiles to one three-winding device", "[transformer]") {
    wdf::Circuit c;
    wdf::TransformerSpec spec;
    spec.turnsPrimary = 2.0;
    spec.turnsSecondary = 1.0;
    spec.model = "Manual";
    spec.Lm = 4.0;
    spec.leakage = 1.0e-3;
    c.addCenterTapTransformerElement("p1", "tap", "p2", "s1", "s2", spec, "CT7");

    REQUIRE(c.ind.size() == 3);
    CHECK(c.ind[0].name == "CT7$sup");
    CHECK(c.ind[1].name == "CT7$slw");
    CHECK(c.ind[2].name == "CT7$sec");
    CHECK(c.ind[0].a == "p1");
    CHECK(c.ind[0].b == "tap");
    CHECK(c.ind[1].a == "tap");
    CHECK(c.ind[1].b == "p2");
    CHECK(c.ind[2].a == "s1");
    CHECK(c.ind[2].b == "s2");

    const double lhh = 0.25 * spec.Lm + 0.5 * spec.leakage;
    const double lss = (spec.Lm + spec.leakage) *
                       (spec.turnsSecondary / spec.turnsPrimary) *
                       (spec.turnsSecondary / spec.turnsPrimary);
    CHECK(c.ind[0].value == Approx(lhh).epsilon(1.0e-12));
    CHECK(c.ind[1].value == Approx(lhh).epsilon(1.0e-12));
    CHECK(c.ind[2].value == Approx(lss).epsilon(1.0e-9));

    REQUIRE(c.mutual.size() == 1);
    CHECK(c.mutual[0].name == "CT7");
    CHECK(c.mutual[0].L.empty() == false);
    REQUIRE(c.mutual[0].L.size() == 3);
    for (int i = 0; i < 3; ++i)
        REQUIRE(c.mutual[0].L[i].size() == 3);
    CHECK(c.mutual[0].L[0][1] == Approx(1.0).epsilon(1.0e-12));
    CHECK(c.mutual[0].L[0][2] == Approx(1.0).epsilon(1.0e-12));
    CHECK(c.mutual[0].L[1][2] == Approx(1.0).epsilon(1.0e-12));
}

TEST_CASE("A transformer drawing converts to the new coupled matrix model", "[transformer]") {
    wdf::LiveWDFDocument doc;

    wdf::SchematicComponent t;
    t.id = "T7";
    t.type = "transformer";
    t.pins = {{"p1", "pri"}, {"p2", "0"}, {"s1", "sec"}, {"s2", "0"}};
    t.properties = {{"model", "Manual"}};
    t.values = {{"turnsPrimary", 2.0},
                {"turnsSecondary", 1.0},
                {"Lm", 4.0},
                {"leakage", 1.0e-3}};
    doc.components.push_back(t);

    const auto c = doc.toCircuit();
    REQUIRE(c.ind.size() == 2);
    CHECK(c.ind[0].name == "T7$pri");
    CHECK(c.ind[1].name == "T7$sec");
    REQUIRE(c.mutual.size() == 1);
    CHECK(c.mutual[0].name == "T7");
    CHECK(c.mutual[0].L.empty() == false);
}

TEST_CASE("A centre-tapped transformer drawing converts to the three-winding device", "[transformer]") {
    wdf::LiveWDFDocument doc;

    wdf::SchematicComponent ct;
    ct.id = "T7";
    ct.type = "transformer_ct";
    ct.pins = {{"p1", "pa"}, {"pt", "tap"}, {"p2", "pb"}, {"s1", "sa"}, {"s2", "sb"}};
    ct.properties = {{"model", "Manual"}};
    ct.values = {{"turnsPrimary", 2.0},
                 {"turnsSecondary", 1.0},
                 {"Lm", 4.0},
                 {"leakage", 1.0e-3}};
    doc.components.push_back(ct);

    // The document converter drops islands, so this test needs one ordinary
    // part sharing one of the CT's nets. It is not testing the resistor; it is
    // keeping the transformer in the circuit so its three windings are compiled.
    wdf::SchematicComponent anchor;
    anchor.id = "R_anchor";
    anchor.type = "res";
    anchor.pins = {{"a", "sa"}, {"b", "0"}};
    anchor.values = {{"value", 1000.0}};
    doc.components.push_back(anchor);

    const auto c = doc.toCircuit();
    REQUIRE(c.ind.size() == 3);
    CHECK(c.ind[0].name == "T7$sup");
    CHECK(c.ind[1].name == "T7$slw");
    CHECK(c.ind[2].name == "T7$sec");
    CHECK(c.ind[0].a == "pa");
    CHECK(c.ind[0].b == "tap");
    CHECK(c.ind[1].a == "tap");
    CHECK(c.ind[1].b == "pb");
    CHECK(c.ind[2].a == "sa");
    CHECK(c.ind[2].b == "sb");

    REQUIRE(c.mutual.size() == 1);
    CHECK(c.mutual[0].name == "T7");
    CHECK(c.mutual[0].L.empty() == false);
    REQUIRE(c.mutual[0].L.size() == 3);
}
