// Which parts of a drawing are joined to nothing, and what that means for the
// solver.
//
// **The failure this exists for.** A WDF engine solves a linear network before
// it can solve anything nonlinear, and that solve is singular when a node has
// no conductance path -- which is what a node with only one thing on it has.
// The engine's own answer -- "rootTransfer failed (singular linear network)"
// -- is true and useless to somebody looking at a drawing: it does not say
// *which* part, or *which terminal*.
//
// So the same question is asked here, in the drawing's terms, and the answer
// is checked against the thing it is supposed to predict: **does the engine
// reject the circuit?**

#include "format/LiveWDFFile.h"
#include "wdf/Engine.h"

#include <catch2.hpp>

#include <algorithm>
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

/// The RC lowpass: an Input terminal, a resistor, a capacitor and a ground.
LiveWDFDocument rcLowpass() {
    LiveWDFDocument doc;

    doc.components.push_back(part("IN1", "in", {{"sig", "in"}}));
    doc.components.push_back(part("R1", "res", {{"a", "in"}, {"b", "mid"}},
                                  {{"value", 1000.0}}));
    doc.components.push_back(part("C1", "cap", {{"a", "mid"}, {"b", "0"}},
                                  {{"value", 1e-6}}));
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

}  // namespace

TEST_CASE("A circuit whose every part is connected has nothing to report",
          "[connectivity]") {
    CHECK(rcLowpass().unconnectedPins().empty());
}

TEST_CASE("A part with one end in mid-air is reported", "[connectivity]") {
    // A resistor whose far end goes nowhere: the node at that end has nothing
    // else attached to it.
    LiveWDFDocument doc;
    doc.components.push_back(part("IN1", "in", {{"sig", "in"}}));
    doc.components.push_back(part("R1", "res", {{"a", "in"}, {"b", "0"}},
                                  {{"value", 1000.0}}));
    doc.components.push_back(part("R2", "res", {{"a", "in"}, {"b", "n_R2_b"}},
                                  {{"value", 1000.0}}));

    const auto unconnected = doc.unconnectedPins();

    REQUIRE(unconnected.size() == 1);
    CHECK(unconnected.front().first == "R2");
    CHECK(unconnected.front().second == "b");
}

TEST_CASE("A part joined to nothing at all is an island, and comes first",
          "[connectivity]") {
    // Two problems at once: the capacitor is an island, and the resistor's far
    // end goes nowhere. The island is the one the solver cannot do without, so
    // it is the one a message names.
    LiveWDFDocument doc;
    doc.components.push_back(part("IN1", "in", {{"sig", "in"}}));
    doc.components.push_back(part("R1", "res", {{"a", "in"}, {"b", "n_R1_b"}},
                                  {{"value", 1000.0}}));
    doc.components.push_back(part("C1", "cap", {{"a", "n_C1_a"}, {"b", "n_C1_b"}},
                                  {{"value", 1e-6}}));

    const auto unconnected = doc.unconnectedPins();

    REQUIRE(unconnected.size() >= 2);
    CHECK(unconnected.front().first == "C1");
}

TEST_CASE("The ground is a reference, not a connection", "[connectivity]") {
    // A source with nothing else on it: its negative terminal is on the
    // ground, and the ground is where every circuit's reference lives.
    LiveWDFDocument doc;
    doc.components.push_back(part("S1", "src", {{"pos", "out"}, {"neg", "0"}},
                                  {{"dc", 1.0}}));

    CHECK(doc.unconnectedPins().empty());
}

TEST_CASE("A source's own terminal is not unconnected", "[connectivity]") {
    // A voltage source's two terminals are the same node at DC, so a source
    // with nothing on its far end drives nothing -- which is legal, and not the
    // engine's business. The Input terminal is the same case: it becomes a
    // source.
    LiveWDFDocument doc;
    doc.components.push_back(part("IN1", "in", {{"sig", "in"}}));
    doc.components.push_back(part("S1", "src", {{"pos", "out"}, {"neg", "0"}},
                                  {{"dc", 1.0}}));

    CHECK(doc.unconnectedPins().empty());
}

TEST_CASE("A tube's plate with no load is reported", "[connectivity]") {
    // The classic: a triode whose plate goes nowhere. The tube draws current
    // into the plate, so the node's equation has no conductance to work with.
    //
    // The grid is unconnected too, in this drawing: nothing drives it. Both
    // are reported, and both are true.
    LiveWDFDocument doc;
    doc.components.push_back(part("B1", "src", {{"pos", "bplus"}, {"neg", "0"}},
                                  {{"dc", 300.0}}));
    doc.components.push_back(part("V1", "triode",
                                  {{"plate", "plate"}, {"grid", "grid"}, {"cathode", "0"}}));

    const auto unconnected = doc.unconnectedPins();

    REQUIRE(unconnected.size() == 2);
    CHECK(unconnected.front().first == "V1");
}

TEST_CASE("An unconnected part is left out of the circuit, and reported",
          "[connectivity]") {
    // **The promise this analysis makes, checked against the engine.** A part
    // joined to nothing *at all* is not in the circuit -- the rest of it still
    // solves -- and the editor is told which part it left out.
    //
    // This is the test that says the diagnosis is worth anything: if the two
    // ever disagree, the message the editor shows would be a lie.
    const auto good = rcLowpass();
    CHECK(compiles(good));
    CHECK(good.unconnectedPins().empty());

    // The capacitor on its own: both of its terminals are on nets of their
    // own, so the capacitor is an island.
    LiveWDFDocument island;
    island.components.push_back(part("C1", "cap", {{"a", "n_C1_a"}, {"b", "n_C1_b"}},
                                     {{"value", 1e-6}}));

    // **It is skipped, not refused.** A part that is joined to nothing is not
    // connected to the circuit it is drawn on; compiling it in would make the
    // solver's matrix singular (the part's own two terminals are joined by
    // nothing but the part), and refusing the whole circuit over a part the
    // user has deliberately pulled out would be wrong.
    CHECK(compiles(island));

    // And it is reported, by name.
    CHECK(island.unconnectedPins().size() == 2);
    CHECK(island.isolatedParts().count("C1") == 1);

    // A part with a loose end is *not* refused by the engine either -- so the
    // finding is a warning there, and not an error.
    LiveWDFDocument loose;
    loose.components.push_back(part("IN1", "in", {{"sig", "in"}}));
    loose.components.push_back(part("R1", "res", {{"a", "in"}, {"b", "0"}},
                                    {{"value", 1000.0}}));
    loose.components.push_back(part("R2", "res", {{"a", "in"}, {"b", "n_R2_b"}},
                                    {{"value", 1000.0}}));

    CHECK(compiles(loose));
    CHECK(loose.unconnectedPins().size() == 1);
}

TEST_CASE("An op-amp on a 9 V single supply clips inside that supply") {
    LiveWDFDocument doc;
    doc.components.push_back(part("U1", "opamp", {{"out", "out"}, {"neg", "out"}, {"pos", "in"},
                                                  {"vcc", "v9"}, {"vee", "0"}}));
    doc.components.push_back(part("IN1", "in", {{"sig", "in"}}));
    doc.components.push_back(part("V9", "rail", {{"pos", "v9"}}, {{"dc", 9.0}}));
    const auto circuit = doc.toCircuit();

    REQUIRE(circuit.opamp.size() == 1);
    CHECK(circuit.opamp[0].model.railLow() == Approx(1.5));
    CHECK(circuit.opamp[0].model.railHigh() == Approx(7.5));
}
