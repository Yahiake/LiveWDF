#pragma once

// The palette list: a thin view of `PartLibrary`'s table.
//
// Why this is separate from `PartLibrary`: the table is data -- pin names, id
// stems, default values -- and every non-GUI reader of it (the canvas, the model
// tests) should be able to include it without dragging in JUCE's widget layer.
// Keeping the panel here instead means `PartLibrary.cpp` compiles against nothing
// but the document format, and the *only* file that needs a `juce::Component` is
// this one.
//
// It stays a view of the same table, deliberately: offering a part the table
// does not describe is the failure mode the table exists to prevent, so the
// panel reads `partPalette()` rather than keeping a list of its own.
//
// The new design adds a search bar and a collapsible category tree. The flat
// `partPalette()` is still the source of truth; the tree is built from the
// `category` field on each `PartSpec`.

#include "format/PartLibrary.h"

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace livewdf::editor {

/// The palette row's text metrics, named so that the painter and any test that
/// measures the row agree on them.
namespace metrics {

/// The name's height, in pixels: the editor's everyday label size.
constexpr int kNameHeight = 17;

/// The shortcut's height, in pixels.
///
/// **Smaller than the name on purpose.** The shortcut is a fact *about* the
/// row, not the row's name, and a full-size "Ctrl+G" would take the width the
/// name needs -- in a 188px panel, that is the difference between
/// "Potentiometer" and "Potentiomet…".
constexpr int kShortcutHeight = 13;

/// The gap between the name and the shortcut, in pixels.
constexpr int kShortcutGap = 6;

/// Category header height.
constexpr int kCategoryHeight = 22;

/// Search bar height.
constexpr int kSearchHeight = 28;

/// Indentation per nesting level.
constexpr int kIndentPerLevel = 12;

}  // namespace metrics

/// The width the shortcut column takes: the widest shortcut in the palette,
/// measured in the shortcut's own font, plus the gap before it.
///
/// A function rather than a constant because the palette's shortcuts are
/// *data*: a new part with a longer shortcut widens the column without anyone
/// editing a number, and the row's layout cannot then be wrong by accident.
int shortcutColumnWidth();

/// How the shortcut is written for the user to read: "R", or "Ctrl+G" on
/// Windows and Linux and "Cmd+G" on a Mac.
juce::String displayShortcut(const std::string& key);

/// A node in the visible palette tree. Can be a category header or a part row.
struct PaletteNode {
    enum class Kind { Category, Part, Wire } kind;

    // For Category:
    const CategorySpec* category = nullptr;
    int level = 0;
    bool expanded = true;

    // For Part:
    const PartSpec* part = nullptr;

    // Common:
    int rowIndex = -1;          // Flat index in the visible list
    juce::Rectangle<float> bounds;
    bool hovered = false;
};

class PartPalettePanel : public juce::Component,
                         private juce::TextEditor::Listener {
public:
    PartPalettePanel();

    /// Called with the palette entry the user picked. A model-bearing part has
    /// one row per model, so the entry (not just its `type`) is what says which
    /// device to place.
    std::function<void(const PartSpec&)> onPartChosen;

    /// Called when the user picks the **Wire** row.
    std::function<void()> onWireChosen;

    /// Highlight the palette entry that is currently armed (nullptr for none).
    /// The editor owns arming; this is the view following it.
    void setArmed(const PartSpec* spec);

    /// Highlight the Wire row when the Wire *tool* is the active one.
    void setWireArmed(bool armed);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e,
                        const juce::MouseWheelDetails& wheel) override;

    // TextEditor::Listener
    void textEditorTextChanged(juce::TextEditor&) override;

private:
    // Rebuild the visible node list from the category tree + search filter.
    void rebuildVisibleNodes();

    // Initialize persisted expanded state from category defaults
    void initCategoryState(const CategorySpec& cat);

    // Recursively add category and its parts to the visible list.
    void addCategoryToVisible(const CategorySpec& cat, int level, bool parentMatches);

    // Part palette helpers (shared with the old implementation).
    static constexpr int kWireRow = 0;

    juce::Rectangle<float> rowBounds(int index) const;
    juce::Rectangle<float> categoryBounds(int index) const;
    int rowAt(juce::Point<float> p) const;
    const PaletteNode* nodeAt(juce::Point<float> p) const;

    /// Total height the visible list wants, including the search bar.
    int contentHeight() const;

    /// Keep the scroll offset inside the scrollable range.
    void clampScroll();

    // UI components
    juce::TextEditor searchBox_;  // real child so it can be focused and typed into
    juce::TextButton clearButton_;  // clears the query; hidden while it is empty

    // State
    std::string searchQuery_;
    std::vector<PaletteNode> visibleNodes_;
    /// Points into the static palette, so it identifies one model row rather
    /// than a whole family. Null when nothing is armed.
    const PartSpec* armedSpec_ = nullptr;
    bool wireArmed_ = false;
    int hoverIndex_ = -1;

    // User's manual expanded/collapsed state per category path.
    // Persists across rebuilds; separate from CategorySpec::defaultExpanded.
    std::unordered_map<std::string, bool> categoryExpandedState_;

    // Cached metrics
    int shortcutColWidth_ = 0;

    // Vertical scroll, in pixels, from the top of the list.
    float scrollOffset_ = 0.0f;
};

}  // namespace livewdf::editor