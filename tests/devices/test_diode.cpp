#include <catch2.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "devices/Diode.h"
#include "support/CircuitCases.h"
#include "support/NgspiceOracle.h"
#include "support/RootGroups.h"
#include "spice/SpiceNet.h"
#include "wdf/Engine.h"

// The `D` element (LiveWDF step G): a non-tube nonlinear root.
//
// Why this is an increment of its own and not a line in an existing test: every
// root the engine had ever seen was a triode, so `K_ = 3*tri.size()`, the
// reduced Newton system was "two rows per tube", and `buildRootOrder()` opened
// with `if (nTri <= 0) return false`. All of that *worked* -- it just had never
// been anything but tubes. A diode is the cheapest possible second opinion: one
// port, one current, one derivative, no coupling of its own. If the root
// abstraction survives that unchanged it is a root abstraction; if it needs
// triode-shaped exceptions, they would have surfaced here.
//
// What is checked, in order:
//   1. the junction law itself, against its own closed-form reference, in both
//      the `RS = 0` and `RS > 0` forms;
//   2. the parser's contract (this is the "checker before parser" rule): what it
//      must build, what it must refuse, and the one thing it must keep doing --
//      reading past a `.model` line no diode uses;
//   3. the DC operating point against ngspice, including the root port's own
//      voltage, which is what catches a port wired to the wrong two nodes;
//   4. a running diode clipper against ngspice;
//   5. a single diode, which the closed form *does* take -- one port, so one
//      group, so the junction law over `Z[e][e] + RS` is one omega call and no
//      Newton at all. This is the increment that removed the `nD_ > 0` refusal;
//   6. a triode and a diode on one node, which is where the closed form has to
//      stand down -- not because "a diode is not a triode" any more, but because
//      two live ports share a node and no ordering of them exists.
//
// The clipper is the circuit `docs/PLAN.md` step G names, and it is the real test:
// two anti-parallel diodes give two root ports that never see zero current, on a
// topology where the old `nTri <= 0` gate would have said "nothing to solve". Its
// one-diode sibling is the control for the *ordering* question: same shape, one
// live port instead of two, and the root goes from a joint Newton to a closed
// form.

using lwdf_test::controlBlock;
using lwdf_test::parseOpFile;
using lwdf_test::parseTranFile;
using lwdf_test::runDeck;
using lwdf_test::TranTable;
using lwdf_test::vecFile;

namespace {
constexpr double kFs = 48000.0;
constexpr double kTstep = 1.0 / kFs;

/// The reference junction law, written the naive way on purpose: `rs` is folded
/// out so it can be evaluated without solving anything. Used only to check that
/// `DiodeModel::eval` says what it claims to say.
double shockley(const wdf::DiodeModel& d, double v) {
    return d.isat * std::expm1(v / d.nvt);
}

std::string clipperNet() {
    return "V1 in 0 SIN(0 1 159.15)\n"
           "Rin in out 1k\n"
           "C1 out 0 1u\n"
           "DP out 0 D1N914\n"
           "DP2 0 out D1N914\n"
           ".model D1N914 D (IS=1e-9 N=1)\n";
}

/// The clipper deck with its second junction removed: one live diode port, so the
/// root is one group and the closed form takes it. Same `Rin`/`C1` as the clipper,
/// so the two differ in exactly the thing under test.
std::string singleDiodeNet() {
    return "V1 in 0 SIN(0 1 159.15)\n"
           "Rin in out 1k\n"
           "C1 out 0 1u\n"
           "DP out 0 D1N914\n"
           ".model D1N914 D (IS=1e-9 N=1)\n";
}

/// One quadric stage with an optional diode from its plate to ground.
/// A plain function rather than a lambda so `CircuitCase::build` can point at it,
/// which is what lets `rootGroupCoupling` measure the mixed case's coupling.
wdf::Circuit mixedStage(bool withDiode) {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addSrc("e", "0", 250.0);
    c.addRes("in", "g", 68e3);
    c.addRes("g", "0", 1e6);
    c.addRes("e", "p", 100e3);
    c.addRes("k", "0", 1.5e3);
    c.addTri("g", "p", "k", "g", lwdf_test::gridlessQuadric());
    if (withDiode)
        c.addDiode("p", "0", wdf::DiodeModel(1e-9, 1.0, 0.0), "DCLAMP");
    return c;
}
wdf::Circuit mixedStageClamped() { return mixedStage(true); }
}  // namespace

