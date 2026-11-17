// Tests for the editor's `CircuitSession`.
//
// Why this exists: the session is the editor's only real logic -- it turns a
// `.livewdf` document into a live engine and answers DC / step queries for the
// DC table, the probe badges and the scope. Everything else in `editor/` is
// drawing. Checking the session headlessly here means the editor's numbers are
// pinned by the same kind of test the engine uses, rather than only by looking at
// the window.

#include <catch2.hpp>

#include "CircuitSession.h"
#include "format/LiveWDFFile.h"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace {

/// The canonical cathode-biased 12AX7 stage -- the same document
/// `test_livewdf_file.cpp` round-trips, reused here so the editor's session is
/// checked against a circuit that is already known-good elsewhere.
wdf::LiveWDFDocument canonicalTubeStage() {
    wdf::LiveWDFDocument doc;
    doc.name = "Test Tube Clipper";
    doc.inputNode = "in";
    doc.outputNode = "out";

    wdf::SchematicComponent src;
    src.id = "Vin"; src.type = "src";
    src.pins = {{"pos", "in"}, {"neg", "0"}};
    src.values = {{"dc", 0.0}, {"ac", 1.0}};
    doc.components.push_back(src);

    wdf::SchematicComponent rin;
    rin.id = "R_in"; rin.type = "res";
    rin.pins = {{"a", "in"}, {"b", "grid"}};
    rin.values = {{"value", 68000.0}};
    doc.components.push_back(rin);

    wdf::SchematicComponent pot;
    pot.id = "Gain"; pot.type = "pot";
    pot.pins = {{"high", "grid"}, {"wiper", "v_grid"}, {"low", "0"}};
    pot.values = {{"totalR", 1000000.0}, {"pos", 0.75}};
    pot.properties = {{"taper", "Log"}, {"label", "Drive"}};
    doc.components.push_back(pot);

    wdf::SchematicComponent tri;
    tri.id = "V1"; tri.type = "triode";
    tri.pins = {{"grid", "v_grid"}, {"plate", "out"}, {"cathode", "k"}};
    tri.properties = {{"model", "12AX7"}};
    doc.components.push_back(tri);

    wdf::SchematicComponent rk;
    rk.id = "R_k"; rk.type = "res";
    rk.pins = {{"a", "k"}, {"b", "0"}};
    rk.values = {{"value", 1500.0}};
    doc.components.push_back(rk);

    wdf::SchematicComponent ck;
    ck.id = "C_k"; ck.type = "cap";
    ck.pins = {{"a", "k"}, {"b", "0"}};
    ck.values = {{"value", 22e-6}};
    doc.components.push_back(ck);

    wdf::SchematicComponent bplus;
    bplus.id = "V_Bplus"; bplus.type = "src";
    bplus.pins = {{"pos", "bplus"}, {"neg", "0"}};
    bplus.values = {{"dc", 250.0}};
    doc.components.push_back(bplus);

    wdf::SchematicComponent rp;
    rp.id = "R_plate"; rp.type = "res";
    rp.pins = {{"a", "bplus"}, {"b", "out"}};
    rp.values = {{"value", 100000.0}};
    doc.components.push_back(rp);

    wdf::SchematicComponent cout;
    cout.id = "C_out"; cout.type = "cap";
    cout.pins = {{"a", "out"}, {"b", "jack_out"}};
    cout.values = {{"value", 22e-9}};
    doc.components.push_back(cout);

    wdf::SchematicComponent rload;
    rload.id = "R_load"; rload.type = "res";
    rload.pins = {{"a", "jack_out"}, {"b", "0"}};
    rload.values = {{"value", 1000000.0}};
    doc.components.push_back(rload);

    return doc;
}

}  // namespace

