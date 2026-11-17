// Which drawings of an RC lowpass the engine accepts, and which it refuses.
//
// **Why this is a test and not a script.** The editor said "circuit rejected:
// root transfer failed (singular linear network)" for a circuit the user had
// drawn, and the message says nothing about *what* is wrong. So the engine was
// asked, over a spread of drawings that all mean "an RC lowpass", which ones it
// refuses -- and the answers are here, so the editor's diagnosis can be
// checked against them rather than guessed at.
//
// **What the engine actually refuses.** Not a floating part: a resistor with
// one end in mid-air is fine (the node's voltage is simply the node next
// door's). What it refuses is a node with **no conductance path**: a tube
// whose plate goes nowhere (a tube is a root, not a conductance), and a
// capacitor joined to nothing at all (its two nodes have only each other, and
// the pair is singular).
//
// **What it does not refuse.** A lowpass with the capacitor's far end left
// open: a capacitor is a port in the wave domain, not a branch, so the node
// beyond it is a stub, and a stub is legal.

#include "format/LiveWDFFile.h"
#include "wdf/Engine.h"

#include <catch2.hpp>

#include <string>
#include <vector>

using namespace wdf;

namespace {

/// A part, with its pins on the given nets.
SchematicComponent part(const std::string& id, const std::string& type,
                         const std::vector<std::pair<std::string, std::string>>& pins,
                         const std::vector<std::pair<std::string, double>>& values = {}) {
    SchematicComponent c;
    c.id = id;
    c.type = type;
    for (const auto& [pin, net] : pins)
        c.pins[pin] = net;
    for (const auto& [key, value] : values)
        c.values[key] = value;
    return c;
}

/// The RC lowpass, drawn the way the editor draws it: an Input terminal, a
/// resistor, a capacitor and a ground symbol.
///
/// `capBottom` is the net the capacitor's lower terminal is on, so a drawing
/// can leave it floating ("n_CAP1_b") or land it on the ground ("0").
LiveWDFDocument rcLowpass(const std::string& capBottom = "0",
                           double capValue = 1e-6) {
    LiveWDFDocument doc;

    doc.components.push_back(part("IN1", "in", {{"sig", "in"}}));
    doc.components.push_back(part("R1", "res", {{"a", "in"}, {"b", "mid"}},
                                   {{"value", 1000.0}}));
    doc.components.push_back(part("C1", "cap", {{"a", "mid"}, {"b", capBottom}},
                                   {{"value", capValue}}));
    doc.components.push_back(part("GND1", "gnd", {{"sig", "0"}}));

    return doc;
}

/// Does the engine take this document?
bool compiles(const LiveWDFDocument& doc) {
    try {
        const auto circuit = doc.toCircuit();
        const Engine engine(circuit, 48000.0);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

/// The message the engine gives, for the report.
std::string refusal(const LiveWDFDocument& doc) {
    try {
        const auto circuit = doc.toCircuit();
        const Engine engine(circuit, 48000.0);
        return "accepted";
    } catch (const std::exception& e) {
        return e.what();
    }
}

}  // namespace

TEST_CASE("The RC lowpass the editor draws compiles", "[lowpass]") {
    // The drawing the editor opens on: an Input terminal, a resistor, a
    // capacitor and a ground symbol.
    const auto doc = rcLowpass();
    INFO(refusal(doc));
    CHECK(compiles(doc));
}

TEST_CASE("An RC lowpass whose capacitor is left floating still compiles",
          "[lowpass]") {
    // A capacitor is a port in the wave domain, not a branch: the node beyond
    // it is a stub, and a stub is legal. So a lowpass with nothing on the far
    // side of the capacitor is *not* what the engine refuses -- and a
    // diagnosis that said it was would be a lie.
    const auto doc = rcLowpass("n_CAP1_b");
    INFO(refusal(doc));
    CHECK(compiles(doc));
}

TEST_CASE("A capacitor with a value of zero still compiles", "[lowpass]") {
    // A capacitor with no capacitance is a port with no conductance -- but the
    // node it is on still has the resistor's, so the matrix is not singular.
    const auto doc = rcLowpass("0", 0.0);
    INFO(refusal(doc));
    CHECK(compiles(doc));
}

TEST_CASE("A capacitor joined to nothing at all is left out of the circuit",
          "[lowpass]") {
    // The drawing that says "an RC lowpass" with *both* of the capacitor's
    // wires missed: the capacitor is an island.
    //
    // **It is left out, not refused.** A part that is joined to nothing is not
    // connected to the circuit it is drawn on, and refusing the whole circuit
    // over a part the user has deliberately pulled out would be wrong. The rest
    // of the lowpass still solves.
    LiveWDFDocument doc;
    doc.components.push_back(part("IN1", "in", {{"sig", "in"}}));
    doc.components.push_back(part("R1", "res", {{"a", "in"}, {"b", "mid"}},
                                   {{"value", 1000.0}}));
    doc.components.push_back(part("C1", "cap", {{"a", "n_C1_a"}, {"b", "n_C1_b"}},
                                   {{"value", 1e-6}}));

    INFO(refusal(doc));
    CHECK(compiles(doc));

    // And the editor is told which part it left out.
    CHECK(doc.isolatedParts().count("C1") == 1);
}

TEST_CASE("Two capacitors in series with a resistor are accepted", "[lowpass]") {
    // The node between them is joined only by capacitors -- and it still has
    // the resistor's node next door to work with, so the matrix is not
    // singular.
    LiveWDFDocument doc;
    doc.components.push_back(part("IN1", "in", {{"sig", "in"}}));
    doc.components.push_back(part("R1", "res", {{"a", "in"}, {"b", "mid"}},
                                   {{"value", 1000.0}}));
    doc.components.push_back(part("C1", "cap", {{"a", "mid"}, {"b", "mid2"}},
                                   {{"value", 1e-6}}));
    doc.components.push_back(part("C2", "cap", {{"a", "mid2"}, {"b", "0"}},
                                   {{"value", 1e-6}}));

    INFO(refusal(doc));
    CHECK(compiles(doc));
}

TEST_CASE("A resistor with one end in mid-air is accepted", "[lowpass]") {
    // The node's voltage is simply the node next door's -- the resistor draws
    // no current, so there is nothing to solve for. The engine is content
    // with it; the *editor* says so, because it is nearly always a wire that
    // stopped short.
    LiveWDFDocument doc;
    doc.components.push_back(part("IN1", "in", {{"sig", "in"}}));
    doc.components.push_back(part("R1", "res", {{"a", "in"}, {"b", "0"}},
                                   {{"value", 1000.0}}));
    doc.components.push_back(part("R2", "res", {{"a", "in"}, {"b", "n_R2_b"}},
                                   {{"value", 1000.0}}));

    INFO(refusal(doc));
    CHECK(compiles(doc));
}

TEST_CASE("A triode with nothing on its plate is refused", "[lowpass]") {
    // The classic: a tube with no plate load. The tube is a *root*, not a
    // conductance, so its plate has no path to ground and the transfer is
    // singular.
    LiveWDFDocument doc;
    doc.components.push_back(part("IN1", "in", {{"sig", "in"}}));
    doc.components.push_back(part("V1", "triode",
                                   {{"plate", "plate"}, {"grid", "in"}, {"cathode", "0"}}));

    INFO(refusal(doc));
    CHECK_FALSE(compiles(doc));
}