// --------------------------------------------------------------------------------------
// 1. the device law
// --------------------------------------------------------------------------------------

TEST_CASE("the Shockley law with zero series resistance is exact at both signs", "[diode][device]") {
    const wdf::DiodeModel d(1e-9, 1.0, 0.0);
    double i = 0.0, di = 0.0;

    d.eval(0.0, i, di);
    REQUIRE(i == 0.0);  // expm1(0) is exactly 0, so i(0) is exactly 0
    REQUIRE(di == Approx(d.isat / d.nvt).margin(1e-20));

    d.eval(-0.5, i, di);
    // A reverse-biased ideal junction saturates: the current approaches -Is and
    // stops. `expm1` is what keeps the residual (about 4e-9 of Is here) instead
    // of losing it to cancellation.
    REQUIRE(std::abs(i + d.isat) < 1e-7 * d.isat);

    for (double v : {-0.01, 0.01, 0.2, 0.4}) {
        d.eval(v, i, di);
        const double ref = shockley(d, v);
        REQUIRE(i == Approx(ref).margin(std::abs(ref) * 1e-12 + 1e-24));
    }

    // The derivative is the derivative: finite-difference it in both directions.
    for (double v : {0.0, 0.2, 0.4, 0.7}) {
        const double h = 1e-6;
        double ip = 0.0, im = 0.0, junk = 0.0;
        d.eval(v + h, ip, junk);
        d.eval(v - h, im, junk);
        d.eval(v, i, di);
        const double fd = (ip - im) / (2.0 * h);
        REQUIRE(di == Approx(fd).margin(std::abs(fd) * 1e-6 + 1e-18));
    }
}

TEST_CASE("the series-resistance form solves the implicit junction law", "[diode][device]") {
    // With RS > 0 the law is `i = Is*(exp((v - i*RS)/nVt) - 1)`, which is
    // implicit in i. `eval` claims to solve it through omega; check the claim
    // by resubstituting the returned current.
    const wdf::DiodeModel d(1e-9, 1.0, 50.0);
    double i = 0.0, di = 0.0;
    for (double v : {-0.3, 0.0, 0.3, 0.6, 1.0}) {
        d.eval(v, i, di);
        REQUIRE(i > -d.isat * (1.0 + 1e-9));
        const double vJunction = v - i * d.rs;
        REQUIRE(i == Approx(shockley(d, vJunction)).margin(std::abs(i) * 1e-9 + 1e-12 * d.isat));
    }
    // i(0) = 0 here too: at v = 0 the only solution is no current.
    d.eval(0.0, i, di);
    REQUIRE(std::abs(i) < 1e-18);
}

TEST_CASE("the grid diode is the same law under its old name", "[diode][device]") {
    // `Diode.h` aliases `GridDiode` to `DiodeModel`. That is a refactor, not a
    // behaviour change, so the alias is pinned to the arithmetic it replaced.
    const wdf::GridDiode g(1e-9, 1.0, 1.0);
    const wdf::DiodeModel d(1e-9, 1.0, 1.0);
    double gi = 0.0, gdi = 0.0, di = 0.0, dd = 0.0;
    for (double v : {-0.5, 0.0, 0.5, 2.0}) {
        g.eval(v, gi, gdi);
        d.eval(v, di, dd);
        REQUIRE(gi == di);
        REQUIRE(gdi == dd);
    }
    REQUIRE(g.rs == 1.0);
    REQUIRE(g.nvt == wdf::kThermalVoltage);
}

// --------------------------------------------------------------------------------------
// 2. the parser's contract (checker before parser, PLAN section 6 item #0)
// --------------------------------------------------------------------------------------