TEST_CASE("CircuitSession compiles a document BJT stage", "[editor][bjt]") {
    // The engine has a BJT device, and the document converter now has a `bjt`
    // branch. This is the seam between them: a part the palette can place has to
    // become a device the session can solve. A common-emitter bias divider is
    // enough to put the transistor in forward active and make a wrong collector
    // or base node obvious in the DC table.
    wdf::LiveWDFDocument doc;
    doc.name = "BJT stage";
    doc.inputNode = "in";
    doc.outputNode = "c";

    wdf::SchematicComponent vcc;
    vcc.id = "Vcc"; vcc.type = "src";
    vcc.pins = {{"pos", "vcc"}, {"neg", "0"}};
    vcc.values = {{"dc", 12.0}};
    doc.components.push_back(vcc);

    wdf::SchematicComponent rb1;
    rb1.id = "Rb1"; rb1.type = "res";
    rb1.pins = {{"a", "vcc"}, {"b", "b"}};
    rb1.values = {{"value", 4.7e6}};
    doc.components.push_back(rb1);

    wdf::SchematicComponent rb2;
    rb2.id = "Rb2"; rb2.type = "res";
    rb2.pins = {{"a", "b"}, {"b", "0"}};
    rb2.values = {{"value", 1.0e6}};
    doc.components.push_back(rb2);

    wdf::SchematicComponent rc;
    rc.id = "Rc"; rc.type = "res";
    rc.pins = {{"a", "vcc"}, {"b", "c"}};
    rc.values = {{"value", 4.7e3}};
    doc.components.push_back(rc);

    wdf::SchematicComponent q;
    q.id = "Q1"; q.type = "bjt";
    q.pins = {{"collector", "c"}, {"base", "b"}, {"emitter", "0"}};
    q.properties = {{"model", "2N3904"}};
    doc.components.push_back(q);

    livewdf::editor::CircuitSession session;
    REQUIRE(session.compile(doc, 48000.0));
    REQUIRE(session.valid());
    REQUIRE(session.lastError().empty());
    REQUIRE(session.dcConverged());

    const auto& v = session.dcVoltages();
    REQUIRE(v.count("b") == 1);
    REQUIRE(v.count("c") == 1);

    // A biased NPN sits with its base about a junction drop above the emitter
    // and its collector well above the base. If the converter had grounded the
    // base or swapped collector/emitter, one of these would fail loudly.
    CHECK(v.at("b") > 0.4);
    CHECK(v.at("b") < 1.2);
    CHECK(v.at("c") > v.at("b") + 1.0);
    CHECK(v.at("c") < 12.0);
}

TEST_CASE("CircuitSession compiles and solves the canonical tube stage", "[editor]") {
    livewdf::editor::CircuitSession session;

    REQUIRE(session.compile(canonicalTubeStage(), 48000.0));
    REQUIRE(session.valid());
    REQUIRE(session.lastError().empty());

    // The DC solve must converge, and the plate node must exist and sit at a
    // believable high voltage for a 250 V supply through a 100k plate load.
    REQUIRE(session.dcConverged());
    const auto& v = session.dcVoltages();
    REQUIRE(v.count("out") == 1);
    REQUIRE(v.at("out") > 100.0);
    REQUIRE(v.at("out") < 250.0);

    // The supply is a voltage-source node, so it is *not* a free node and must
    // not appear in the table -- the same counting rule the engine uses.
    REQUIRE(v.count("bplus") == 0);

    // A cathode-biased stage lifts the cathode a little above ground; a solve
    // that left it at 0 would mean the tube is not conducting.
    REQUIRE(v.count("k") == 1);
    REQUIRE(v.at("k") > 0.1);

    // Source identification: the audio drive is `Vin` (it has ac != 0), not the
    // 250 V rail, so the scope drives the right one.
    REQUIRE(session.inputSourceIndex() >= 0);
    REQUIRE(session.inputSourceName() == "Vin");

    // The output node maps to a real index in `step()`'s return.
    REQUIRE(session.outputIndex() >= 0);
    REQUIRE(session.outputIndex() < static_cast<int>(session.nodeNames().size()));
}

TEST_CASE("CircuitSession step returns node voltages and reacts to input", "[editor]") {
    livewdf::editor::CircuitSession session;
    REQUIRE(session.compile(canonicalTubeStage(), 48000.0));

    const int outIdx = session.outputIndex();
    REQUIRE(outIdx >= 0);

    // Settle first, then drive. A step must be finite and the output must move
    // when the input moves -- which is the whole point of the scope trace.
    for (int i = 0; i < 200; ++i)
        session.step(0.0);

    const double base = session.step(0.0)[static_cast<size_t>(outIdx)];
    REQUIRE(std::isfinite(base));

    // A large positive grid swing drives the plate down (inverting stage); a
    // peak of the test signal must therefore move the output.
    double moved = 0.0;
    for (int i = 0; i < 64; ++i) {
        const double vin = 0.05 * std::sin(2.0 * 3.14159265358979 * 440.0 * i / 48000.0);
        const double out = session.step(vin)[static_cast<size_t>(outIdx)];
        REQUIRE(std::isfinite(out));
        moved = std::max(moved, std::abs(out - base));
    }
    REQUIRE(moved > 1e-4);
}

