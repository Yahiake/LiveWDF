// SPDT (changeover) switch: one common, two throws, never both.
//
// The `switch_res` part is three pins: `in` (common), `outA`, `outB`. Its
// `select` property is "0" or "1", and `toCircuit` builds two ordinary
// resistors named <id>_A and <id>_B, toggled in opposition:
//   select = "0" → path A = 0.01 Ω (closed), path B = 1 GΩ (open)
//   select = "1" → path B = 0.01 Ω (closed), path A = 1 GΩ (open)
//
// Ganging: switch_res parts sharing a `label` are one changeover -- flipping
// one flips them all. That happens in `InspectorPanel::commitEdit`, which the
// inspector test below drives through the real panel.

#include "ElementAccess.h"
#include "InspectorPanel.h"
#include "format/PartLibrary.h"
#include "format/PartModels.h"
#include "SchematicModel.h"
#include "SymbolArtwork.h"

#include "CircuitSession.h"
#include "format/LiveWDFFile.h"

#include <catch2.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace livewdf::editor;

namespace {

/// Build a minimal `switch_res` document: the switch plus a source and a load
/// on each throw, so the circuit is solvable.
wdf::LiveWDFDocument spdtDoc(const std::string& select = "0",
                              const std::string& label = {}) {
    wdf::LiveWDFDocument doc;
    doc.name = "SPDT Test";
    doc.inputNode = "in";
    doc.outputNode = "out";

    wdf::SchematicComponent src;
    src.id = "Vin"; src.type = "src";
    src.pins = {{"pos", "in"}, {"neg", "0"}};
    src.values = {{"dc", 1.0}, {"ac", 1.0}};
    doc.components.push_back(src);

    wdf::SchematicComponent sw;
    sw.id = "S1"; sw.type = "switch_res";
    sw.pins = {{"in", "in"}, {"outA", "nA"}, {"outB", "nB"}};
    sw.properties = {{"select", select}};
    if (!label.empty())
        sw.properties["label"] = label;
    doc.components.push_back(sw);

    wdf::SchematicComponent ra;
    ra.id = "R_A"; ra.type = "res";
    ra.pins = {{"a", "nA"}, {"b", "out"}};
    ra.values = {{"value", 1000000.0}};
    doc.components.push_back(ra);

    wdf::SchematicComponent rb;
    rb.id = "R_B"; rb.type = "res";
    rb.pins = {{"a", "nB"}, {"b", "out"}};
    rb.values = {{"value", 680000.0}};
    doc.components.push_back(rb);

    wdf::SchematicComponent rout;
    rout.id = "R_out"; rout.type = "res";
    rout.pins = {{"a", "out"}, {"b", "0"}};
    rout.values = {{"value", 100000.0}};
    doc.components.push_back(rout);

    return doc;
}

/// Find a `TextButton` in a panel's tree, by matching its button text. The
/// inspector's rows live inside a viewport's content component now, so a direct
/// scan of the panel's children would miss the throw button.
juce::TextButton* findButton(juce::Component& panel, const juce::String& text) {
    for (auto* child : panel.getChildren()) {
        if (auto* btn = dynamic_cast<juce::TextButton*>(child))
            if (btn->getButtonText() == text)
                return btn;

        if (auto* nested = findButton(*child, text))
            return nested;
    }
    return nullptr;
}

juce::ComboBox* findComboWithItems(juce::Component& panel, int items) {
    for (auto* child : panel.getChildren()) {
        if (auto* box = dynamic_cast<juce::ComboBox*>(child); box && box->getNumItems() == items)
            return box;
        if (auto* nested = findComboWithItems(*child, items))
            return nested;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("switch_res is in the palette with three pins", "[editor]") {
    const PartSpec* spec = findPart("switch_res");
    REQUIRE(spec != nullptr);
    CHECK(spec->idPrefix == "SPDT");
    CHECK(spec->pins == std::vector<std::string>{"in", "outA", "outB"});
    CHECK(spec->pinSides == std::vector<int>{3, 0, 1});
    CHECK(spec->defaultValue == 0.0);
    CHECK(spec->valueKey.empty());
}

TEST_CASE("switch_res has artwork for both throws", "[editor]") {
    CHECK(hasArtwork("switch_res"));

    const SymbolArtwork* a = artworkForType("switch_res");
    REQUIRE(a != nullptr);
    CHECK_FALSE(a->isEmpty());

    wdf::SchematicComponent onA = makeComponent("switch_res", {}, 0.0f, 0.0f);
    wdf::SchematicComponent onB = onA;
    onB.properties["select"] = "1";

    const SymbolArtwork* artA = artworkForPart(onA);
    const SymbolArtwork* artB = artworkForPart(onB);
    REQUIRE(artA != nullptr);
    REQUIRE(artB != nullptr);
    CHECK(artA != artB);
    CHECK(artA->shapes.size() == artB->shapes.size());
}

TEST_CASE("switch_res pins match Celine's SPDT frame", "[editor]") {
    const auto pins = Schematic::pinOffsets("switch_res");
    REQUIRE(pins.size() == 3);
    CHECK(pins[0].x == 0);   CHECK(pins[0].y == 2);
    CHECK(pins[1].x == -1);  CHECK(pins[1].y == -2);
    CHECK(pins[2].x == 1);   CHECK(pins[2].y == -2);
}

TEST_CASE("switch_res defaults select to \"0\"", "[editor]") {
    wdf::SchematicComponent comp = makeComponent("switch_res", {}, 0.0f, 0.0f);
    REQUIRE(comp.properties.count("select") == 1);
    CHECK(comp.properties["select"] == "0");
}

TEST_CASE("switch_res extends its pin frame from the throw count", "[editor]") {
    auto sw = makeComponent("switch_res", {}, 10.0f, 10.0f);
    sw.properties["throws"] = "4";
    sw.properties["select"] = "3";

    CHECK(partPins(sw) == std::vector<std::string>{"in", "outA", "outB", "outC", "outD"});
    const auto pins = Schematic::pinOffsets(sw);
    REQUIRE(pins.size() == 5);
    CHECK(pins[0].x == 0);
    CHECK(pins[1].x == -3);
    CHECK(pins[2].x == -1);
    CHECK(pins[3].x == 1);
    CHECK(pins[4].x == 3);
    CHECK(wdf::switchResPosition(sw) == 3);
}

TEST_CASE("toCircuit connects exactly one of three throws", "[editor]") {
    auto doc = spdtDoc("2", "CHOICE");
    auto& sw = doc.components[1];
    sw.properties["throws"] = "3";
    sw.pins["outC"] = "nC";

    wdf::SchematicComponent rc;
    rc.id = "R_C"; rc.type = "res";
    rc.pins = {{"a", "nC"}, {"b", "out"}};
    rc.values = {{"value", 470000.0}};
    doc.components.push_back(rc);

    const auto circuit = doc.toCircuit();
    for (const char suffix : {'A', 'B', 'C'}) {
        const auto resistor = std::ranges::find(circuit.res, "S1_" + std::string(1, suffix),
                                               &wdf::Res::name);
        REQUIRE(resistor != circuit.res.end());
        CHECK(resistor->value == Approx(suffix == 'C' ? wdf::kSpdtClosedOhms
                                                       : wdf::kSpdtOpenOhms));
    }
}

TEST_CASE("A three-throw switch may leave its middle throw unconnected", "[editor]") {
    auto doc = spdtDoc("1", "CHOICE");
    auto& sw = doc.components[1];
    sw.properties["throws"] = "3";
    sw.pins["outC"] = "nC";
    doc.components.erase(doc.components.begin() + 3);

    const auto loose = doc.unconnectedPins();
    CHECK(std::ranges::none_of(loose, [](const auto& pin) { return pin.first == "S1"; }));
    CircuitSession session;
    REQUIRE(session.compile(doc, 48000.0));
}

TEST_CASE("Multi-throw selectors preserve throw-B gating", "[editor]") {
    auto doc = spdtDoc("1", "CHOICE");
    doc.components[1].properties["throws"] = "3";
    doc.components[1].pins["outC"] = "nC";
    doc.components[2].properties["enabledBy"] = "S1";
    CHECK(std::ranges::any_of(doc.toCircuit().res, [](const auto& r) { return r.name == "R_A"; }));

    doc.components[1].properties["select"] = "2";
    CHECK(std::ranges::none_of(doc.toCircuit().res, [](const auto& r) { return r.name == "R_A"; }));
}

TEST_CASE("Inspector sets a switch throw count and indexed position", "[editor]") {
    auto doc = spdtDoc("0", "CHOICE");
    doc.components[1].properties["throws"] = "3";
    InspectorPanel panel;
    panel.setTarget(&doc, "S1");

    auto* position = findComboWithItems(panel, 3);
    REQUIRE(position != nullptr);
    position->setSelectedId(3, juce::dontSendNotification);
    position->onChange();
    CHECK(doc.components[1].properties["select"] == "2");

    auto* throws = findComboWithItems(panel, wdf::kMaxSwitchThrows - wdf::kMinSwitchThrows + 1);
    REQUIRE(throws != nullptr);
    throws->setSelectedId(1, juce::dontSendNotification);
    throws->onChange();
    CHECK(doc.components[1].properties.count("throws") == 0);
    CHECK(wdf::switchResPosition(doc.components[1]) == 1);
}

TEST_CASE("toCircuit builds two resistors for switch_res", "[editor]") {
    auto doc = spdtDoc("0", "CHOICE");
    wdf::Circuit c = doc.toCircuit();

    // A switch is two ordinary resistors, like Celine's. A root device would
    // leave an unused throw -- or a switch feeding only an output -- on a node
    // with no tree path, which the engine cannot solve.
    REQUIRE(c.ctrlRes.empty());

    bool foundA = false, foundB = false;
    for (const auto& r : c.res) {
        if (r.name == "S1_A") {
            foundA = true;
            CHECK(r.value == Approx(0.01));   // closed
        }
        if (r.name == "S1_B") {
            foundB = true;
            CHECK(r.value == Approx(1.0e9)); // open
        }
    }
    REQUIRE(foundA);
    REQUIRE(foundB);
}

TEST_CASE("toCircuit flips throws when select is \"1\"", "[editor]") {
    auto doc = spdtDoc("1", "CHOICE");
    wdf::Circuit c = doc.toCircuit();

    for (const auto& r : c.res) {
        if (r.name == "S1_A")
            CHECK(r.value == Approx(1.0e9)); // open
        if (r.name == "S1_B")
            CHECK(r.value == Approx(0.01)); // closed
    }
}

TEST_CASE("CircuitSession compiles and solves a switch_res circuit", "[editor]") {
    livewdf::editor::CircuitSession session;
    REQUIRE(session.compile(spdtDoc("0", "CHOICE"), 48000.0));
    REQUIRE(session.valid());
    REQUIRE(session.dcConverged());

    // With select = 0, the path through R_A (1 MΩ) is active; R_B (680 kΩ) is
    // open. The output sits between 0 and 250 through the divider.
    const auto& v = session.dcVoltages();
    REQUIRE(v.count("out") == 1);
    REQUIRE(v.at("out") > 0.0);
    REQUIRE(v.at("out") < 1.0);   // small signal through 1 MΩ into 100 kΩ load
}

TEST_CASE("switch_res circuit flips output when select changes", "[editor]") {
    // Select 0: path through 1 MΩ
    livewdf::editor::CircuitSession s0;
    REQUIRE(s0.compile(spdtDoc("0", "CHOICE"), 48000.0));
    const double out0 = s0.dcVoltages().at("out");

    // Select 1: path through 680 kΩ
    livewdf::editor::CircuitSession s1;
    REQUIRE(s1.compile(spdtDoc("1", "CHOICE"), 48000.0));
    const double out1 = s1.dcVoltages().at("out");

    // Different resistors → different output voltage.
    REQUIRE(out0 != out1);
}

TEST_CASE("Inspector shows a switch button for switch_res select", "[editor]") {
    auto doc = spdtDoc("0", "CHOICE");
    InspectorPanel panel;
    panel.setTarget(&doc, doc.components[1].id); // the switch_res

    // Should find a TextButton labelled "Throw A" (select = "0"), and building
    // the panel must not index a missing caption (the old crash).
    juce::TextButton* btn = findButton(panel, "Throw A");
    REQUIRE(btn != nullptr);
    CHECK(findButton(panel, "Throw B") == nullptr);
}

TEST_CASE("Inspector shows throw A when the select property is missing", "[editor]") {
    auto doc = spdtDoc();
    doc.components[1].properties.erase("select");

    InspectorPanel panel;
    panel.setTarget(&doc, doc.components[1].id);

    juce::TextButton* btn = findButton(panel, "Throw A");
    REQUIRE(btn != nullptr);
    btn->onClick();
    CHECK(doc.components[1].properties["select"] == "1");
}

TEST_CASE("Clicking the switch button flips select, and a second click flips back", "[editor]") {
    auto doc = spdtDoc("0", "CHOICE");
    InspectorPanel panel;
    panel.setTarget(&doc, doc.components[1].id);

    juce::TextButton* btn = findButton(panel, "Throw A");
    REQUIRE(btn != nullptr);

    // Drive the callback directly: `triggerClick()` posts an async command and
    // the test runner has no message loop to deliver it.
    btn->onClick();
    CHECK(doc.components[1].properties["select"] == "1");
    CHECK(btn->getButtonText() == "Throw B");

    // The second click must read the *live* document, not the value captured
    // when the panel was built.
    btn->onClick();
    CHECK(doc.components[1].properties["select"] == "0");
    CHECK(btn->getButtonText() == "Throw A");
}

TEST_CASE("Ganging: two switch_res with the same label toggle together", "[editor]") {
    wdf::LiveWDFDocument doc;
    doc.name = "Ganged SPDT";
    doc.inputNode = "in";
    doc.outputNode = "out";

    wdf::SchematicComponent src;
    src.id = "Vin"; src.type = "src";
    src.pins = {{"pos", "in"}, {"neg", "0"}};
    src.values = {{"dc", 0.0}, {"ac", 1.0}};
    doc.components.push_back(src);

    // Two switches, same label "CHOICE".
    wdf::SchematicComponent sw1;
    sw1.id = "S1"; sw1.type = "switch_res";
    sw1.pins = {{"in", "in"}, {"outA", "n1a"}, {"outB", "n1b"}};
    sw1.properties = {{"select", "0"}, {"label", "CHOICE"}};
    doc.components.push_back(sw1);

    wdf::SchematicComponent sw2;
    sw2.id = "S2"; sw2.type = "switch_res";
    sw2.pins = {{"in", "in"}, {"outA", "n2a"}, {"outB", "n2b"}};
    sw2.properties = {{"select", "0"}, {"label", "CHOICE"}};
    doc.components.push_back(sw2);

    // The plugin builds one switch control from this same group list.
    const auto groups = wdf::switchControlGroups(doc);
    REQUIRE(groups.size() == 1);
    CHECK(groups.front().name == "CHOICE");
    REQUIRE(groups.front().members.size() == 2);
    CHECK(groups.front().members[0].id == "S1");
    CHECK(groups.front().members[1].id == "S2");

    InspectorPanel panel;
    panel.setTarget(&doc, "S1");

    juce::TextButton* btn = findButton(panel, "Throw A");
    REQUIRE(btn != nullptr);
    btn->onClick();

    // Both switches should now read "1".
    CHECK(doc.components[1].properties["select"] == "1"); // S1
    CHECK(doc.components[2].properties["select"] == "1"); // S2
}

TEST_CASE("Unlabelled switches remain separate controls", "[editor]") {
    wdf::LiveWDFDocument doc;
    for (const char* id : {"S1", "S2"}) {
        wdf::SchematicComponent sw;
        sw.id = id;
        sw.type = "switch_res";
        sw.pins = {{"in", "in"}, {"outA", "a"}, {"outB", "b"}};
        sw.properties = {{"select", "0"}};
        doc.components.push_back(sw);
    }

    const auto groups = wdf::switchControlGroups(doc);
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].name == "S1");
    CHECK(groups[1].name == "S2");
}

TEST_CASE("An SPDT with one unused throw still compiles", "[editor]") {
    // A changeover wired only through its A throw is the ordinary way to use
    // one: the B contact is left in the air. The open path must not count as a
    // dangling part, and it must not make the circuit unsolvable.
    auto doc = spdtDoc("0", "CHOICE");
    doc.components[1].pins["outB"] = "n_S1_outB";

    // Drop R_B: it was the load on the unused throw, and a floating load is a
    // second, separate drawing mistake.
    doc.components.erase(doc.components.begin() + 3);

    livewdf::editor::CircuitSession session;
    const bool compiled = session.compile(doc, 48000.0);
    INFO(session.lastError());
    REQUIRE(compiled);
    REQUIRE(session.dcConverged());
}

TEST_CASE("An unused SPDT throw is not reported as unconnected", "[editor]") {
    auto doc = spdtDoc("0", "CHOICE");
    doc.components[1].pins["outB"] = "n_S1_outB";
    doc.components.erase(doc.components.begin() + 3);   // no load on B

    const auto blamesSwitch = [](const std::vector<std::pair<std::string, std::string>>& loose) {
        return std::any_of(loose.begin(), loose.end(),
                           [](const auto& entry) { return entry.first == "S1"; });
    };

    CHECK_FALSE(blamesSwitch(doc.unconnectedPins()));

    // The exception is only for a switch that is otherwise in the circuit. A
    // switch joined to nothing at all is still an island and still named.
    doc.components[1].pins = {{"in", "n_s1_in"},
                              {"outA", "n_s1_a"},
                              {"outB", "n_s1_b"}};
    CHECK(blamesSwitch(doc.unconnectedPins()));
}

TEST_CASE("An SPDT wired through one throw in the editor compiles", "[editor]") {
    // The same case through the drawing model, so the pin geometry and the
    // document's net names are exercised too -- not just a hand-built document.
    Schematic s;
    const std::string srcId = s.addPart("src", 12, 20);
    const std::string swId = s.addPart("switch_res", 12, 10);
    const std::string resId = s.addPart("res", 4, 10);
    REQUIRE_FALSE(srcId.empty());
    REQUIRE_FALSE(swId.empty());
    REQUIRE_FALSE(resId.empty());

    s.findPart(resId)->pins["b"] = "0";

    auto wirePins = [&](const std::string& aId, int aPin,
                        const std::string& bId, int bPin) {
        const auto a = Schematic::pinPosition(*s.findPart(aId), aPin);
        const auto b = Schematic::pinPosition(*s.findPart(bId), bPin);
        s.addWire(a.x, a.y, b.x, b.y);
    };

    wirePins(srcId, 0, swId, 0);   // source to common
    wirePins(swId, 1, resId, 0);   // throw A to the load; throw B left open

    auto doc = s.toDocument();
    doc.inputNode = "in";
    doc.outputNode = "out";

    livewdf::editor::CircuitSession session;
    const bool compiled = session.compile(doc, 48000.0);
    INFO(session.lastError());
    REQUIRE(compiled);
    REQUIRE(session.dcConverged());
}

TEST_CASE("An SPDT feeding only the output terminal compiles", "[editor]") {
    // A series switch can be the last thing before the Output marker, leaving
    // that node with no load. An ordinary resistor branch still gives the
    // solver a tree path; a controlled-resistor root would not.
    wdf::LiveWDFDocument doc;
    doc.name = "SPDT output";
    doc.inputNode = "in";
    doc.outputNode = "out";

    wdf::SchematicComponent src;
    src.id = "Vin"; src.type = "src";
    src.pins = {{"pos", "in"}, {"neg", "0"}};
    src.values = {{"dc", 1.0}, {"ac", 1.0}};
    doc.components.push_back(src);

    wdf::SchematicComponent r;
    r.id = "R1"; r.type = "res";
    r.pins = {{"a", "in"}, {"b", "0"}};
    r.values = {{"value", 1000000.0}};
    doc.components.push_back(r);

    wdf::SchematicComponent sw;
    sw.id = "S1"; sw.type = "switch_res";
    sw.pins = {{"in", "in"}, {"outA", "out"}, {"outB", "n_S1_outB"}};
    sw.properties = {{"select", "0"}};
    doc.components.push_back(sw);

    wdf::SchematicComponent out;
    out.id = "OUT"; out.type = "out";
    out.pins = {{"sig", "out"}};
    doc.components.push_back(out);

    livewdf::editor::CircuitSession session;
    const bool compiled = session.compile(doc, 48000.0);
    INFO(session.lastError());
    CHECK(compiled);
}

TEST_CASE("Ganging does not touch switches with a different label", "[editor]") {
    wdf::LiveWDFDocument doc;
    doc.name = "Mixed SPDT";
    doc.inputNode = "in";
    doc.outputNode = "out";

    wdf::SchematicComponent src;
    src.id = "Vin"; src.type = "src";
    src.pins = {{"pos", "in"}, {"neg", "0"}};
    src.values = {{"dc", 0.0}, {"ac", 1.0}};
    doc.components.push_back(src);

    wdf::SchematicComponent sw1;
    sw1.id = "S1"; sw1.type = "switch_res";
    sw1.pins = {{"in", "in"}, {"outA", "n1a"}, {"outB", "n1b"}};
    sw1.properties = {{"select", "0"}, {"label", "CHOICE"}};
    doc.components.push_back(sw1);

    wdf::SchematicComponent sw2;
    sw2.id = "S2"; sw2.type = "switch_res";
    sw2.pins = {{"in", "in"}, {"outA", "n2a"}, {"outB", "n2b"}};
    sw2.properties = {{"select", "0"}, {"label", "OTHER"}};
    doc.components.push_back(sw2);

    InspectorPanel panel;
    panel.setTarget(&doc, "S1");

    juce::TextButton* btn = findButton(panel, "Throw A");
    REQUIRE(btn != nullptr);
    btn->onClick();

    // S1 flips, S2 does not.
    CHECK(doc.components[1].properties["select"] == "1"); // S1
    CHECK(doc.components[2].properties["select"] == "0"); // S2 unchanged
}
