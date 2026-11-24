#pragma once

// The searchable preset library shared by the editor and plugin.
//
// "Preset" here means a `.livewdf` circuit on disk. The panel is deliberately
// not a second document model: it lists files, and choosing one opens it in a
// tab through the same path as the Open button. That keeps tabs, undo, dirty
// marks and the engine in one place.
//
// The library is a dropdown rather than a permanent column because the editor
// already has a palette and an inspector; a third always-on panel would leave
// the sheet narrower than the circuit deserves. The list is re-read from disk
// every time it opens. A factory-only view leaves editor actions to the editor.

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <vector>

namespace livewdf::editor {

/// Previous and next controls for a factory circuit. A browser selection
/// supplies the visible factory rows so stepping follows that view.
class FactoryPresetStepper {
public:
    using Order = std::vector<juce::File>;

    FactoryPresetStepper(juce::Component& parent, juce::Label& name);

    void setFactoryFolder(const juce::File& folder, bool showPreviewPresets = false);
    void setCurrentFile(const juce::File& file);
    void setBrowseOrder(Order order);
    void setName(const juce::String& name);
    void setBounds(juce::Rectangle<int> bounds);

    std::function<void(const juce::File&)> onPresetChosen;

private:
    void updateButtons();
    juce::File adjacent(int offset) const;
    void step(int offset);

    juce::Label& name_;
    juce::TextButton previous_{juce::String::charToString(0x2039)};
    juce::TextButton next_{juce::String::charToString(0x203a)};
    juce::File factoryFolder_;
    bool showPreviewPresets_ = false;
    juce::File currentFile_;
    Order browseOrder_;
    Order defaultOrder_;
    bool defaultOrderLoaded_ = false;
    juce::Rectangle<int> bounds_;
};

class PresetLibraryPanel : public juce::Component,
                           private juce::ListBoxModel {
public:
    enum class Mode { Editor, Factory };
    enum class Scope { All, Featured, User, Pedals, Studio, Amps, Other };

    struct View {
        Scope scope = Scope::All;
        juce::String effect;
        juce::String search;
    };

    explicit PresetLibraryPanel(Mode mode = Mode::Editor);

    /// The folder to list. An empty or missing folder still shows the built-in
    /// rows, plus a way to choose a real one.
    void setFolder(const juce::File& folder);
    juce::File getFolder() const { return folder_; }
    void setFactoryFolder(const juce::File& folder, bool showPreviewPresets = false);

    /// Mark the file the current tab came from.
    void setCurrentFile(const juce::File& file);

    /// Factory files in the visible order, with user rows and actions omitted.
    FactoryPresetStepper::Order visibleFactoryOrder() const;
    View view() const;
    void setView(const View& view);

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
    static constexpr std::array kScopes{
        Scope::All, Scope::Amps, Scope::Pedals, Scope::Studio, Scope::Featured, Scope::User
    };

    enum class Kind {
        NewSheet,
        Starter,
        File,
        Section,
        Note,
        ChooseFolder,
        SaveCurrent,
        OpenFactoryFolder,
    };

    struct Entry {
        Kind kind = Kind::Note;
        juce::String label;
        juce::File file;
        juce::String detail;
        Scope scope = Scope::User;
        juce::String effect;
        bool featured = false;
    };

    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                          bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent& e) override;
    void returnKeyPressed(int lastRowSelected) override;

    void rebuild();
    void selectScope(Scope scope);
    void updateEffects();
    void applyFilter();
    bool matchesScope(const Entry& entry) const;
    static juce::String scopeName(Scope scope);
    juce::File factoryFolder() const;

    const Mode mode_;
    juce::File folder_;
    juce::File factoryFolder_;
    bool showPreviewPresets_ = false;
    juce::File currentFile_;
    juce::String searchText_;
    Scope scope_ = Scope::All;

    std::vector<Entry> files_;
    std::vector<Entry> shown_;

    juce::TextEditor search_;
    juce::ComboBox effectSelector_;
    std::array<juce::TextButton, kScopes.size()> scopeButtons_;
    juce::ListBox list_{"preset library", nullptr};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetLibraryPanel)
};

}  // namespace livewdf::editor
