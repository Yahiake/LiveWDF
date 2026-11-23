// Holds the palette's row to a layout that can actually be read.
//
// **The failure this exists for.** The palette row is a symbol, a name and a
// shortcut, in a panel 188 pixels wide. The three together do not fit unless
// the layout knows how much each one takes, and the symptom of getting it
// wrong is quiet and nasty: the name is ellipsized to "Potentiomet…" and the
// user reads it as the part being called something else.
//
// So the row's widths are *measured* -- the name in the name's font, the
// shortcut in the shortcut's font -- and checked against the width the panel
// actually has. A part added to the palette with a long name, or a shortcut
// that grows by a character, fails here rather than in front of a user.

#include "EditorLayout.h"
#include "format/PartLibrary.h"
#include "format/PartModels.h"
#include "PartPalettePanel.h"
#include "Shortcut.h"
#include "gui/Fonts.h"

#include <catch2.hpp>

#include <algorithm>
#include <functional>
#include <string>

using namespace livewdf::editor;

namespace {

/// The row's geometry, as the painter builds it.
///
/// Written out rather than asked of the panel, because the panel draws into
/// whatever size it is given and the *palette's* width is the number that
/// matters here -- it is Céline's, and the theme test holds it there.
struct RowGeometry {
    /// The row's width: the panel's width less the 4px inset on each side.
    int rowWidth;

    /// The symbol column: square on the row's height, which is the row height
    /// less the pill's 2px inset.
    int symbolColumn;

    /// The width left for the name and shortcut after the symbol column
    /// and the 6px inset on either side of the text.
    int textWidth;
};

RowGeometry rowGeometry() {
    constexpr int kPanelInset = 4;
    constexpr int kTextInset = 6;

    const int rowWidth = layout::kPaletteWidth - 2 * kPanelInset;
    const int symbolColumn = layout::kPaletteRowHeight - 4;

    return {rowWidth, symbolColumn, rowWidth - symbolColumn - 2 * kTextInset};
}

}  // namespace

TEST_CASE("The transistor palette entries name the part, not a model family",
          "[palette][transistors]") {
    // The MOSFET entry used to read "NMOS", as though the palette were offering
    // a device family rather than one generic N-channel part. (The engine has no
    // polarity property for these devices, so P-channel would be a promise it
    // cannot keep.) "BJT" is the counterpart: the part is a bipolar transistor,
    // and its model picker is where the NPN device name lives.
    const auto* mosfet = findPart("mosfet");
    REQUIRE(mosfet != nullptr);
    CHECK(mosfet->label == "MOSFET");

    const auto* bjt = findPart("bjt");
    REQUIRE(bjt != nullptr);
    CHECK(bjt->label == "BJT");
    CHECK(bjt->pins == std::vector<std::string>({"collector", "base", "emitter"}));
    CHECK(bjt->key == "Ctrl+B");

    // No row may still call the generic MOSFET an "NMOS"; that is the regression
    // this test exists for.
    for (const auto& spec : partPalette())
        CHECK(spec.label != "NMOS");
}

TEST_CASE("Every palette name fits beside its shortcut", "[palette][layout]") {
    const auto geometry = rowGeometry();
    const auto nameFont = Fonts::light(static_cast<float>(metrics::kNameHeight));
    const int shortcutColumn = shortcutColumnWidth();

    INFO("the row's text is " << geometry.textWidth << "px wide, the shortcut "
                              << "column " << shortcutColumn << "px");

    for (const auto& spec : partPalette()) {
        INFO("part: " << spec.paletteLabel());

        const auto nameWidth =
            juce::GlyphArrangement::getStringWidth(nameFont, spec.paletteLabel());
        const int reserved = displayShortcut(spec.key).isEmpty() ? 0 : shortcutColumn;
        CHECK(nameWidth + reserved <= static_cast<float>(geometry.textWidth));
    }
}

TEST_CASE("The shortcut column is as wide as the widest shortcut, and no wider",
           "[palette][layout]") {
    // The column is measured from the shortcuts that are actually in the
    // palette. A column wider than that steals the name's width; a column
    // narrower clips the text. Both are checked here, against the strings.
    const auto font = Fonts::light(static_cast<float>(metrics::kShortcutHeight));

    float widest = 0.0f;
    for (const auto& spec : partPalette())
        widest = std::max(widest,
                          juce::GlyphArrangement::getStringWidth(font, displayShortcut(spec.key)));

    const int column = shortcutColumnWidth();

    // Wide enough for the widest shortcut and its gap...
    CHECK(column >= static_cast<int>(widest) + metrics::kShortcutGap);

    // ...and no wider than it needs to be, plus a pixel of slack for the
    // rounding. A column that grows for another reason is the name losing
    // width for nothing.
    CHECK(column <= static_cast<int>(std::ceil(widest)) + metrics::kShortcutGap);
}

TEST_CASE("The Wire row has room for its name", "[palette][layout]") {
    // The Wire row has no shortcut, so it has the whole text width -- but its
    // name still has to fit in it, for the same reason.
    const auto geometry = rowGeometry();
    const auto nameFont = Fonts::light(static_cast<float>(metrics::kNameHeight));

    CHECK(juce::GlyphArrangement::getStringWidth(nameFont, "Wire")
          <= static_cast<float>(geometry.textWidth));
}