TEST_CASE("CircuitSession compiles and biases an op-amp stage", "[editor][opamp]") {
    // The palette can place an op-amp, so the session must be able to compile
    // one. Before the bias pass existed, the op-amp branch set no operating
    // point at all; this checks the whole seam: document -> bias-derived model
    // fields -> compiled engine -> DC table.
    wdf::LiveWDFDocument doc;
    doc.name = "Single-supply op-amp stage";
    doc.inputNode = "sig";
    doc.outputNode = "out";

    auto add = [&](wdf::SchematicComponent c) {
        doc.components.push_back(std::move(c));
    };

    wdf::SchematicComponent vcc;
    vcc.id = "VCC"; vcc.type = "rail";
    vcc.pins = {{"pos", "9v"}};
    vcc.values = {{"dc", 9.0}};
    add(vcc);

    for (const auto& [id, a, b, ohms] : {
             std::tuple{"R1", "9v", "vbias", 10000.0},
             std::tuple{"R2", "vbias", "0", 10000.0},
             std::tuple{"Rbias", "in", "vbias", 1000000.0},
             std::tuple{"Ri", "in", "neg", 10000.0},
             std::tuple{"Rf", "neg", "out", 100000.0},
             std::tuple{"Rload", "out", "0", 10000.0}}) {
        wdf::SchematicComponent r;
        r.id = id; r.type = "res";
        r.pins = {{"a", a}, {"b", b}};
        r.values = {{"value", ohms}};
        add(r);
    }

    wdf::SchematicComponent vin;
    vin.id = "Vin"; vin.type = "src";
    vin.pins = {{"pos", "sig"}, {"neg", "0"}};
    vin.values = {{"dc", 0.0}, {"ac", 1.0}};
    vin.properties = {{"mode", "AC"}};
    add(vin);

    wdf::SchematicComponent cin;
    cin.id = "Cin"; cin.type = "cap";
    cin.pins = {{"a", "sig"}, {"b", "in"}};
    cin.values = {{"value", 100e-9}};
    add(cin);

    wdf::SchematicComponent u1;
    u1.id = "U1"; u1.type = "opamp";
    u1.pins = {{"out", "out"}, {"neg", "neg"}, {"pos", "vbias"},
               {"vcc", "9v"}, {"vee", "0"}};
    u1.properties = {{"model", "tl072"}};
    add(u1);

    livewdf::editor::CircuitSession session;
    REQUIRE(session.compile(doc, 48000.0));
    REQUIRE(session.valid());
    REQUIRE(session.lastError().empty());
    REQUIRE(session.dcConverged());

    const auto& v = session.dcVoltages();
    REQUIRE(v.count("vbias") == 1);
    REQUIRE(v.count("out") == 1);

    // The divider is at 4.5 V and the feedback holds the inverting input there.
    // The output sits a few tens of mV below it because the 10k load draws
    // current through the model's output resistance.
    CHECK(v.at("vbias") == Approx(4.5).margin(0.05));
    CHECK(v.at("out") > 4.4);
    CHECK(v.at("out") < 4.51);

    // A source node is not a free node and must not appear in the table.
    CHECK(v.count("9v") == 0);

    // The live path: an AC drive through the input coupling cap moves the
    // output. This is what the scope trace reads.
    const int outIdx = session.outputIndex();
    REQUIRE(outIdx >= 0);
    for (int i = 0; i < 400; ++i)
        session.step(0.0);

    const double base = session.step(0.0)[static_cast<size_t>(outIdx)];
    REQUIRE(std::isfinite(base));

    double moved = 0.0;
    for (int i = 0; i < 256; ++i) {
        const double vinVolts =
            0.05 * std::sin(2.0 * 3.14159265358979 * 440.0 * i / 48000.0);
        const double out = session.step(vinVolts)[static_cast<size_t>(outIdx)];
        REQUIRE(std::isfinite(out));
        moved = std::max(moved, std::abs(out - base));
    }
    CHECK(moved > 1e-4);
}

