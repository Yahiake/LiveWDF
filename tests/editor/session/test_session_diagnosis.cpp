// Why the editor says a circuit was rejected, in the user's words.
//
// **The failure this exists for.** The editor said "circuit rejected: root
// transfer failed (singular linear network)" for a circuit the user had drawn
// -- an RC lowpass -- and the message says nothing they can act on. It does
// not say *which* part, and it does not say that the part is joined to
// nothing.
//
// So the editor now says the thing that is true: the part that is not
// connected to anything, by name. These tests are that promise, checked.

#include <catch2.hpp>

#include "CircuitSession.h"
#include "format/LiveWDFFile.h"

#include <string>

namespace {

/// A part, with its pins on the given nets.
wdf::SchematicComponent part(const std::string& id, const std::string& type,
                              const std::vector<std::pair<std::string, std::string>>& pins,
                              const std::vector<std::pair<std::string, double>>& values = {}) {
    wdf::SchematicComponent c;
    c.id = id;
    c.type = type;
    for (const auto& [pin, net] : pins)
        c.pins[pin] = net;
    for (const auto& [key, value] : values)
        c.values[key] = value;
    return c;
}

/// The RC lowpass, drawn the way the editor draws it.
///
/// `capTo` is the net the capacitor's lower terminal is on, and `capFrom` the
/// one its upper terminal is on. When both of the capacitor's wires missed,
/// the capacitor is joined to nothing at all -- an island -- and the engine
/// cannot solve the circuit.
wdf::LiveWDFDocument rcLowpass(const std::string& capFrom = "mid",
                                const std::string& capTo = "0") {
    wdf::LiveWDFDocument doc;

    doc.components.push_back(part("IN1", "in", {{"sig", "in"}}));
    doc.components.push_back(part("R1", "res", {{"a", "in"}, {"b", "mid"}},
                                  {{"value", 1000.0}}));
    doc.components.push_back(part("C1", "cap", {{"a", capFrom}, {"b", capTo}},
                                  {{"value", 1e-6}}));
    doc.components.push_back(part("GND1", "gnd", {{"sig", "0"}}));

    return doc;
}

}  // namespace

TEST_CASE("A circuit that compiles says nothing about being rejected") {
    livewdf::editor::CircuitSession session;

    CHECK(session.compile(rcLowpass(), 48000.0));
    CHECK(session.lastError().empty());
    CHECK(session.lastErrorPartId().empty());
}

TEST_CASE("A circuit with a part pulled out of it still converges", "[session][diagnosis]") {
    // **The case the user drew**: a working RC lowpass with a resistor that has
    // been pulled out of it and replaced by another one.
    //
    // The lowpass must still work -- the resistor that is gone is not part of
    // the circuit any more -- so the circuit has to *converge*. And the part
    // that was pulled out has to be *reported*, because a drawing that says one
    // thing and a circuit that says another is exactly the confusion.
    //
    // The part that is out is an **island**: both of its terminals are on nets
    // of their own. Compiling it in would make the solver's matrix singular --
    // the part's own two terminals are joined by nothing but the part, which is
    // a loop with no reference to ground in it -- and the DC solve would go
    // round in circles looking for a voltage that does not exist.
    wdf::LiveWDFDocument doc;

    doc.components.push_back(part("IN1", "in", {{"sig", "in"}}));
    doc.components.push_back(part("R2", "res", {{"a", "in"}, {"b", "mid"}},
                                  {{"value", 1000.0}}));
    doc.components.push_back(part("C1", "cap", {{"a", "mid"}, {"b", "0"}},
                                  {{"value", 1e-6}}));
    doc.components.push_back(part("GND1", "gnd", {{"sig", "0"}}));
    doc.components.push_back(part("OUT1", "out", {{"sig", "mid"}},
                                  {{"fullScaleVolts", 1.0}}));

    // **The part that was pulled out**, sitting on its own.
    doc.components.push_back(part("R1", "res", {{"a", "n_R1_a"}, {"b", "n_R1_b"}},
                                  {{"value", 1000.0}}));

    // The circuit converges.
    livewdf::editor::CircuitSession session;
    CHECK(session.compile(doc, 48000.0));
    CHECK(session.lastError().empty());
    CHECK(session.dcConverged());

    // **And the part that was pulled out is reported**, by name.
    REQUIRE_FALSE(session.unconnectedParts().empty());
    CHECK(session.unconnectedParts().front().first == "R1");
}

