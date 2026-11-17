#include <catch2.hpp>

#include <stdexcept>
#include <string>

#include "spice/Compiled.h"
#include "spice/SpiceNet.h"

// The parser's contract, asserted instead of assumed (PLAN section 6, item #0).
//
// Why this is a test and not only a stricter parser: `SpiceNet.h` used to fall off
// the end of its dispatch for every line it did not recognise, so a netlist holding
// an `E1 B+ 0 DC 250` supply and an `X1 p g k 12AX7` tube parsed to a circuit
// missing both -- with no error. The engine then compiled that *unintended*
// topology, and on the fixture the first draft produced it crashed on a floating
// node. A component that can vanish without a message is worse than one that fails
// loudly, so the failure is pinned here: it is what has to stay true every time a
// new element letter is added to the parser.

namespace {
/// What the parser is allowed to read past. `.param`, `.subckt` and `.control` are
/// consumed upstream; `.model`, `.tran`, `.backanno` and `.end` reach the dispatch
/// and are skipped there. None of them is a component.
const std::string kDirectives =
    ".param rin = 1k\n"
    ".model dx d (IS=1n RS=1 CJO=1p)\n"
    ".tran 0 50m 0 10u\n"
    ".backanno\n"
    ".end\n";
}  // namespace

TEST_CASE("the parser builds every element it understands and reads past directives",
          "[spice]") {
    const wdf::Circuit c = wdf::buildFromString(
        "* a whole-line comment\n"
        "V1 in 0 DC 5\n"
        "R1 in out 1k\n"
        "C1 out 0 1u\n"
        "L1 out b 1m\n"
        "L2 b 0 2m\n"
        "K1 L1 L2 0.99\n"
        "I1 b 0 DC 1m\n"
        "E1 eout 0 in 0 2.5 0.1\n"
        "XP1 p s g 0 EL34\n" +
        kDirectives);
    REQUIRE(c.src.size() == 1);
    REQUIRE(c.res.size() == 1);
    REQUIRE(c.cap.size() == 1);
    REQUIRE(c.ind.size() == 2);
    REQUIRE(c.mutual.size() == 1);
    REQUIRE(c.mutual[0].name == "K1");
    REQUIRE(c.mutual[0].indNames == std::vector<std::string>{"L1", "L2"});
    REQUIRE(c.mutual[0].k == 0.99);
    REQUIRE(c.isrc.size() == 1);
    REQUIRE(c.vcvs.size() == 1);
    REQUIRE(c.vcvs[0].name == "E1");
    REQUIRE(c.vcvs[0].gain == 2.5);
    REQUIRE(c.vcvs[0].ro == 0.1);
    REQUIRE(c.pent.size() == 1);
    REQUIRE(c.pent[0].name == "XP1");
    REQUIRE(c.tri.empty());
}

TEST_CASE("the parser refuses an element letter it does not know", "[spice]") {
    // Unsupported element letters like 'Q' (BJT) or 'M' (MOSFET) must fail loudly.
    REQUIRE_THROWS_AS(wdf::buildFromString("V1 in 0 DC 5\nQ1 c b e 2N3904\n"),
                      std::runtime_error);
    REQUIRE_THROWS_AS(wdf::buildFromString("M1 d g s b NMOS\n"),
                      std::runtime_error);
}

TEST_CASE("Tubes_12AX7 elaborates as the quadric triode", "[spice]") {
    // The Tubes.lib subckt still supplies the passive grid network and the
    // inter-electrode capacitances, but the plate law is the engine's one
    // triode law -- not the subckt's Koren parameters.
    const wdf::Circuit c = wdf::buildFromString(
        ".subckt Tubes_12AX7 k g p\n"
        ".ends\n"
        ".subckt 12AX7_model k g p\n"
        ".param mu=100\n"
        ".ends\n"
        "X1 nc 0 in p Tubes_12AX7\n");
    REQUIRE(c.tri.size() == 1);
    REQUIRE(c.tri[0].model.p.kP2 == Approx(wdf::kQuadric12AX7.kP2));
}

TEST_CASE("the parser refuses an X naming a subcircuit it cannot elaborate", "[spice]") {
    // The other half of the same finding: `12AX7` is not `Tubes_12AX7`, so the tube
    // used to be dropped and its four nodes left floating.
    REQUIRE_THROWS_AS(wdf::buildFromString("X1 p g k 12AX7\n"), std::runtime_error);
    REQUIRE_THROWS_AS(wdf::buildFromString("X1 p s g k UnknownTubeModel\n"), std::runtime_error);
}

TEST_CASE("the parser refuses a known element with the wrong shape", "[spice]") {
    // These used to be the *quieter* half of the same bug: a letter the parser knows,
    // with no value or no terminals, parsed as "no such element".
    REQUIRE_THROWS_AS(wdf::buildFromString("R1 a b\n"), std::runtime_error);   // no value
    REQUIRE_THROWS_AS(wdf::buildFromString("V1 a\n"), std::runtime_error);     // one terminal
    REQUIRE_THROWS_AS(wdf::buildFromString("R1\n"), std::runtime_error);       // value only
    REQUIRE_THROWS_AS(wdf::buildFromString("K1 L1\n"), std::runtime_error);    // single inductor
    REQUIRE_THROWS_AS(wdf::buildFromString("K1 L1 L2\n"), std::runtime_error); // no coupling value
    REQUIRE_THROWS_AS(wdf::buildFromString("K1 L1 L2 1.0\n"), std::runtime_error);  // |k| >= 1.0
    REQUIRE_THROWS_AS(wdf::buildFromString("K1 L1 L2 -1.2\n"), std::runtime_error); // |k| >= 1.0
    REQUIRE_THROWS_AS(wdf::buildFromString("VR1 a b\n"), std::runtime_error);        // VR no value
    REQUIRE_THROWS_AS(wdf::buildFromString("CR1 a\n"), std::runtime_error);          // CR missing terminal
    REQUIRE_THROWS_AS(wdf::buildFromString("E1 out 0 in 0\n"), std::runtime_error);  // E missing gain
    REQUIRE_THROWS_AS(wdf::buildFromString("XP1 p s g\n"), std::runtime_error);      // XP missing cathode/model
    REQUIRE_THROWS_AS(wdf::buildFromString("XP1 p s g k NonExistentPentode\n"), std::runtime_error); // XP unknown model
}

TEST_CASE("the parser builds VR and CR elements correctly", "[spice]") {
    const wdf::Circuit c = wdf::buildFromString(
        "VR_Mid in out 25k pos=0.7 LOG\n"
        "CR1 out 0 5k\n");
    REQUIRE(c.varRes.size() == 1);
    REQUIRE(c.varRes[0].name == "VR_Mid");
    REQUIRE(c.varRes[0].maxResistance == 25000.0);
    REQUIRE(c.varRes[0].position == Approx(0.7));
    REQUIRE(c.varRes[0].taper == wdf::PotTaper::Log);
    REQUIRE(c.res.size() == 1); // Expanded underlying resistor
    REQUIRE(c.res[0].name == "VR_Mid");

    REQUIRE(c.ctrlRes.size() == 1);
    REQUIRE(c.ctrlRes[0].name == "CR1");
    REQUIRE(c.ctrlRes[0].initialOhms == 5000.0);
}

TEST_CASE("compilation refuses coupled inductors naming a non-existent inductor", "[spice]") {
    const wdf::Circuit c = wdf::buildFromString("L1 a 0 1m\nK1 L1 L_missing 0.99\n");
    REQUIRE_THROWS_AS(wdf::compile(c, {}), std::runtime_error);
}