TEST_CASE("The editor does not overwrite a signal source's own waveform", "[editor]") {
    // **The lab.** The user puts a signal source in AC mode -- a 1 Hz sine -- and
    // a scope probe somewhere, and expects to watch it. There used to be a
    // fallback that drove "the first source" with the editor's own 440 Hz test
    // signal, so the source's waveform was overwritten every sample and the
    // scope showed a signal the user had not asked for.
    //
    // So: the editor's test signal drives the *Input terminal's* source, or
    // nothing at all. A circuit with no Input terminal is excited by its own
    // sources, and the editor watches.
    wdf::LiveWDFDocument doc;
    doc.name = "AC lab";
    doc.inputNode = "in";
    doc.outputNode = "out";
    doc.groundNode = "0";

    wdf::SchematicComponent src;
    src.id = "V1";
    src.type = "src";
    src.pins = {{"pos", "n1"}, {"neg", "0"}};
    src.values = {{"dc", 0.0}, {"ac", 2.0}, {"freq", 1.0}};
    src.properties = {{"mode", "AC"}};
    doc.components.push_back(src);

    wdf::SchematicComponent res;
    res.id = "R1";
    res.type = "res";
    res.pins = {{"a", "n1"}, {"b", "0"}};
    res.values = {{"value", 1000.0}};
    doc.components.push_back(res);

    livewdf::editor::CircuitSession session;
    REQUIRE(session.compile(doc, 48000.0));

    // **No Input terminal, so nothing is driven.** The source is the user's own
    // excitation and must be left alone.
    CHECK(session.inputSourceIndex() < 0);

    // And the source's own sine is what goes into the circuit: the value on the
    // input source, which moves from sample to sample.
    double first = 0.0;
    double second = 0.0;
    for (int i = 0; i < 200; ++i) {
        session.step(0.0);
        if (i == 10) first = session.inputVolts();
        if (i == 20) second = session.inputVolts();
    }

    // The circuit is being driven by something that moves.
    CHECK(std::abs(first - second) > 1e-6);

    // And it is the *source's* waveform, not the editor's test signal: the
    // editor asked for zero volts and the circuit still moved.
    CHECK(std::abs(first) > 1e-6);
}

TEST_CASE("The editor's test signal drives the Input terminal's source", "[editor]") {
    wdf::LiveWDFDocument doc;
    doc.name = "Input lab";
    doc.inputNode = "in";
    doc.outputNode = "out";
    doc.groundNode = "0";

    wdf::SchematicComponent in;
    in.id = "IN1";
    in.type = "in";
    in.pins = {{"sig", "n1"}};
    doc.components.push_back(in);

    wdf::SchematicComponent res;
    res.id = "R1";
    res.type = "res";
    res.pins = {{"a", "n1"}, {"b", "0"}};
    res.values = {{"value", 1000.0}};
    doc.components.push_back(res);

    livewdf::editor::CircuitSession session;
    REQUIRE(session.compile(doc, 48000.0));

    // The Input terminal's source is found by name, not guessed.
    CHECK(session.inputSourceIndex() >= 0);
    CHECK(session.inputSourceName() == "IN1");

    // And driving it moves what is going into the circuit.
    session.step(0.0);
    const double quiet = session.inputVolts();
    session.step(1.0);
    const double loud = session.inputVolts();

    CHECK(std::abs(quiet - loud) > 1e-6);
}