TEST_CASE("the parser builds a D from its own .model line", "[diode][spice]") {
    const wdf::Circuit c = wdf::buildFromString(
        "V1 in 0 DC 5\n"
        "R1 in out 1k\n"
        "D1 out 0 DTEST\n"
        ".model DTEST D (IS=1e-9 N=1.5 RS=2)\n");
    REQUIRE(c.diode.size() == 1);
    REQUIRE(c.diode[0].a == "out");
    REQUIRE(c.diode[0].b == "0");
    REQUIRE(c.diode[0].model.isat == 1e-9);
    REQUIRE(c.diode[0].model.n == 1.5);
    REQUIRE(c.diode[0].model.rs == 2.0);
    REQUIRE(c.res.size() == 1);
    REQUIRE(c.src.size() == 1);
}

TEST_CASE("a diode model falls back to SPICE's own defaults", "[diode][spice]") {
    const wdf::Circuit c =
        wdf::buildFromString("D1 a b M1\n.model M1 D\n");
    REQUIRE(c.diode.size() == 1);
    REQUIRE(c.diode[0].model.isat == Approx(1e-14));  // SPICE's default IS
    REQUIRE(c.diode[0].model.n == 1.0);
    REQUIRE(c.diode[0].model.rs == 0.0);  // SPICE's default RS: the zero-R path
}

TEST_CASE("the parser refuses a diode line it cannot complete", "[diode][spice]") {
    // A diode naming a model that does not exist. Before this check the line
    // would have had no model to build from -- and a `D` with no law is not a
    // diode, it is a missing element in a circuit that still compiles.
    REQUIRE_THROWS_AS(wdf::buildFromString("D1 a b NOPE\n"), std::runtime_error);
    // No model name at all: a known letter with the wrong shape, which is the
    // second half of the same finding (`R1 a b` already refuses).
    REQUIRE_THROWS_AS(wdf::buildFromString("D1 a b\n"), std::runtime_error);
    REQUIRE_THROWS_AS(wdf::buildFromString("D1 a\n"), std::runtime_error);
}

TEST_CASE("a diode model that sets something the engine does not model is refused",
          "[diode][spice]") {
    // The deliberate choice, pinned because it is a choice: `CJO` changes the
    // circuit (it is a junction capacitance), and honouring the `.model` line by
    // quietly not having it is the increment-8 bug in a corner. So the parser
    // refuses, and the message says which parameter -- a refusal nobody can act
    // on is only marginally better than silence. `tests/oracle` and the fixtures
    // are the evidence that this does not block the circuits in scope.
    try {
        wdf::buildFromString("D1 a b M1\n.model M1 D (IS=1e-9 CJO=4p)\n");
        FAIL("an unmodelled diode parameter was accepted");
    } catch (const std::runtime_error& ex) {
        REQUIRE(std::string(ex.what()).find("cjo") != std::string::npos);
    }
    // ... while the parameters the engine does model are all it checks for.
    REQUIRE_NOTHROW(wdf::buildFromString("D1 a b M1\n.model M1 D (IS=1e-9 N=1 RS=0.5)\n"));
}

TEST_CASE("a .model line no diode uses is still just read past", "[diode][spice]") {
    // Lazy resolution, and it is the other behaviour that must not change: the
    // directives test above has carried `.model dx d (IS=1n RS=1 CJO=1p)` at the
    // top level since before this element existed. Eagerly validating every
    // `.model` line would have broken it -- and would have been right for the
    // wrong reason.
    REQUIRE_NOTHROW(wdf::buildFromString(
        "V1 in 0 DC 5\n"
        "R1 in out 1k\n"
        ".model dx d (IS=1n RS=1 CJO=1p)\n"
        ".tran 0 50m 0 10u\n"));
}

// --------------------------------------------------------------------------------------
// 3. the DC operating point against ngspice
// --------------------------------------------------------------------------------------

