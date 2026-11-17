#include "PartPalettePanel.h"
#include "EditorLayout.h"
#include "EditorLookAndFeel.h"
#include "Shortcut.h"
#include "gui/Fonts.h"
#include "SymbolArtwork.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace livewdf::editor {

// The width the shortcut column takes: the widest shortcut in the palette,
// measured in the shortcut's own font, plus the gap before it.
//
// A function rather than a constant because the palette's shortcuts are
// *data*: a new part with a longer shortcut widens the column without anyone
// editing a number, and the row's layout cannot then be wrong by accident.
int shortcutColumnWidth() {
    // The widest shortcut in the palette, measured in the shortcut's own font.
    //
    // **Measured rather than assumed.** A fixed width has to be a guess about
    // the longest string that will ever appear in the column, and a guess that
    // is one character short clips the text; a guess that is generous steals
    // the width the name needs. Measuring the strings that are actually there
    // answers it exactly, and the palette's shortcuts are data -- so this
    // follows them.
    const auto font = Fonts::light(static_cast<float>(metrics::kShortcutHeight));

    float widest = 0.0f;
    for (const auto& spec : partPalette()) {
        const auto shortcut = displayShortcut(spec.key);
        if (shortcut.isNotEmpty())
            widest = std::max(widest, juce::GlyphArrangement::getStringWidth(font, shortcut));
    }

    return static_cast<int>(std::ceil(widest)) + metrics::kShortcutGap;
}

// Céline's palette metrics, via `EditorLayout.h` so the parity test reads the
// same number: a 35px row inside a viewport inset by 4. The pill is the row less
// a 2px inset, drawn by `rowBounds` below.
constexpr int kRowHeight = layout::kPaletteRowHeight;
constexpr int kRowPad = 6;   // where the list starts, below the search bar

