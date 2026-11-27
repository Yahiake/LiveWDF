// Mixed part-and-wire selection.
//
// The complaint: Shift/Ctrl-click built a selection only while it stayed on one
// kind of thing. Picking two parts and then a wire dropped the parts (and the
// other way round), so the additive gesture was not actually additive across
// the two lists the model keeps.
//
// These tests drive the same public selection entry points the gestures use, so
// a future "cleanup" that makes either setter clear the other list has to fail
// here rather than in the editor.

#include "SchematicEditor.h"
#include "SchematicModel.h"

#include <catch2.hpp>

using namespace livewdf::editor;

namespace {

struct SelectionFixture {
    Schematic sheet;
    std::string first;
    std::string second;
    int wire = -1;

    SelectionFixture() {
        first = sheet.addPart("res", 10, 10);
        second = sheet.addPart("res", 20, 10);
        wire = sheet.addWire(30, 0, 40, 0).front();
    }
};

}  // namespace

TEST_CASE("Additive part and wire selection keeps both kinds") {
    SelectionFixture fixture;
    SchematicEditor editor;
    editor.setDocument(fixture.sheet.toDocument());

    // Start with one part and one wire together, as a Ctrl-click gesture can.
    editor.setSelection({fixture.first}, {fixture.wire});
    REQUIRE(editor.selectedPartIds() == std::vector<std::string>{fixture.first});
    REQUIRE(editor.selectedWireIds() == std::vector<int>{fixture.wire});

    const std::vector<std::string> bothParts{fixture.first, fixture.second};

    // Adding a part must not drop the wire.
    editor.togglePartInSelection(fixture.second);
    CHECK(editor.selectedPartIds() == bothParts);
    CHECK(editor.selectedWireIds() == std::vector<int>{fixture.wire});

    // Adding a wire must not drop the parts. Toggle the one already selected
    // off, then add it back: both directions preserve the other list.
    editor.toggleWireInSelection(fixture.wire);
    CHECK(editor.selectedPartIds() == bothParts);
    CHECK(editor.selectedWireIds().empty());

    editor.toggleWireInSelection(fixture.wire);
    CHECK(editor.selectedPartIds() == bothParts);
    CHECK(editor.selectedWireIds() == std::vector<int>{fixture.wire});

    // Removing a part leaves the wire and the other part alone too.
    editor.togglePartInSelection(fixture.first);
    CHECK(editor.selectedPartIds() == std::vector<std::string>{fixture.second});
    CHECK(editor.selectedWireIds() == std::vector<int>{fixture.wire});

    editor.togglePartInSelection(fixture.second);
    CHECK(editor.selectedPartIds().empty());
    CHECK(editor.selectedWireIds() == std::vector<int>{fixture.wire});
}

TEST_CASE("A replacement selection still means one kind of thing") {
    SelectionFixture fixture;
    SchematicEditor editor;
    editor.setDocument(fixture.sheet.toDocument());

    editor.setSelection({fixture.first}, {fixture.wire});
    REQUIRE(editor.selectedPartIds().size() == 1);
    REQUIRE(editor.selectedWireIds().size() == 1);

    // Plain clicks use these setters; each must clear the other half.
    editor.setSelection(fixture.first);
    CHECK(editor.selectedPartIds() == std::vector<std::string>{fixture.first});
    CHECK(editor.selectedWireIds().empty());

    editor.setSelection({fixture.first}, {fixture.wire});
    editor.setSelectedWires({fixture.wire});
    CHECK(editor.selectedPartIds().empty());
    CHECK(editor.selectedWireIds() == std::vector<int>{fixture.wire});
}