TEST_CASE("ngspice DC oracle: a single diode on a series resistor", "[diode][ngspice][dc]") {
    if (!lwdf_test::requireNgspice("diode DC oracle")) return;
    const std::string net =
        "V1 in 0 DC 5\n"
        "R1 in out 1k\n"
        "D1 out 0 DTEST\n"
        ".model DTEST D (IS=1e-14 N=1)\n";

    const std::string deck = "* livewdf diode dc oracle\n" + net +
                             controlBlock({"op"}, {"v(out) i(v1)"}, "ddc") + ".end\n";
    const auto run = runDeck(deck, "ddc");
    REQUIRE(run.ok);
    const std::vector<double> ng = parseOpFile(vecFile("ddc", 0));
    REQUIRE(ng.size() == 2);

    wdf::Circuit c = wdf::buildFromString(net);
    wdf::Engine e(c, kFs);
    REQUIRE(e.rootCount() == 1);
    std::vector<double> node, rootV;
    REQUIRE(e.solveDc(node, rootV));

    // verify.py DC threshold
    const double vOut = node[static_cast<size_t>(e.nodeIndex("out"))];
    REQUIRE(std::abs(ng[0] - vOut) <= 5e-4);

    // The root port is `out` against `0`, so its solved voltage *is* the diode's
    // voltage -- which is the check that the port was wired to the two nodes the
    // element names rather than to something that merely happens to be nearby.
    REQUIRE(std::abs(c.diode[0].model.isat *
                     std::expm1(rootV[0] / c.diode[0].model.nvt) +
                     ng[1]) <= 5e-6);
    REQUIRE(std::abs(rootV[0]) < 1.0);  // a silicon knee, not volts
    REQUIRE(rootV[0] > 0.0);            // forward biased at +5 V
    // `rootV` is the solved port voltage, not the open-circuit one: bias the same
    // resistor with the diode removed and the node would sit at 5 V.
    REQUIRE(rootV[0] < 4.0);
}

// --------------------------------------------------------------------------------------
// 4. the clipper running, against ngspice
// --------------------------------------------------------------------------------------

TEST_CASE("ngspice transient oracle: the diode clipper", "[diode][ngspice][tran]") {
    if (!lwdf_test::requireNgspice("diode clipper oracle")) return;
    const std::string net = clipperNet();

    const double tstop = 0.02;  // 20 ms = 960 samples (~3 cycles at 159.15 Hz)
    // `tmax` is deliberately a tenth of the output step. With `tmax = tstep` the
    // clipper's corners make ngspice's own step control the dominant error: two
    // ngspice runs of this deck at tmax = tstep and tmax = tstep/20 differ from
    // each other by 4.1e-4 V, which is the same size as the engine's error against
    // the coarse one. Forcing the internal step down moves the reference onto the
    // curve it claims to be on, and what is left is the engine's.
    char tranLine[160];
    std::snprintf(tranLine, sizeof tranLine, ".tran %.17g %.17g 0 %.17g\n", kTstep, tstop,
                  kTstep / 10.0);

    const std::string deck = "* livewdf diode clipper oracle\n" + std::string(tranLine) + net +
                             controlBlock({"run", "linearize v(out)"}, {"time v(out)"}, "dclip") +
                             ".end\n";

    const auto run = runDeck(deck, "dclip");
    REQUIRE(run.ok);
    const TranTable ng = parseTranFile(vecFile("dclip", 0), 1);
    REQUIRE(ng.nRows() >= 900);

    wdf::Circuit c = wdf::buildFromString(net);
    wdf::Engine e(c, kFs);
    REQUIRE(e.rootCount() == 2);
    REQUIRE(e.sourceCount() == 1);
    std::vector<double> node, rootV;
    REQUIRE(e.solveDc(node, rootV));
    e.reset();
    const int outIdx = e.nodeIndex("out");
    REQUIRE(outIdx >= 0);

    double maxErr = 0.0;
    double maxOut = 0.0;
    size_t maxRow = 0;
    int clipped = 0;
    int compared = 0;
    for (size_t row = 0; row < ng.nRows(); ++row) {
        const double t = ng.time[row];
        const double vin = 1.0 * std::sin(2.0 * M_PI * 159.15 * t);
        const std::vector<double> vEng = e.step({vin});
        const double engOut = vEng[static_cast<size_t>(outIdx)];
        REQUIRE(std::isfinite(engOut));
        REQUIRE(e.lastConverged());
        const double err = std::abs(engOut - ng.cols[0][row]);
        if (err > maxErr) {
            maxErr = err;
            maxRow = row;
        }
        maxOut = std::max(maxOut, std::abs(engOut));
        // The clipper has to actually clip. A diode that never conducts is a
        // passing test and a broken circuit: the assertion that the element
        // works is that the waveform is held down, not merely that it is finite.
        if (std::abs(engOut) < 0.6)
            ++clipped;
        ++compared;
    }
    REQUIRE(compared >= 900);
    REQUIRE(clipped > 0);
    REQUIRE(maxOut < 0.6);
    WARN("ngspice diode clipper oracle: maxErr=" << maxErr << " maxOut=" << maxOut << " at row "
                                                 << maxRow << " (t=" << ng.time[maxRow] << ") over "
                                                 << compared << " samples");
    // The threshold is set against this deck's *resolved* reference, not against
    // a convenient one: `tmax` above is a tenth of the output step, and the
    // engine lands at ~9e-5. (Measured against ngspice at `tmax = tstep` the same
    // loop reads ~4.6e-4, which is ngspice's own step-control error -- two
    // ngspice runs of this deck at those two step sizes differ from *each other*
    // by 4.1e-4. That is why the deck forces the smaller step.)
    REQUIRE(maxErr <= 2e-4);
}

