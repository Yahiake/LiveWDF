#pragma once

// A small, searchable preset library for the editor.
//
// "Preset" here means a `.livewdf` circuit on disk. The panel is deliberately
// not a second document model: it lists files, and choosing one opens it in a
// tab through the same path as the Open button. That keeps tabs, undo, dirty
// marks and the engine in one place.
//
// The library is a dropdown rather than a permanent column because the editor
// already has a palette and an inspector; a third always-on panel would leave
// the sheet narrower than the circuit deserves. The folder is the user's
// choice, and the list is re-read from disk every time it opens.

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace livewdf::editor {

class PresetLibraryPanel : public juce::Component,
                           private juce::ListBoxModel {
public:
    PresetLibraryPanel();

    /// The folder to list. An empty or missing folder still shows the built-in
    /// rows, plus a way to choose a real one.
    void setFolder(const juce::File& folder);
    juce::File getFolder() const { return folder_; }

    /// Mark the file the current tab came from.
    void setCurrentFile(const juce::File& file);

    /// Re-read the folder from disk.
    void rescan();

    /// The search field's text. Public for tests and for the host.
    void setSearchText(const juce::String& text);

    /// Focus the search box, so opening the panel means typing immediately.
    void focusSearch();

    /// Choose the first row the current search shows.
    void chooseFirstMatch();

    // Read-only row access, for tests and for tooltips.
    int getRowCount() const;
    juce::String getRowText(int row) const;
    bool isRowFile(int row) const;
    juce::File getRowFile(int row) const;
    bool isRowCurrent(int row) const;

    /// What clicking a row does.
    void chooseRow(int row);

    std::function<void()> onNewSheet;
    std::function<void()> onStarterChosen;
    std::function<void(const juce::File&)> onPresetChosen;
    std::function<void()> onChooseFolder;
    std::function<void()> onSaveCurrent;
    std::function<void()> onDismiss;

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    enum class Kind {
        NewSheet,
        Starter,
        File,
        Section,
        Note,
        ChooseFolder,
        SaveCurrent,
    };

    struct Entry {
        Kind kind = Kind::Note;
        juce::String label;
        juce::File file;
        juce::String detail;
    };

    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                          bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent& e) override;
    void returnKeyPressed(int lastRowSelected) override;

    void rebuild();
    void applyFilter();
    void clearRows();

    juce::File folder_;
    juce::File currentFile_;
    juce::String searchText_;

    std::vector<Entry> entries_;
    std::vector<Entry> shown_;

    juce::TextEditor search_;
    juce::ListBox list_{"preset library", nullptr};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetLibraryPanel)
};

}  // namespace livewdf::editor