TEST_CASE("The scope probe names the node the scope traces", "[editor]") {
    // The scope probe is the editor's "look here": it stamps nothing into the
    // circuit, it only names a net, and the scope traces *that* net instead of
    // the output. A document with no probe leaves the scope on the output.
    auto doc = canonicalTubeStage();

    livewdf::editor::CircuitSession session;
    REQUIRE(session.compile(doc, 48000.0));

    // No probe yet: the scope shows the output.
    CHECK(session.probeIndex() < 0);
    CHECK(session.probeLabel().empty());

    // Put a probe on the plate node and compile again.
    wdf::SchematicComponent probe;
    probe.id = "SC1";
    probe.type = "scope";
    probe.pins = {{"probe", "out"}, {"ref", "0"}};
    doc.components.push_back(probe);

    livewdf::editor::CircuitSession withProbe;
    REQUIRE(withProbe.compile(doc, 48000.0));

    // The probe resolves to a real node, and it is the *plate* node -- the same
    // one the output is, so the scope has not moved in this case.
    CHECK(withProbe.probeIndex() >= 0);
    CHECK(withProbe.probeIndex() < static_cast<int>(withProbe.nodeNames().size()));
    CHECK(withProbe.probeLabel() == "SC1");

    // A probe on a different node moves the scope's eye there -- but only when
    // it is the one being watched. With nothing selected, the first probe on
    // the sheet is the one the scope looks at.
    wdf::SchematicComponent other;
    other.id = "SC2";
    other.type = "scope";
    other.pins = {{"probe", "k"}, {"ref", "0"}};
    doc.components.push_back(other);

    livewdf::editor::CircuitSession twoProbes;
    REQUIRE(twoProbes.compile(doc, 48000.0));
    CHECK(twoProbes.probeIndex() == withProbe.probeIndex());   // the first one
    CHECK(twoProbes.probeLabel() == "SC1");

    // Selecting the second probe moves the scope's eye to it. That is the whole
    // mechanism: the probe stamps nothing, so the selection is the only way to
    // say which one you mean.
    twoProbes.watchProbe("SC2", "k");
    CHECK(twoProbes.probeIndex() != withProbe.probeIndex());
    CHECK(twoProbes.probeLabel() == "SC2");
}

TEST_CASE("CircuitSession reports an error instead of crashing on a bad document",
          "[editor]") {
    livewdf::editor::CircuitSession session;

    // A triode with no plate path to any supply: the nodes exist but the circuit
    // is not solvable to a useful operating point. The session must not throw,
    // must not report valid, and must leave a message for the status line.
    wdf::LiveWDFDocument doc;
    doc.name = "Dangling";
    doc.inputNode = "in";
    doc.outputNode = "out";

    wdf::SchematicComponent src;
    src.id = "Vin"; src.type = "src";
    src.pins = {{"pos", "in"}, {"neg", "0"}};
    src.values = {{"dc", 0.0}};
    doc.components.push_back(src);

    wdf::SchematicComponent tri;
    tri.id = "V1"; tri.type = "triode";
    tri.pins = {{"grid", "in"}, {"plate", "out"}, {"cathode", "0"}};
    tri.properties = {{"model", "12AX7"}};
    doc.components.push_back(tri);

    // Whatever the outcome, this must not throw and the session must still be
    // usable afterwards (an empty step, not a crash).
    const bool ok = session.compile(doc, 48000.0);
    if (ok) {
        REQUIRE(session.valid());
        // An unsolvable circuit is legal to compile; the DC table says so.
        REQUIRE_FALSE(session.dcStatus().empty());
    } else {
        REQUIRE_FALSE(session.valid());
        REQUIRE_FALSE(session.lastError().empty());
    }

    session.clear();
    REQUIRE_FALSE(session.valid());
    const auto& nodes = session.step(0.0);
    REQUIRE(nodes.empty());
}

TEST_CASE("The session lists every scope probe on the sheet", "[session][probe]") {
    // **The scope draws a trace per probe**, each on its own colour. So the
    // session has to hand the window every probe, not just the selected one.
    auto doc = canonicalTubeStage();

    wdf::SchematicComponent p1;
    p1.id = "M1"; p1.type = "scope";
    p1.pins = {{"probe", "grid"}, {"ref", "0"}};
    doc.components.push_back(p1);

    wdf::SchematicComponent p2;
    p2.id = "M2"; p2.type = "scope";
    p2.pins = {{"probe", "out"}, {"ref", "0"}};
    doc.components.push_back(p2);

    livewdf::editor::CircuitSession session;
    REQUIRE(session.compile(doc, 48000.0));

    const auto& traces = session.probeTraces();

    INFO("the sheet has " << traces.size() << " probes");
    REQUIRE(traces.size() == 2);

    // The ids come back in the sheet's order, and each has a node to read.
    CHECK(traces[0].first == "M1");
    CHECK(traces[1].first == "M2");
    for (const auto& [id, idx] : traces) {
        INFO("the probe " << id << " reads node " << idx);
        CHECK(idx >= 0);
    }
}

TEST_CASE("A sheet with no probe has no probe traces", "[session][probe]") {
    livewdf::editor::CircuitSession session;
    REQUIRE(session.compile(canonicalTubeStage(), 48000.0));
    CHECK(session.probeTraces().empty());
}