// --------------------------------------------------------------------------------------
// 4b. the same oracle on the circuit the closed form takes
// --------------------------------------------------------------------------------------

TEST_CASE("ngspice transient oracle: one diode, solved in closed form", "[diode][ngspice][tran]") {
    if (!lwdf_test::requireNgspice("closed-form diode oracle")) return;
    // The clipper's oracle above checks the *element* against ngspice; this checks
    // the *closed form* against it, on a circuit where the engine is not running
    // Newton at all. Without this, the group solve's one-device diode path would
    // only ever be compared with the engine's own Newton -- a self-consistency
    // check, which cannot see an error the two paths share. It is the first
    // external reference the diode closed form gets.
    const std::string net = singleDiodeNet();

    const double tstop = 0.02;  // 20 ms = 960 samples (~3 cycles at 159.15 Hz)
    // Same reasoning as the clipper: `tmax` is a tenth of the output step, so the
    // reference is resolved rather than step-control-limited.
    char tranLine[160];
    std::snprintf(tranLine, sizeof tranLine, ".tran %.17g %.17g 0 %.17g\n", kTstep, tstop,
                  kTstep / 10.0);

    const std::string deck = "* livewdf single diode oracle\n" + std::string(tranLine) + net +
                             controlBlock({"run", "linearize v(out)"}, {"time v(out)"}, "dsing") +
                             ".end\n";

    const auto run = runDeck(deck, "dsing");
    REQUIRE(run.ok);
    const TranTable ng = parseTranFile(vecFile("dsing", 0), 1);
    REQUIRE(ng.nRows() >= 900);

    wdf::Circuit c = wdf::buildFromString(net);
    wdf::Engine e(c, kFs);
    REQUIRE(e.rootCount() == 1);
    // The claim this test is about: the engine is on the closed form, not Newton.
    REQUIRE(e.allowsExplicitRoot());
    REQUIRE(e.explicitRoots());

    std::vector<double> node, rootV;
    REQUIRE(e.solveDc(node, rootV));
    e.reset();
    const int outIdx = e.nodeIndex("out");
    REQUIRE(outIdx >= 0);

    double maxErr = 0.0;
    double maxOut = 0.0;
    double minOut = 0.0;
    size_t maxRow = 0;
    int compared = 0;
    for (size_t row = 0; row < ng.nRows(); ++row) {
        const double t = ng.time[row];
        const double vin = 1.0 * std::sin(2.0 * M_PI * 159.15 * t);
        const std::vector<double> vEng = e.step({vin});
        const double engOut = vEng[static_cast<size_t>(outIdx)];
        REQUIRE(std::isfinite(engOut));
        REQUIRE(e.lastConverged());
        const double err = std::abs(engOut - ng.cols[0][row]);
        if (err > maxErr) {
            maxErr = err;
            maxRow = row;
        }
        maxOut = std::max(maxOut, engOut);
        minOut = std::min(minOut, engOut);
        ++compared;
    }
    REQUIRE(compared >= 900);
    // The one-sided junction has to show both halves of its own behaviour: held
    // down on the forward side, and left to swing on the reverse side. A diode
    // that never conducts would pass an agreement check and be a broken circuit.
    REQUIRE(maxOut < 0.6);
    REQUIRE(minOut < -0.5);
    WARN("ngspice single diode oracle: maxErr=" << maxErr << " out=[" << minOut << ", " << maxOut
                                                << "] at row " << maxRow << " over " << compared
                                                << " samples");
    REQUIRE(maxErr <= 2e-4);
}