TEST_CASE("Escape disarms placement and clears the pending palette row",
          "[tools][escape]") {
    // What the user sees as "the row is still highlighted after Escape": the
    // canvas dropped to Select but the pending part stayed armed. Escape is the
    // one gesture that means "I am done with that part", so it has to clear
    // both the tool and the palette's idea of it.
    SchematicEditor editor;

    const PartSpec* triode = nullptr;
    for (const auto& spec : partPalette())
        if (spec.type == "triode" && spec.model == "12AT7")
            triode = &spec;
    REQUIRE(triode != nullptr);

    editor.armPlacement(*triode);
    REQUIRE(editor.getTool() == SchematicEditor::Tool::Place);
    CHECK(editor.pendingPartSpec() == triode);
    CHECK(editor.pendingPlacement() == "triode");

    CHECK(editor.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
    CHECK(editor.getTool() == SchematicEditor::Tool::Select);
    CHECK(editor.pendingPartSpec() == nullptr);
    CHECK(editor.pendingPlacement().empty());
}

TEST_CASE("A model row places that model, not the family default",
          "[palette][models]") {
    SchematicEditor editor;

    const PartSpec* jfet = nullptr;
    for (const auto& spec : partPalette())
        if (spec.type == "jfet" && spec.model == "J201")
            jfet = &spec;
    REQUIRE(jfet != nullptr);

    editor.armPlacement(*jfet);
    const size_t before = editor.schematic().getParts().size();
    REQUIRE(editor.placePartAt(10, 10));

    REQUIRE(editor.schematic().getParts().size() == before + 1);
    const auto& placed = editor.schematic().getParts().back();
    CHECK(placed.type == "jfet");
    CHECK(placed.properties.at("model") == "J201");

    // The type-only entry point (keyboard shortcut, tests, callers that only
    // know a family) still arms the family's default model.
    editor.setTool(SchematicEditor::Tool::Select);
    editor.armPlacement("jfet");
    REQUIRE(editor.placePartAt(20, 10));

    REQUIRE(editor.schematic().getParts().size() == before + 2);
    CHECK(editor.schematic().getParts().back().properties.at("model") == "2N5952");
}

TEST_CASE("A pot placed from the palette starts with its high terminal at the top") {
    // The palette row carries the pot's default orientation (`H` above `L`),
    // and both paths that build a new pot -- `makeComponent` and the pending
    // state seeded by arming -- have to seed that same row, or the placed
    // part lands in an orientation the swatch never promised.
    SchematicEditor editor;
    editor.armPlacement("pot");
    REQUIRE(editor.placePartAt(10, 10));

    const wdf::SchematicComponent* pot = nullptr;
    for (const auto& part : editor.schematic().getParts())
        if (part.type == "pot")
            pot = &part;
    REQUIRE(pot != nullptr);

    CHECK(pot->visual.rotation == 0);
    CHECK_FALSE(pot->visual.flipped);

    const auto high = Schematic::pinPosition(*pot, 0);
    const auto low = Schematic::pinPosition(*pot, 2);
    CHECK(high.y < low.y);
    CHECK(high.y < static_cast<int>(pot->visual.y));
    CHECK(low.y > static_cast<int>(pot->visual.y));
}

TEST_CASE("Ctrl+F flips every selected component") {
    // One key press must mirror the whole selection, whatever the part type.
    // The flag is `visual.flipped`, which every symbol path reads -- the
    // artwork, the pins, the pot's H/L letters and its wiper all follow it.
    SelectionFixture fixture;
    SchematicEditor editor;
    editor.setDocument(fixture.sheet.toDocument());

    const auto pot = editor.schematic().addPart("pot", 30, 10);
    const auto triode = editor.schematic().addPart("triode", 40, 10);

    editor.setSelection({fixture.first, pot, triode});
    const bool potStartedFlipped = editor.schematic().findPart(pot)->visual.flipped;
    CHECK_FALSE(potStartedFlipped); // the pot's high-up default sits upright, no mirror
    const auto ctrlF = juce::KeyPress('f', juce::ModifierKeys::ctrlModifier, 'f');
    CHECK(editor.keyPressed(ctrlF));
    CHECK(editor.schematic().findPart(fixture.first)->visual.flipped);
    CHECK(editor.schematic().findPart(pot)->visual.flipped != potStartedFlipped);
    CHECK(editor.schematic().findPart(triode)->visual.flipped);
    CHECK(editor.isDirty());

    // A second press puts them back; the shortcut is a toggle, not one-way.
    CHECK(editor.keyPressed(ctrlF));
    CHECK_FALSE(editor.schematic().findPart(fixture.first)->visual.flipped);
    CHECK(editor.schematic().findPart(pot)->visual.flipped == potStartedFlipped);
    CHECK_FALSE(editor.schematic().findPart(triode)->visual.flipped);
}

TEST_CASE("Space rotates selected and pending components") {
    SelectionFixture fixture;
    SchematicEditor editor;
    editor.setDocument(fixture.sheet.toDocument());
    editor.setSelection(fixture.first);

    CHECK(editor.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));
    CHECK(editor.schematic().findPart(fixture.first)->visual.rotation == 90);

    editor.armPlacement("res");
    CHECK(editor.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));
    REQUIRE(editor.placePartAt(40, 10));
    CHECK(editor.schematic().getParts().back().visual.rotation == 90);
}

TEST_CASE("A mixed selection deletes both parts and wires") {
    SelectionFixture fixture;
    SchematicEditor editor;
    editor.setDocument(fixture.sheet.toDocument());

    // Put the wire on the sheet, then select both parts and the wire together.
    editor.setSelection({fixture.first, fixture.second}, {fixture.wire});
    REQUIRE(editor.hasSelection());

    CHECK(editor.deleteSelection());
    CHECK_FALSE(editor.hasSelection());
    CHECK(editor.schematic().getParts().empty());
    CHECK(editor.schematic().getWires().empty());
}
