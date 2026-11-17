#include "PresetLibraryPanel.h"

#include "gui/Fonts.h"
#include "gui/glow/Glow.h"

#include <algorithm>
#include <memory>

namespace livewdf::editor {

namespace {
constexpr int kRowH = 24;
constexpr int kSearchH = 26;
constexpr int kPad = 9;
constexpr int kMaxFiles = 2000;

/** Natural order: "Bass 9" before "Bass 10". */
void sortFiles(juce::Array<juce::File>& files, const juce::File& root) {
    std::sort(files.begin(), files.end(), [&root](const juce::File& a, const juce::File& b) {
        return a.getRelativePathFrom(root).compareNatural(b.getRelativePathFrom(root)) < 0;
    });
}
}  // namespace

PresetLibraryPanel::PresetLibraryPanel() {
    setOpaque(false);
    setInterceptsMouseClicks(true, true);

    search_.setFont(Fonts::mono(12.0f));
    search_.setJustification(juce::Justification::centredLeft);
    search_.setBorder(juce::BorderSize<int>(0));
    search_.setIndents(9, 0);
    search_.setTextToShowWhenEmpty("Search presets...", glow::getColourTextSecondary());
    search_.setColour(juce::TextEditor::backgroundColourId, glow::getColourBackground());
    search_.setColour(juce::TextEditor::textColourId, glow::getColourTextPrimary());
    search_.setColour(juce::TextEditor::outlineColourId, glow::getColourBorder());
    search_.setColour(juce::TextEditor::focusedOutlineColourId, glow::getColourAccent());
    search_.setColour(juce::TextEditor::highlightColourId,
                      glow::getColourAccent().withAlpha(0.35f));
    search_.onTextChange = [this] {
        searchText_ = search_.getText().trim();
        applyFilter();
    };
    search_.onEscapeKey = [this] {
        if (onDismiss)
            onDismiss();
    };
    search_.onReturnKey = [this] { chooseFirstMatch(); };
    addAndMakeVisible(search_);

    list_.setModel(this);
    list_.setRowHeight(kRowH);
    list_.setMultipleSelectionEnabled(false);
    list_.setClickingTogglesRowSelection(false);
    list_.setOutlineThickness(0);
    list_.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list_.setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(list_);

    rebuild();
}

void PresetLibraryPanel::setFolder(const juce::File& folder) {
    folder_ = folder;
    rescan();
}

void PresetLibraryPanel::setCurrentFile(const juce::File& file) {
    currentFile_ = file;
    list_.repaint();
}

void PresetLibraryPanel::rescan() {
    rebuild();
}

void PresetLibraryPanel::setSearchText(const juce::String& text) {
    search_.setText(text, juce::dontSendNotification);
    searchText_ = search_.getText().trim();
    applyFilter();
}

void PresetLibraryPanel::focusSearch() {
    search_.grabKeyboardFocus();
    search_.selectAll();
}

void PresetLibraryPanel::chooseFirstMatch() {
    // **Return with nothing typed must not load the first row.** The first row
    // is "New empty sheet", and a stray Return would throw away the sheet the
    // user opened the library to browse away from.
    if (searchText_.isEmpty())
        return;

    for (int i = 0; i < static_cast<int>(shown_.size()); ++i)
        if (shown_[static_cast<size_t>(i)].kind != Kind::Section &&
            shown_[static_cast<size_t>(i)].kind != Kind::Note) {
            chooseRow(i);
            return;
        }
}

int PresetLibraryPanel::getNumRows() {
    return static_cast<int>(shown_.size());
}

int PresetLibraryPanel::getRowCount() const {
    return static_cast<int>(shown_.size());
}

juce::String PresetLibraryPanel::getRowText(int row) const {
    if (row < 0 || row >= static_cast<int>(shown_.size()))
        return {};
    return shown_[static_cast<size_t>(row)].label;
}

bool PresetLibraryPanel::isRowFile(int row) const {
    if (row < 0 || row >= static_cast<int>(shown_.size()))
        return false;
    return shown_[static_cast<size_t>(row)].kind == Kind::File;
}

juce::File PresetLibraryPanel::getRowFile(int row) const {
    if (row < 0 || row >= static_cast<int>(shown_.size()))
        return {};
    return shown_[static_cast<size_t>(row)].file;
}

bool PresetLibraryPanel::isRowCurrent(int row) const {
    if (row < 0 || row >= static_cast<int>(shown_.size()))
        return false;
    const auto& entry = shown_[static_cast<size_t>(row)];
    return entry.kind == Kind::File && currentFile_ != juce::File() &&
           entry.file == currentFile_;
}

void PresetLibraryPanel::chooseRow(int row) {
    if (row < 0 || row >= static_cast<int>(shown_.size()))
        return;

    const auto entry = shown_[static_cast<size_t>(row)];   // copy: callbacks may rebuild

    switch (entry.kind) {
    case Kind::NewSheet:
        if (onNewSheet) onNewSheet();
        break;
    case Kind::Starter:
        if (onStarterChosen) onStarterChosen();
        break;
    case Kind::File:
        if (onPresetChosen) onPresetChosen(entry.file);
        break;
    case Kind::ChooseFolder:
        if (onChooseFolder) onChooseFolder();
        break;
    case Kind::SaveCurrent:
        if (onSaveCurrent) onSaveCurrent();
        break;
    case Kind::Section:
    case Kind::Note:
        break;
    }

    list_.deselectAllRows();
}

void PresetLibraryPanel::rebuild() {
    entries_.clear();

    entries_.push_back({Kind::NewSheet, "New empty sheet", {}, {}});
    entries_.push_back({Kind::Starter, "Starter: RC lowpass", {}, {}});
    entries_.push_back({Kind::Section, "PRESETS", {}, {}});

    if (folder_.isDirectory()) {
        juce::Array<juce::File> files;
        int scanned = 0;

        for (const auto& entry : juce::RangedDirectoryIterator(
                 folder_, true, "*.livewdf", juce::File::findFiles)) {
            const auto file = entry.getFile();

            if (file.getFileName().startsWith("."))
                continue;

            files.add(file);
            if (++scanned >= kMaxFiles)
                break;
        }

        sortFiles(files, folder_);

        for (const auto& file : files) {
            Entry entry;
            entry.kind = Kind::File;
            entry.file = file;
            entry.label = file.getFileNameWithoutExtension();
            const auto parent = file.getParentDirectory();
            entry.detail = parent == folder_
                               ? juce::String()
                               : parent.getRelativePathFrom(folder_);
            entries_.push_back(std::move(entry));
        }

        if (files.isEmpty())
            entries_.push_back({Kind::Note,
                                "No .livewdf files in " + folder_.getFileName() + " yet",
                                {}, {}});
    } else {
        entries_.push_back({Kind::Note, "No preset folder chosen.", {}, {}});
    }

    entries_.push_back({Kind::Section, "LIBRARY", {}, {}});

    if (!folder_.isDirectory())
        entries_.push_back({Kind::ChooseFolder, "Choose preset folder...", {}, {}});

    entries_.push_back({Kind::SaveCurrent, "Save current as preset...", {}, {}});

    applyFilter();
}

void PresetLibraryPanel::applyFilter() {
    shown_.clear();

    if (searchText_.isEmpty()) {
        shown_ = entries_;
    } else {
        for (const auto& entry : entries_)
            if (entry.kind == Kind::File &&
                (entry.label.containsIgnoreCase(searchText_) ||
                 entry.detail.containsIgnoreCase(searchText_)))
                shown_.push_back(entry);

        if (shown_.empty())
            shown_.push_back({Kind::Note, "No presets match \"" + searchText_ + "\"", {}, {}});
    }

    list_.updateContent();
    list_.repaint();
    repaint();
}

void PresetLibraryPanel::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat().reduced(0.5f);