// --------------------------------------------------------------------------------------
// 5. the two device kinds in one circuit: the closed form has to stand down
// --------------------------------------------------------------------------------------

TEST_CASE("a triode and a diode on one node keep the joint Newton", "[diode][mixed]") {
    // The guard this pins is the one that is easy to get wrong in the *quiet*
    // direction. Step G's version of it refused on `nD_ > 0` -- "the closed form
    // covers triodes only" -- and that reason is gone: a lone diode now takes the
    // closed form. What is left is the one that was always the real one, coupling:
    // the diode hangs off the plate, so the tube's plate port and the diode's port
    // share a node, `Z[pk][e]` and its transpose `Z[e][pk]` are the *same* nonzero
    // number, and the ordering is live in both directions. Two live ports on one
    // node is the shared-cathode shape one device kind over.
    //
    // The physical assertion is that the diode is really in the loop: a diode
    // from plate to ground clamps the plate to its knee, which is a different
    // answer by two orders of magnitude, not a rounding difference.
    wdf::Circuit plain = mixedStage(false);
    wdf::Engine ePlain(plain, kFs);
    REQUIRE(ePlain.explicitRoots());  // a quadric triode is the fast path
    std::vector<double> nodeP, rootP;
    REQUIRE(ePlain.solveDc(nodeP, rootP));
    const double vPlateFree = nodeP[static_cast<size_t>(ePlain.nodeIndex("p"))];
    REQUIRE(vPlateFree > 50.0);  // a live plate, nowhere near a junction knee

    const wdf::Circuit clamped = mixedStage(true);
    wdf::Engine eClamp(clamped, kFs);
    REQUIRE(eClamp.rootCount() == 4);        // 3 triode ports + 1 diode
    REQUIRE_FALSE(eClamp.explicitRoots());   // and the closed form stands down

    // ... and the refusal is measured rather than asserted: both directions of the
    // coupling are live, which is exactly why no topological order exists.
    const lwdf_test::CircuitCase mtc{"mixed", &mixedStageClamped, &lwdf_test::sine1v};
    const lwdf_test::RootGroupCoupling cp = lwdf_test::rootGroupCoupling(clamped, mtc, kFs);
    REQUIRE(cp.nTri == 1);
    REQUIRE(cp.nD == 1);
    REQUIRE(cp.nGroups() == 2);
    REQUIRE(cp.predictedCoupled(0, 1));   // the tube's plate row sees the diode
    REQUIRE(cp.predictedCoupled(1, 0));   // and the diode sees the plate: a cycle
    REQUIRE(cp.maxAboveDiagonal() > 0.0);

    std::vector<double> nodeC, rootC;
    REQUIRE(eClamp.solveDc(nodeC, rootC));
    const double vPlateClamped = nodeC[static_cast<size_t>(eClamp.nodeIndex("p"))];
    WARN("plate: free=" << vPlateFree << " clamped=" << vPlateClamped);
    REQUIRE(vPlateClamped > 0.1);  // the junction is forward biased, not shorted
    REQUIRE(vPlateClamped < 1.0);  // and held at a silicon knee
    REQUIRE(vPlateClamped < vPlateFree / 50.0);

    // The same circuit has to run (not just solve once) on the joint Newton, and
    // every sample has to converge -- a root that is merely present is not a root
    // that is solved.
    eClamp.reset();
    for (int n = 0; n < 200; ++n) {
        const std::vector<double> v = eClamp.step({0.2 * std::sin(0.01 * n), 250.0});
        REQUIRE(eClamp.lastConverged());
        REQUIRE(std::isfinite(v[static_cast<size_t>(eClamp.nodeIndex("p"))]));
    }
}

// --------------------------------------------------------------------------------------
// 6. the lone diode the closed form takes
// --------------------------------------------------------------------------------------