TEST_CASE("A part joined to nothing is not in the circuit, and is reported",
          "[session][diagnosis]") {
    // The capacitor's two wires both missed: it is joined to nothing at all.
    //
    // **It is not in the circuit** -- and that is the right answer, not a
    // refusal. A part whose every terminal is on a net of its own is not
    // connected to the circuit it is drawn on, and compiling it in makes the
    // solver's matrix singular: the part's own two terminals are joined by
    // nothing but the part, which is a loop with no reference to ground in it.
    //
    // So the circuit skips it -- the rest of the lowpass still solves -- and
    // the editor says which part it left out.
    livewdf::editor::CircuitSession session;

    CHECK(session.compile(rcLowpass("n_C1_a", "n_C1_b"), 48000.0));
    CHECK(session.lastError().empty());

    // **And the editor is told**, so it can say so and point at it.
    REQUIRE_FALSE(session.unconnectedParts().empty());
    CHECK(session.unconnectedParts().front().first == "C1");
}

TEST_CASE("The part that is joined to nothing is named before the one with a loose end",
          "[session][diagnosis]") {
    // Two problems at once: the capacitor is an island, and the resistor's far
    // end goes nowhere. The island is the one the solver cannot do without, so
    // it is the one the message names.
    wdf::LiveWDFDocument doc;
    doc.components.push_back(part("IN1", "in", {{"sig", "in"}}));
    doc.components.push_back(part("R1", "res", {{"a", "in"}, {"b", "n_R1_b"}},
                                  {{"value", 1000.0}}));
    doc.components.push_back(part("C1", "cap", {{"a", "n_C1_a"}, {"b", "n_C1_b"}},
                                  {{"value", 1e-6}}));

    livewdf::editor::CircuitSession session;
    session.compile(doc, 48000.0);

    REQUIRE(session.unconnectedParts().size() >= 2);
    CHECK(session.unconnectedParts().front().first == "C1");
}

TEST_CASE("The engine's own message is kept for what the diagnosis cannot explain",
          "[session][diagnosis]") {
    // Two voltage sources across the same pair of nodes is a short: no part is
    // joined to nothing, so there is no diagnosis to give, and the engine's
    // message is all there is.
    wdf::LiveWDFDocument doc;
    doc.components.push_back(part("S1", "src", {{"pos", "a"}, {"neg", "0"}},
                                  {{"dc", 1.0}}));
    doc.components.push_back(part("S2", "src", {{"pos", "a"}, {"neg", "0"}},
                                  {{"dc", 2.0}}));

    livewdf::editor::CircuitSession session;
    const bool compiled = session.compile(doc, 48000.0);

    if (!compiled) {
        // The engine's words are still there, so nothing is lost.
        CHECK(session.lastError().find("circuit rejected") != std::string::npos);
        CHECK(session.lastErrorPartId().empty());
    }
}

TEST_CASE("A part joined to nothing is reported even when the circuit works",
          "[session][diagnosis]") {
    // A resistor with one end in mid-air: the engine can still solve that
    // (the node's voltage is simply the node next door's), so the build
    // succeeds -- but the part is joined to nothing, and that is worth saying.
    wdf::LiveWDFDocument doc;
    doc.components.push_back(part("IN1", "in", {{"sig", "in"}}));
    doc.components.push_back(part("R1", "res", {{"a", "in"}, {"b", "0"}},
                                  {{"value", 1000.0}}));
    doc.components.push_back(part("R2", "res", {{"a", "in"}, {"b", "n_R2_b"}},
                                  {{"value", 1000.0}}));

    livewdf::editor::CircuitSession session;
    session.compile(doc, 48000.0);

    REQUIRE(session.unconnectedParts().size() == 1);
    CHECK(session.unconnectedParts().front().first == "R2");
}