    g.setColour(glow::getColourPanel());
    g.fillRoundedRectangle(bounds, 7.0f);

    g.setColour(glow::getColourBorder());
    g.drawRoundedRectangle(bounds, 7.0f, 1.0f);

    // A tiny pointer notch toward the toolbar button would be nice, but the
    // panel is anchored under it and a notch that misses by two pixels is worse
    // than none. The border is enough.
}

void PresetLibraryPanel::resized() {
    const int w = std::max(80, getWidth() - 2 * kPad);
    search_.setBounds(kPad, kPad, w, kSearchH);
    list_.setBounds(kPad, kPad + kSearchH + 4, w,
                    std::max(20, getHeight() - (kPad + kSearchH + 4) - kPad));
}

bool PresetLibraryPanel::keyPressed(const juce::KeyPress& key) {
    if (key == juce::KeyPress::escapeKey) {
        if (onDismiss)
            onDismiss();
        return true;
    }

    if (key == juce::KeyPress::returnKey) {
        chooseFirstMatch();
        return true;
    }

    return false;
}

void PresetLibraryPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                                          bool selected) {
    if (row < 0 || row >= static_cast<int>(shown_.size()))
        return;

    const auto& entry = shown_[static_cast<size_t>(row)];
    const juce::Rectangle<int> area(0, 0, width, height);

    if (selected) {
        g.setColour(glow::getColourElevated());
        g.fillRect(area);
    }

    switch (entry.kind) {
    case Kind::Section: {
        g.setColour(glow::getColourTextSecondary().withAlpha(0.65f));
        g.setFont(Fonts::semiBold(9.0f));
        g.drawText(entry.label, area.withTrimmedLeft(2), juce::Justification::centredLeft);

        const int textW = static_cast<int>(
            juce::GlyphArrangement::getStringWidth(Fonts::semiBold(9.0f), entry.label));
        g.setColour(glow::getColourBorder());
        g.drawHorizontalLine(height / 2, static_cast<float>(textW + 8),
                             static_cast<float>(width - 4));
        break;
    }

    case Kind::Note:
        g.setColour(glow::getColourTextSecondary().withAlpha(0.7f));
        g.setFont(Fonts::mono(11.0f));
        g.drawText(entry.label, area.withTrimmedLeft(2).withTrimmedRight(4),
                   juce::Justification::centredLeft, true);
        break;

    case Kind::File: {
        const bool current = isRowCurrent(row);
        const int left = current ? 16 : 4;

        if (current) {
            g.setColour(glow::getColourAccent());
            g.fillEllipse(4.0f, static_cast<float>(height) * 0.5f - 2.0f, 4.0f, 4.0f);
        }

        g.setColour(current ? glow::getColourAccent() : glow::getColourTextPrimary());
        g.setFont(Fonts::mono(12.0f));

        const int detailW =
            entry.detail.isEmpty()
                ? 0
                : static_cast<int>(juce::jmin(
                      140.0f, juce::GlyphArrangement::getStringWidth(Fonts::mono(10.0f),
                                                                     entry.detail) +
                                  8.0f));
        g.drawText(entry.label, area.withTrimmedLeft(left).withTrimmedRight(detailW + 8),
                   juce::Justification::centredLeft, true);

        if (!entry.detail.isEmpty()) {
            g.setColour(glow::getColourTextSecondary().withAlpha(0.7f));
            g.setFont(Fonts::mono(10.0f));
            g.drawText(entry.detail, area.withTrimmedRight(6),
                       juce::Justification::centredRight, true);
        }
        break;
    }

    case Kind::NewSheet:
    case Kind::Starter:
    case Kind::ChooseFolder:
    case Kind::SaveCurrent:
        g.setColour(entry.kind == Kind::NewSheet     ? glow::getColourAccent()
                    : entry.kind == Kind::Starter    ? glow::getColourSignal()
                                                     : glow::getColourTextSecondary());
        g.setFont(Fonts::mono(12.0f));
        g.drawText(entry.label, area.withTrimmedLeft(4).withTrimmedRight(6),
                   juce::Justification::centredLeft, true);
        break;
    }
}

void PresetLibraryPanel::listBoxItemClicked(int row, const juce::MouseEvent&) {
    chooseRow(row);
}

void PresetLibraryPanel::returnKeyPressed(int lastRowSelected) {
    if (lastRowSelected >= 0)
        chooseRow(lastRowSelected);
    else
        chooseFirstMatch();
}

void PresetLibraryPanel::clearRows() {
    entries_.clear();
    shown_.clear();
    list_.updateContent();
}

}  // namespace livewdf::editor