TEST_CASE("a lone diode is a root the closed form takes", "[diode][closedform]") {
    // Step G shipped the opposite refusal on purpose, and it was the honest thing
    // to ship: the closed form covered triodes only, so `buildRootOrder` said
    // `if (nD_ > 0) return false` and every diode circuit kept the joint Newton.
    // A diode's port *is* its whole group and the junction law over `Z[e][e] + RS`
    // is one omega call, so the assertion inverts here -- and the inversion is the
    // point: the root abstraction now has two device kinds in it, not one kind and
    // an exception.
    const wdf::Circuit c = lwdf_test::diodeSingle();
    wdf::Engine e(c, kFs);
    REQUIRE(e.rootCount() == 1);  // one diode port and nothing else
    REQUIRE(e.allowsExplicitRoot());
    REQUIRE(e.explicitRoots());

    // The closed form is that circuit's solution, not a nearby one: the same engine
    // forced onto the joint Newton is the reference, on the same drive. (The sweep
    // in `test_root_groups.cpp` makes the same comparison over the whole fixture
    // library; this one is here so the claim is checkable beside the element it is
    // about.)
    wdf::Engine slow(lwdf_test::diodeSingle(), kFs);
    REQUIRE(slow.setExplicitRoots(false));
    REQUIRE_FALSE(slow.explicitRoots());

    const lwdf_test::CircuitCase tc{"diode_single", &lwdf_test::diodeSingle, &lwdf_test::sine1v};
    e.reset();
    slow.reset();
    const int outIdx = e.nodeIndex("out");
    REQUIRE(outIdx >= 0);
    double worst = 0.0;
    int clipped = 0;
    for (int n = 0; n < 4800; ++n) {
        const std::vector<double> sv = lwdf_test::sampleSources(c, tc, n);
        const std::vector<double>& a = e.step(sv);
        const std::vector<double>& b = slow.step(sv);
        REQUIRE(a.size() == b.size());
        for (size_t i = 0; i < a.size(); ++i)
            worst = std::max(worst, std::abs(a[i] - b[i]));
        if (a[static_cast<size_t>(outIdx)] < 0.6)
            ++clipped;
    }
    REQUIRE(worst < 1e-9);
    REQUIRE(clipped > 0);  // the junction conducts; a dead one would also agree
    WARN("diode_single: closed-form vs joint Newton, worst=" << worst);
}

// --------------------------------------------------------------------------------------
// 7. K-Method 1D Lookup Table (KTable1D)
// --------------------------------------------------------------------------------------

TEST_CASE("KTable1D: cubic Hermite interpolation accuracy and continuation", "[diode][ktable]") {
    wdf::KTable1D table;
    REQUIRE_FALSE(table.isReady());

    // Tabulate tanh(x) over [-5.0, 5.0] with 257 points (256 intervals, h = 10/256 = 0.039)
    table.build(-5.0, 5.0, 257, [](double x) { return std::tanh(x); });
    REQUIRE(table.isReady());
    REQUIRE(table.steps() == 257);
    REQUIRE(table.minV() == -5.0);
    REQUIRE(table.maxV() == 5.0);

    // Exact at nodes
    for (int i = 0; i < 257; i += 16) {
        const double x = -5.0 + i * (10.0 / 256.0);
        CHECK(table.lookup(x) == Approx(std::tanh(x)).margin(1e-12));
    }

    // High fidelity between nodes (Cubic Hermite error < 5e-6 with 256 intervals)
    double maxErr = 0.0;
    for (int i = 0; i < 1000; ++i) {
        const double x = -4.9 + i * (9.8 / 1000.0);
        const double approx = table.lookup(x);
        const double exact = std::tanh(x);
        maxErr = std::max(maxErr, std::abs(approx - exact));
    }
    CHECK(maxErr < 5e-6);

    // Linear extrapolation outside bounds
    const double valUnder = table.lookup(-6.0);
    const double valOver = table.lookup(6.0);
    CHECK(std::isfinite(valUnder));
    CHECK(std::isfinite(valOver));
    CHECK(valUnder < table.lookup(-5.0));
    CHECK(valOver > table.lookup(5.0));
}