TEST_CASE("Every palette part is reachable in the category tree",
          "[palette][categories]") {
    // The panel walks `partCategories()`, not `partPalette()`; a part missing
    // from the tree is invisible in the editor no matter how correct the flat
    // table is.
    //
    // This is the regression for a tree builder that stored `CategorySpec*`
    // into `std::vector`s that later reallocated: the pointers went stale, most
    // categories silently lost their parts, and the visible palette shrank to
    // the handful whose pointers happened to survive.
    std::vector<const PartSpec*> found;

    std::function<void(const CategorySpec&)> walk = [&](const CategorySpec& cat) {
        CHECK_FALSE(cat.name.empty());
        CHECK_FALSE(cat.path.empty());
        found.insert(found.end(), cat.parts.begin(), cat.parts.end());

        for (const auto& child : cat.children) {
            INFO("child category: " << child.path << " under " << cat.path);
            CHECK(child.path.rfind(cat.path + "/", 0) == 0);
            walk(child);
        }
    };

    for (const auto& cat : partCategories()) {
        INFO("root category: " << cat.path);
        walk(cat);
    }

    for (const auto& spec : partPalette()) {
        INFO("part: " << spec.type << " / " << spec.paletteLabel());
        const auto count = std::count(found.begin(), found.end(), &spec);
        CHECK(count == 1);
    }
}

TEST_CASE("Model-bearing parts are listed once per registry model",
          "[palette][models]") {
    // The palette used to offer one anonymous "JFET" / "Triode" / "Diode" row
    // and hide the actual devices behind the inspector. The user asked for the
    // library to be the registry: every model is a row, and the row places that
    // model. This is the contract that makes that true.
    struct Family {
        const char* type;
        ChoiceKind kind;
        const char* property;
        const char* defaultModel;
    };

    const Family families[] = {
        {"diode", ChoiceKind::DiodeModel, "model", "1N4148"},
        {"triode", ChoiceKind::TriodeModel, "model", "12AX7"},
        {"pentode", ChoiceKind::PentodeModel, "model", "EL34"},
        {"jfet", ChoiceKind::JfetModel, "model", "2N5952"},
        {"mosfet", ChoiceKind::MosfetPreset, "preset", "2N7000"},
        {"bjt", ChoiceKind::BjtModel, "model", "2N3904"},
        {"opamp", ChoiceKind::OpAmpModel, "model", "TL072"},
    };

    for (const auto& family : families) {
        INFO("family: " << family.type);
        const auto models = modelChoices(family.kind);
        REQUIRE_FALSE(models.empty());

        int rows = 0;
        const PartSpec* first = nullptr;
        for (const auto& spec : partPalette()) {
            if (spec.type != family.type)
                continue;

            if (first == nullptr)
                first = &spec;
            ++rows;

            INFO("row: " << spec.model);
            CHECK(spec.modelProperty == family.property);
            CHECK_FALSE(spec.model.empty());
            CHECK(spec.paletteLabel() == spec.model);

            // Every row must name a model the engine can resolve.
            CHECK(std::find(models.begin(), models.end(), spec.model) != models.end());
        }

        CHECK(rows == static_cast<int>(models.size()));
        REQUIRE(first != nullptr);

        // The family's default leads, so its shortcut and `findPart(type)` name
        // the same device the palette used to place.
        CHECK(first->model == family.defaultModel);

        // Placing through a row applies that row's model, not the family
        // default. The last registry model is the least likely to be the
        // default, so it is the one that proves the wiring.
        const PartSpec* variant = nullptr;
        for (const auto& spec : partPalette())
            if (spec.type == family.type && spec.model == models.back())
                variant = &spec;
        REQUIRE(variant != nullptr);

        const auto part = makeComponent(family.type, {}, 0.0f, 0.0f,
                                        variant->modelProperty, variant->model);
        REQUIRE_FALSE(part.type.empty());
        const auto it = part.properties.find(family.property);
        REQUIRE(it != part.properties.end());
        CHECK(it->second == variant->model);
    }
}

TEST_CASE("The palette, rendered, for the eye", "[palette][render]") {
    // A PNG of the palette as it is drawn, written next to the test binary.
    //
    // **Why a test writes a file.** The layout is checked numerically above,
    // and the numbers cannot say whether the result is *readable* -- whether the
    // shortcut is dim enough to be secondary, whether the name and the symbol
    // are the right distance apart, whether the thing looks like a palette at
    // all. That judgement is made by looking, and this makes the looking
    // possible without starting the editor.
    //
    // Written into the build directory, which is where build output belongs,
    // and it is not an assertion: the file existing is the whole of the
    // result. If it cannot be written the test says so rather than failing
    // for a reason that has nothing to do with the palette.
    PartPalettePanel panel;

    const int rows = 1 + static_cast<int>(partPalette().size());
    panel.setBounds(0, 0, layout::kPaletteWidth, rows * layout::kPaletteRowHeight + 12);

    juce::Image image(juce::Image::ARGB, panel.getWidth(), panel.getHeight(), true);
    {
        juce::Graphics g(image);
        panel.paintEntireComponent(g, true);
    }

    // Written next to the test binary, which is where build output belongs.
    const juce::File out(
        juce::File::getSpecialLocation(juce::File::currentExecutableFile)
            .getParentDirectory()
            .getChildFile("livewdf_palette.png"));

    juce::MemoryOutputStream stream;
    juce::PNGImageFormat png;

    if (!png.writeImageToStream(image, stream)) {
        WARN("could not encode the palette as a PNG");
        return;
    }

    juce::FileOutputStream file(out);
    if (!file.openedOk()) {
        WARN("could not open " << out.getFullPathName());
        return;
    }

    file.write(stream.getData(), stream.getDataSize());
    file.flush();
}