namespace {

// Case-insensitive substring search.
bool matchesQuery(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    std::string h = haystack;
    std::string n = needle;
    std::transform(h.begin(), h.end(), h.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return h.find(n) != std::string::npos;
}

// Does this palette row match what the user typed?
//
// Both names are searched: the model (`12AX7`) is what the row says, and the
// family (`Triode`) is what the user is likely to type when they do not yet
// know the model they want.
bool partMatchesQuery(const PartSpec& spec, const std::string& query) {
    return matchesQuery(spec.paletteLabel(), query) ||
           matchesQuery(spec.label, query) ||
           matchesQuery(spec.type, query) ||
           matchesQuery(spec.idPrefix, query) ||
           (!spec.key.empty() && matchesQuery(spec.key, query));
}

// Check if a category or any of its descendants contain a part matching the query.
bool categoryOrDescendantMatches(const CategorySpec& cat, const std::string& query) {
    if (query.empty()) return true;
    for (const PartSpec* spec : cat.parts) {
        if (spec && partMatchesQuery(*spec, query))
            return true;
    }
    for (const auto& child : cat.children) {
        if (categoryOrDescendantMatches(child, query))
            return true;
    }
    return false;
}

}  // namespace

PartPalettePanel::PartPalettePanel() {
    // Allow child components to receive mouse/keyboard input (none currently)
    setInterceptsMouseClicks(true, true);

    // The search field is a real child component: it has to be focusable and
    // take keystrokes, which a hand-painted box cannot do.
    searchBox_.setFont(Fonts::medium(13.5f));
    searchBox_.setJustification(juce::Justification::centredLeft);
    searchBox_.setTextToShowWhenEmpty("Search components...",
                                      EditorLookAndFeel::textOnPanel().withAlpha(0.4f));
    searchBox_.setColour(juce::TextEditor::backgroundColourId, EditorLookAndFeel::panel().brighter(0.08f));
    searchBox_.setColour(juce::TextEditor::textColourId, EditorLookAndFeel::textOnPanel());
    searchBox_.setColour(juce::TextEditor::highlightColourId, EditorLookAndFeel::accent().withAlpha(0.3f));
    searchBox_.setColour(juce::TextEditor::outlineColourId, EditorLookAndFeel::textOnPanel().withAlpha(0.2f));
    searchBox_.setColour(juce::TextEditor::focusedOutlineColourId, EditorLookAndFeel::accent());
    searchBox_.setColour(juce::CaretComponent::caretColourId, EditorLookAndFeel::accent());
    searchBox_.setBorder(juce::BorderSize<int>(0));
    searchBox_.setInputRestrictions(0, "");
    searchBox_.addListener(this);
    addAndMakeVisible(searchBox_);

    clearButton_.setButtonText(juce::String::fromUTF8("\xc3\x97"));  // ×
    clearButton_.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    clearButton_.setColour(juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
    clearButton_.setColour(juce::TextButton::textColourOffId,
                           EditorLookAndFeel::textOnPanel().withAlpha(0.5f));
    clearButton_.setColour(juce::TextButton::textColourOnId,
                           EditorLookAndFeel::textOnPanel());
    clearButton_.setTooltip("Clear search");
    clearButton_.onClick = [this] {
        searchBox_.clear();
        searchBox_.grabKeyboardFocus();
    };
    // Starts hidden (addChildComponent, not addAndMakeVisible) and is shown
    // only while there is a query to clear.
    addChildComponent(clearButton_);

    // Initialize expanded state from defaults
    for (const auto& cat : partCategories()) {
        initCategoryState(cat);
    }

    rebuildVisibleNodes();
    shortcutColWidth_ = shortcutColumnWidth();
}

void PartPalettePanel::initCategoryState(const CategorySpec& cat) {
    if (categoryExpandedState_.find(cat.path) == categoryExpandedState_.end()) {
        categoryExpandedState_[cat.path] = cat.defaultExpanded;
    }
    for (const auto& child : cat.children) {
        initCategoryState(child);
    }
}

void PartPalettePanel::setArmed(const PartSpec* spec) {
    if (armedSpec_ == spec)
        return;
    armedSpec_ = spec;
    repaint();
}

void PartPalettePanel::setWireArmed(bool armed) {
    wireArmed_ = armed;
    if (armed)
        armedSpec_ = nullptr;
    repaint();
}

void PartPalettePanel::rebuildVisibleNodes() {
    visibleNodes_.clear();
    const std::string query = searchQuery_;
    hoverIndex_ = -1;

    // Wire tool entry (special, not a part) - respects search filtering
    const bool wireMatches = query.empty() || matchesQuery("Wire", query) || matchesQuery("wire", query);
    if (wireMatches) {
        PaletteNode wireNode;
        wireNode.kind = PaletteNode::Kind::Wire;
        wireNode.rowIndex = static_cast<int>(visibleNodes_.size());
        visibleNodes_.push_back(wireNode);
    }

    // Build from category tree
    const auto& categories = partCategories();
    for (const auto& cat : categories) {
        addCategoryToVisible(cat, 0, false);
    }

    clampScroll();
}

void PartPalettePanel::addCategoryToVisible(const CategorySpec& cat, int level, bool parentMatches) {
    const bool queryEmpty = searchQuery_.empty();

    // A query that names a category shows everything under it. Otherwise a
    // category is visible when it or any descendant holds a matching part.
    const bool categoryNameMatches = !queryEmpty && matchesQuery(cat.name, searchQuery_);
    const bool showAllUnder = parentMatches || categoryNameMatches;
    const bool categoryMatches = queryEmpty || showAllUnder ||
                                 categoryOrDescendantMatches(cat, searchQuery_);

    // In search mode: hide categories with no matches
    // In normal mode: show all categories
    if (!queryEmpty && !categoryMatches)
        return;

    // Category header node
    PaletteNode catNode;
    catNode.kind = PaletteNode::Kind::Category;
    catNode.category = &cat;
    catNode.level = level;

    // Use persisted expanded state, falling back to default. During a search
    // every matching category is shown open, so its chevron must say so.
    auto it = categoryExpandedState_.find(cat.path);
    catNode.expanded = (it != categoryExpandedState_.end()) ? it->second : cat.defaultExpanded;
    if (!queryEmpty)
        catNode.expanded = true;

    catNode.rowIndex = static_cast<int>(visibleNodes_.size());
    visibleNodes_.push_back(catNode);

    // During search: always expand categories that have matches so results are visible
    // During normal browsing: respect user's collapsed/expanded state
    const bool shouldShowChildren = queryEmpty ? catNode.expanded : true;

    if (shouldShowChildren) {
        // Parts directly in this category
        for (const PartSpec* spec : cat.parts) {
            if (!spec) continue;

            const bool partMatches = queryEmpty || showAllUnder ||
                                     partMatchesQuery(*spec, searchQuery_);

            if (!partMatches) continue;

            PaletteNode partNode;
            partNode.kind = PaletteNode::Kind::Part;
            partNode.part = spec;
            partNode.level = level + 1;
            partNode.rowIndex = static_cast<int>(visibleNodes_.size());
            visibleNodes_.push_back(partNode);
        }

        // Recurse into children
        for (const auto& child : cat.children) {
            addCategoryToVisible(child, level + 1, showAllUnder);
        }
    }
}

juce::Rectangle<float> PartPalettePanel::rowBounds(int index) const {
    // The list starts under the search bar and slides with the scroll offset.
    const float top = static_cast<float>(metrics::kSearchHeight + kRowPad + 2)
                    + static_cast<float>(index * kRowHeight)
                    - std::round(scrollOffset_);
    return {4.0f, top, static_cast<float>(getWidth() - 8), static_cast<float>(kRowHeight - 4)};
}

juce::Rectangle<float> PartPalettePanel::categoryBounds(int index) const {
    // A category header occupies the same slot as a part row, so the whole row
    // is a click target rather than just the 22px the text happens to fill.
    return rowBounds(index);
}

int PartPalettePanel::rowAt(juce::Point<float> p) const {
    // The search bar is not part of the scrolling list.
    if (p.y < static_cast<float>(metrics::kSearchHeight))
        return -1;

    const int rows = static_cast<int>(visibleNodes_.size());
    for (int i = 0; i < rows; ++i) {
        if (visibleNodes_[static_cast<size_t>(i)].kind == PaletteNode::Kind::Category) {
            if (categoryBounds(i).contains(p))
                return i;
        } else {
            if (rowBounds(i).contains(p))
                return i;
        }
    }
    return -1;
}

int PartPalettePanel::contentHeight() const {
    const int listTop = metrics::kSearchHeight + kRowPad;
    return listTop + static_cast<int>(visibleNodes_.size()) * kRowHeight + kRowPad;
}

void PartPalettePanel::clampScroll() {
    const int maxScroll = std::max(0, contentHeight() - getHeight());
    scrollOffset_ = juce::jlimit(0.0f, static_cast<float>(maxScroll), scrollOffset_);
}

const PaletteNode* PartPalettePanel::nodeAt(juce::Point<float> p) const {
    const int idx = rowAt(p);
    if (idx < 0 || idx >= static_cast<int>(visibleNodes_.size()))
        return nullptr;
    return &visibleNodes_[static_cast<size_t>(idx)];
}

void PartPalettePanel::paint(juce::Graphics& g) {
    g.fillAll(EditorLookAndFeel::panel());

    // Search bar ground. The field itself is a child component painted on top.
    juce::Rectangle<float> searchBarArea(0, 0, static_cast<float>(getWidth()),
                                        static_cast<float>(metrics::kSearchHeight));
    g.setColour(EditorLookAndFeel::panel().darker(0.04f));
    g.fillRect(searchBarArea);

    g.setColour(EditorLookAndFeel::textOnPanel().withAlpha(0.18f));
    g.drawLine(0.0f, static_cast<float>(metrics::kSearchHeight),
               static_cast<float>(getWidth()), static_cast<float>(metrics::kSearchHeight), 1.0f);

    // The list scrolls beneath the search bar, so clip it there.
    juce::Graphics::ScopedSaveState saved(g);
    g.reduceClipRegion(juce::Rectangle<int>(0, metrics::kSearchHeight, getWidth(),
                                            std::max(0, getHeight() - metrics::kSearchHeight)));

    // Draw rows
    const auto inkDefault = EditorLookAndFeel::textOnPanel();
    const auto inkArmed = EditorLookAndFeel::panelText();
    const auto pillDefault = EditorLookAndFeel::pill();
    const auto pillHover = pillDefault.darker(0.05f);
    const auto pillArmed = EditorLookAndFeel::accent();

    for (size_t i = 0; i < visibleNodes_.size(); ++i) {
        const auto& node = visibleNodes_[i];

        if (node.kind == PaletteNode::Kind::Category) {
            // Category header
            const auto r = categoryBounds(static_cast<int>(i));
            if (r.getBottom() < static_cast<float>(metrics::kSearchHeight) ||
                r.getY() > static_cast<float>(getHeight()))
                continue;

            const bool hovered = (static_cast<int>(i) == hoverIndex_);

            // Category background (subtle)
            if (hovered) {
                g.setColour(EditorLookAndFeel::pill().darker(0.03f));
                g.fillRoundedRectangle(r.reduced(2.0f, 1.0f), EditorLookAndFeel::cornerRadius);
            }

            // Chevron — compact, using the secondary text colour so it
            // reads as furniture rather than content.
            const float chevronSize = 6.0f;
            const float indent = static_cast<float>(node.level * metrics::kIndentPerLevel);
            const float cx = r.getX() + 6.0f + indent + chevronSize / 2.0f;
            const float cy = r.getCentreY();
            juce::Path chevron;
            if (node.expanded) {
                chevron.startNewSubPath(cx - chevronSize/2, cy - chevronSize/3);
                chevron.lineTo(cx + chevronSize/2, cy - chevronSize/3);
                chevron.lineTo(cx, cy + chevronSize/2);
                chevron.closeSubPath();
            } else {
                chevron.startNewSubPath(cx - chevronSize/3, cy - chevronSize/2);
                chevron.lineTo(cx + chevronSize/2, cy);
                chevron.lineTo(cx - chevronSize/3, cy + chevronSize/2);
                chevron.closeSubPath();
            }
            g.setColour(EditorLookAndFeel::textSecondary().withAlpha(hovered ? 0.9f : 0.5f));
            g.fillPath(chevron);

            // Category name — small-caps feel: uppercase, semibold, secondary
            // colour. Reads as a section divider, not as a clickable row.
            g.setColour(EditorLookAndFeel::textSecondary().withAlpha(hovered ? 1.0f : 0.75f));
            g.setFont(Fonts::semiBold(10.5f));
            auto textArea = r.withTrimmedLeft(18.0f + indent)
                                 .withTrimmedRight(6.0f)
                                 .toNearestInt();
            g.drawText(juce::String(node.category->name).toUpperCase(), textArea, juce::Justification::centredLeft, true);

            // Hairline under the category header — quiet separation.
            g.setColour(EditorLookAndFeel::panelBorder().withAlpha(0.25f));
            g.drawHorizontalLine(juce::roundToInt(r.getBottom()),
                                 r.getX() + 4.0f, r.getRight() - 4.0f);
        }
        else if (node.kind == PaletteNode::Kind::Part) {
            // Part row
            const auto r = rowBounds(static_cast<int>(i));
            if (r.getBottom() < static_cast<float>(metrics::kSearchHeight) ||
                r.getY() > static_cast<float>(getHeight()))
                continue;

            const bool armed = (node.part == armedSpec_);
            const bool hovered = (static_cast<int>(i) == hoverIndex_);

            const auto fill = armed   ? pillArmed
                            : hovered ? pillHover
                                      : pillDefault;
            g.setColour(fill);
            g.fillRoundedRectangle(r, EditorLookAndFeel::cornerRadius);

            const auto ink = armed ? inkArmed : inkDefault;

            // Symbol on the left, in a column square on the row height
            const auto symbolArea = r.withWidth(r.getHeight());
            const bool isWire = (node.part->type == "wire");
            if (isWire) {
                // Wire tool: draw elbow symbol manually (no artwork file)
                const auto area = symbolArea.reduced(7.0f);
                g.setColour(ink);
                g.drawLine(area.getX(), area.getBottom(), area.getCentreX(), area.getBottom(), 1.5f);
                g.drawLine(area.getCentreX(), area.getBottom(), area.getCentreX(), area.getY(), 1.5f);
                g.drawLine(area.getCentreX(), area.getY(), area.getRight(), area.getY(), 1.5f);
            } else {
                drawPreviewSymbol(g, node.part->type, symbolArea, ink);
            }

            // Name on the left, shortcut on the right
            const auto& spec = *node.part;
            const auto shortcut = displayShortcut(spec.key);

            // No depth indent on part rows: the category header above already
            // says where the part lives, and indenting here clips long names in
            // the 188px panel ("Resist…", "Capac…").
            auto textArea = r.withTrimmedLeft(r.getHeight()).reduced(6.0f, 0.0f).toNearestInt();
            auto shortcutArea = juce::Rectangle<int>();
            if (shortcut.isNotEmpty())
                shortcutArea = textArea.removeFromRight(shortcutColWidth_);

            g.setColour(ink);
            g.setFont(Fonts::light(static_cast<float>(metrics::kNameHeight)));
            g.drawText(spec.paletteLabel(), textArea, juce::Justification::centredLeft, true);

            if (shortcut.isNotEmpty()) {
                g.setColour(ink.withAlpha(0.5f));
                g.setFont(Fonts::light(static_cast<float>(metrics::kShortcutHeight)));
                g.drawText(shortcut, shortcutArea, juce::Justification::centredRight, true);
            }
        }
        else if (node.kind == PaletteNode::Kind::Wire) {
            // Wire tool row (special, not a part)
            const auto r = rowBounds(static_cast<int>(i));
            if (r.getBottom() < static_cast<float>(metrics::kSearchHeight) ||
                r.getY() > static_cast<float>(getHeight()))
                continue;

            const bool armed = wireArmed_;
            const bool hovered = (static_cast<int>(i) == hoverIndex_);

            const auto fill = armed   ? pillArmed
                            : hovered ? pillHover
                                      : pillDefault;
            g.setColour(fill);
            g.fillRoundedRectangle(r, EditorLookAndFeel::cornerRadius);

            const auto ink = armed ? inkArmed : inkDefault;

            // Wire symbol (elbow)
            const auto symbolArea = r.withWidth(r.getHeight());
            const auto area = symbolArea.reduced(7.0f);
            g.setColour(ink);
            g.drawLine(area.getX(), area.getBottom(), area.getCentreX(), area.getBottom(), 1.5f);
            g.drawLine(area.getCentreX(), area.getBottom(), area.getCentreX(), area.getY(), 1.5f);
            g.drawLine(area.getCentreX(), area.getY(), area.getRight(), area.getY(), 1.5f);

            g.setColour(ink);
            g.setFont(Fonts::light(static_cast<float>(metrics::kNameHeight)));
            g.drawText("Wire", r.withTrimmedLeft(r.getHeight()).reduced(6.0f, 0.0f).toNearestInt(),
                       juce::Justification::centredLeft, true);
        }
    }

    // A slim scrollbar, only when the list is taller than the panel.
    const int maxScroll = std::max(0, contentHeight() - getHeight());
    if (maxScroll > 0) {
        const float trackTop = static_cast<float>(metrics::kSearchHeight + 4);
        const float trackBottom = static_cast<float>(getHeight() - 4);
        const float trackHeight = std::max(0.0f, trackBottom - trackTop);
        const float viewport = static_cast<float>(getHeight());
        const float content = static_cast<float>(contentHeight());
        const float thumbHeight = std::min(trackHeight,
                                           std::max(24.0f, trackHeight * viewport / content));
        const float travel = trackHeight - thumbHeight;
        const float thumbTop = trackTop +
            travel * (scrollOffset_ / static_cast<float>(maxScroll));

        juce::Rectangle<float> thumb(static_cast<float>(getWidth() - 5),
                                     thumbTop, 3.0f, thumbHeight);
        g.setColour(EditorLookAndFeel::textOnPanel().withAlpha(0.25f));
        g.fillRoundedRectangle(thumb, 1.5f);
    }

    // The hairline down the panel's right edge, on the light ground.
    g.setColour(EditorLookAndFeel::textOnPanel().withAlpha(0.18f));
    g.drawLine(static_cast<float>(getWidth()) - 0.5f,
               static_cast<float>(metrics::kSearchHeight),
               static_cast<float>(getWidth()) - 0.5f,
               static_cast<float>(getHeight()), 1.0f);

    // Empty state: only when the query leaves no usable entry (Wire counts).
    bool hasEntry = false;
    for (const auto& node : visibleNodes_) {
        if (node.kind != PaletteNode::Kind::Category) {
            hasEntry = true;
            break;
        }
    }
    if (!hasEntry && !searchQuery_.empty()) {
        g.setColour(inkDefault.withAlpha(0.4f));
        g.setFont(Fonts::medium(13.5f));
        juce::Rectangle<int> msgArea(8, metrics::kSearchHeight + kRowPad,
                                     getWidth() - 16, kRowHeight * 2);
        g.drawFittedText("No components match \"" + searchQuery_ + "\"",
                         msgArea, juce::Justification::centredTop, 2);
    }
}

void PartPalettePanel::resized() {
    shortcutColWidth_ = shortcutColumnWidth();

    // The button's slot is always reserved, so the field does not resize as
    // the query appears and disappears.
    constexpr int kButtonWidth = 18;
    const int fieldWidth = std::max(0, getWidth() - 16 - kButtonWidth - 4);
    searchBox_.setBounds(8, 4, fieldWidth, metrics::kSearchHeight - 8);
    clearButton_.setBounds(8 + fieldWidth + 4, 4, kButtonWidth,
                           metrics::kSearchHeight - 8);

    clampScroll();
}

void PartPalettePanel::mouseDown(const juce::MouseEvent& e) {
    const auto* node = nodeAt(e.position.toFloat());
    if (!node) return;

    if (node->kind == PaletteNode::Kind::Category) {
        // Toggle only this category, not its descendants. The user opens
        // exactly what they click; children stay collapsed until they click them.
        const CategorySpec& cat = *node->category;
        categoryExpandedState_[cat.path] = !node->expanded;
        rebuildVisibleNodes();
        repaint();
        return;
    }

    if (node->kind == PaletteNode::Kind::Part) {
        wireArmed_ = false;
        armedSpec_ = node->part;
        if (onPartChosen)
            onPartChosen(*node->part);
        repaint();
        return;
    }

    if (node->kind == PaletteNode::Kind::Wire) {
        // Wire tool row
        armedSpec_ = nullptr;
        wireArmed_ = true;
        if (onWireChosen)
            onWireChosen();
        repaint();
        return;
    }
}

void PartPalettePanel::mouseMove(const juce::MouseEvent& e) {
    const int idx = rowAt(e.position.toFloat());
    if (idx != hoverIndex_) {
        hoverIndex_ = idx;
        repaint();
    }
}

void PartPalettePanel::mouseExit(const juce::MouseEvent&) {
    if (hoverIndex_ != -1) {
        hoverIndex_ = -1;
        repaint();
    }
}

void PartPalettePanel::mouseWheelMove(const juce::MouseEvent&,
                                      const juce::MouseWheelDetails& wheel) {
    const int maxScroll = std::max(0, contentHeight() - getHeight());
    if (maxScroll <= 0)
        return;

    // Three rows per notch. Deltas are fractional on trackpads, so keep the
    // offset itself fractional and round only when painting.
    scrollOffset_ = juce::jlimit(0.0f, static_cast<float>(maxScroll),
                                 scrollOffset_ - wheel.deltaY * kRowHeight * 3.0f);
    repaint();
}

void PartPalettePanel::textEditorTextChanged(juce::TextEditor&) {
    const std::string newQuery = searchBox_.getText().toStdString();
    if (newQuery != searchQuery_) {
        searchQuery_ = newQuery;
        clearButton_.setVisible(!searchQuery_.empty());
        scrollOffset_ = 0.0f;  // new results start at the top
        rebuildVisibleNodes();
        repaint();
    }
}

}  // namespace livewdf::editor