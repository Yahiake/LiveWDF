#include "EditorWindow.h"
#include "EditorWindowInternal.h"
#include "DiagLog.h"
#include "EditorLayout.h"
#include "EditorLookAndFeel.h"
#include "FactorySchematics.h"
#include "FastLiveCache.h"
#include "format/ControlGroups.h"
#include "gui/Fonts.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace livewdf::editor {
namespace {

bool applySavedPlaybackControls(const wdf::LiveWDFDocument& doc, wdf::Engine& engine) {
    const auto component = [&](const std::string& id) -> const wdf::SchematicComponent& {
        return *std::find_if(doc.components.begin(), doc.components.end(),
                            [&](const auto& part) { return part.id == id; });
    };
    std::vector<std::pair<int, double>> pots, rheostats, switches;
    const auto potGroups = wdf::potControlGroups(doc);
    const juce::NormalisableRange<float> knobRange{0.0f, 1.0f, wdf::kLivePotStep};
    const size_t groupCount = std::min(potGroups.size(), static_cast<size_t>(wdf::kLivePotSlots));
    for (size_t i = 0; i < groupCount; ++i) {
        const auto& group = potGroups[i];
        float position = 0.5f;
        for (const auto& member : group.members) {
            const auto& values = component(member.id).values;
            if (const auto found = values.find("pos"); found != values.end()) {
                position = static_cast<float>(std::clamp(found->second, 0.0, 1.0));
                break;
            }
        }
        // Match the host parameter's normalized round trip on load.
        position = knobRange.snapToLegalValue(knobRange.convertFrom0to1(
            knobRange.convertTo0to1(knobRange.snapToLegalValue(position))));
        for (const auto& member : group.members) {
            const bool rheostat = member.type == "varres";
            const int index = rheostat ? engine.varResIndexByName(member.id)
                                       : engine.potIndexByName(member.id);
            if (index >= 0)
                (rheostat ? rheostats : pots).emplace_back(index, position);
        }
    }
    for (const auto& group : wdf::switchControlGroups(doc)) {
        const bool throwB = wdf::switchResSelectsB(component(group.members.front().id));
        for (const auto& member : group.members) {
            const int a = engine.resIndexByName(member.id + "_A");
            const int b = engine.resIndexByName(member.id + "_B");
            if (a >= 0 && b >= 0) {
                switches.emplace_back(a, throwB ? wdf::kSpdtOpenOhms : wdf::kSpdtClosedOhms);
                switches.emplace_back(b, throwB ? wdf::kSpdtClosedOhms : wdf::kSpdtOpenOhms);
            }
        }
    }
    return engine.setParametersByIndex(pots, rheostats, switches);
}

void prepareSavedCircuit(const juce::File& file) {
    wdf::LiveWDFDocument doc;
    std::string error;
    if (!doc.loadFromFile(file.getFullPathName().toStdString(), error))
        return;

    const auto inputId = doc.inputId();
    if (inputId.empty() || !doc.terminal("out") || !doc.unconnectedPins().empty())
        return;

    const auto circuit = doc.toCircuit();
    const auto input = std::find_if(circuit.src.begin(), circuit.src.end(),
                                    [&](const auto& source) { return source.name == inputId; });
    if (input == circuit.src.end())
        return;
    wdf::Engine engine(circuit, 48000.0);
    if (engine.nodeIndex(doc.outputNet()) < 0 || !applySavedPlaybackControls(doc, engine))
        return;
    if (auto setup = engine.captureMixedAffineSetup(static_cast<size_t>(input - circuit.src.begin()))) {
        const auto cacheFile = file.isAChildOf(FactorySchematics::folder()) ? juce::File{} : file;
        buildCachedAffineSetup(*setup, nullptr, doc.toJson(), cacheFile, true);
    }
}

} // namespace

void EditorWindow::Content::startSavedCircuitPreparation() {
    if (pendingPreparations_.empty() ||
        (savedCircuitPreparation_.valid() &&
         savedCircuitPreparation_.wait_for(std::chrono::seconds(0)) != std::future_status::ready))
        return;

    auto file = std::move(pendingPreparations_.front());
    pendingPreparations_.pop_front();
    try {
        savedCircuitPreparation_ = std::async(std::launch::async, [file = std::move(file)] {
            try {
                prepareSavedCircuit(file);
            } catch (const std::exception&) {
                // The saved schematic is still usable. Playback can prepare it on load.
            }
        });
    } catch (const std::system_error&) {
        // A background worker is optional to a completed save.
    }
}

void EditorWindow::Content::rebuildNow() {
    rebuildEngine();
    pendingRebuild_ = false;
    updateRebuildButton();
}

void EditorWindow::Content::markContentChanged() {
    // Both flags, and the split matters: `pendingRebuild_` is what you can hear
    // falling behind (cleared only by a compile), the canvas's dirty flag is what
    // differs from the file on disk (cleared only by a save). An edit sets both;
    // each is cleared by a different action, so one flag could not do both jobs.
    pendingRebuild_ = true;
    editor_.markDirty();

    updateRebuildButton();
    refreshTitle();
}

void EditorWindow::Content::updateRebuildButton() {
    // The button is the design's one *stateful colour*: pending amber against an
    // ordinary fill, so "what you can hear is behind" is visible without a
    // separate indicator. Gathered here so a theme change agrees with every other
    // reader rather than three setters drifting apart.
    const auto colour = pendingRebuild_ ? glow::getColourAmber() : glow::getColourNormal();

    rebuildBtn_.setColour(juce::TextButton::textColourOffId, colour);
    rebuildBtn_.setColour(juce::TextButton::textColourOnId, colour);
}

void EditorWindow::Content::refreshTitle() {
    // **The current tab's name**, and its modified mark. The title is the
    // window's answer to "which circuit am I in", and the answer is the *tab's*
    // -- so it changes when the tab does.
    titleLabel_.setText(currentTabName(), juce::dontSendNotification);
}

bool EditorWindow::Content::openFile(const juce::File& file) {
    wdf::LiveWDFDocument doc;
    std::string err;
    if (!doc.loadFromFile(file.getFullPathName().toStdString(), err)) {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "Could not open circuit",
            juce::String("Failed to read ") + file.getFileName() + ":\n" + juce::String(err));
        return false;
    }

    diagLog("open: " + file.getFullPathName().toStdString());

    // **The loader's own words belong in the log too.** The status bar says them
    // while the window is open; the log says them after the window has gone,
    // which is what makes "open this file and tell me why it will not build"
    // answerable without a screenshot.
    for (const auto& warning : doc.loadWarnings)
        diagLog("load warning: " + warning);

    openDocumentTab(std::move(doc), file);
    return true;
}

void EditorWindow::Content::openDocumentTab(wdf::LiveWDFDocument document,
                                            const juce::File& file) {
    // **A document opens in a tab of its own**, so the circuit being edited
    // stays where it was -- the way a browser opens a link in a new tab rather
    // than throwing away what you had. New, Open, the starter sheet and the
    // preset library all arrive here, so a tab cannot be assembled one way by
    // one entry point and another way by the next.
    Tab tab;
    tab.document = std::move(document);
    tab.file = file;
    tab.view = editor_.viewState();

    saveCurrentTab();

    tabs_.push_back(std::move(tab));
    currentTab_ = static_cast<int>(tabs_.size()) - 1;

    editor_.setDocument(tabs_.back().document);
    history_.reset(editor_.schematic());   // a different document: no undo across it

    session_.clear();
    nodeTable_.clear();
    updateUndoButtons();
    pendingRebuild_ = true;   // opened means heard: compile the new sheet now
    rebuildNow();
    clearScopeHistory();

    refreshTitle();
    tabBar_.setTabs(tabNames(), currentTab_);
}

void EditorWindow::Content::openStarterTab() {
    openDocumentTab(starterDocument_, juce::File());
}

bool EditorWindow::Content::saveFile(const juce::File& file) {
    // The document the inspector edited is the canvas's, so it is already in the
    // engine's form. `saveToFile` reads it straight out.
    if (!editor_.getDocument().saveToFile(file.getFullPathName().toStdString())) {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "Could not save circuit",
            juce::String("Failed to write ") + file.getFileName() + ".");
        return false;
    }
    // **The tab remembers which file it is**, so switching away and back does
    // not ask where to save it all over again -- and so that a second tab's
    // Save never writes over this one's file.
    if (currentTab_ >= 0 && currentTab_ < static_cast<int>(tabs_.size())) {
        tabs_[static_cast<size_t>(currentTab_)].file = file;
        tabs_[static_cast<size_t>(currentTab_)].dirty = false;
    }

    editor_.markClean();
    refreshTitle();
    tabBar_.setTabs(tabNames(), currentTab_);
    if (std::find(pendingPreparations_.begin(), pendingPreparations_.end(), file) == pendingPreparations_.end())
        pendingPreparations_.push_back(file);
    startSavedCircuitPreparation();
    return true;
}

// ----------------------------------------------------------------------------
// Engine hookup
// ----------------------------------------------------------------------------

void EditorWindow::Content::saveCurrentTab() {
    if (currentTab_ < 0 || currentTab_ >= static_cast<int>(tabs_.size()))
        return;

    auto& tab = tabs_[static_cast<size_t>(currentTab_)];

    // The canvas owns the document while the tab is showing, so the tab's copy
    // is refreshed on the way out.
    tab.document = editor_.getDocument();
    tab.history = history_;
    tab.selectedParts = editor_.selectedPartIds();
    tab.selectedWires = editor_.selectedWireIds();
    tab.view = editor_.viewState();
    tab.dirty = editor_.isDirty();
}

void EditorWindow::Content::loadTab(int index) {
    if (index < 0 || index >= static_cast<int>(tabs_.size()))
        return;

    const auto& tab = tabs_[static_cast<size_t>(index)];

    // **The document goes on first**, because everything else reads it.
    editor_.setDocument(tab.document);
    history_ = tab.history;

    // The selection and the view: a tab that comes back as it was left. One
    // combined call, because a selection may contain parts and wires together
    // and the two single-kind setters each clear the other half.
    editor_.setSelection(tab.selectedParts, tab.selectedWires);
    editor_.setViewState(tab.view);

    // **And its modified mark**, which is the tab's own: one tab's save must not
    // clear another's, and a tab that comes back must say whether it is
    // unsaved.
    if (tab.dirty)
        editor_.markDirty();
    else
        editor_.markClean();

    // **And the engine is recompiled**, because the scope and the node table
    // show the *current* tab's circuit, and showing the previous tab's numbers
    // under the new tab's drawing is worse than a moment's wait.
    session_.clear();
    nodeTable_.clear();
    pendingRebuild_ = true;
    rebuildNow();
    clearScopeHistory();

    updateUndoButtons();
    refreshTitle();
    tabBar_.setTabs(tabNames(), currentTab_);
}

void EditorWindow::Content::selectTab(int index) {
    if (index == currentTab_)
        return;

    if (index < 0 || index >= static_cast<int>(tabs_.size()))
        return;

    saveCurrentTab();
    currentTab_ = index;
    loadTab(index);
}

std::vector<std::string> EditorWindow::Content::tabNames() const {
    std::vector<std::string> names;
    names.reserve(tabs_.size());

    for (size_t i = 0; i < tabs_.size(); ++i) {
        const auto& tab = tabs_[i];

        std::string name = tab.file == juce::File()
                               ? std::string("Untitled")
                               : tab.file.getFileName().toStdString();

        // **The modified mark**, so a tab says it has unsaved changes -- the
        // same mark the title bar carries.
        if (tab.dirty)
            name += " *";

        names.push_back(std::move(name));
    }

    return names;
}

juce::String EditorWindow::Content::currentTabName() const {
    if (currentTab_ < 0 || currentTab_ >= static_cast<int>(tabs_.size()))
        return "LiveWDF";

    const auto& tab = tabs_[static_cast<size_t>(currentTab_)];

    // **The same words the tab strip uses**, so the title bar and the strip
    // never disagree about what a tab is called.
    juce::String name = tab.file == juce::File() ? juce::String("Untitled")
                                                  : tab.file.getFileName();
    if (tab.dirty)
        name += " *";
    return name;
}

std::vector<juce::String> EditorWindow::Content::tabTitles() const {
    std::vector<juce::String> titles;
    for (const auto& name : tabNames())
        titles.push_back(juce::String(name));
    return titles;
}

juce::File EditorWindow::Content::currentFile() const {    if (currentTab_ < 0 || currentTab_ >= static_cast<int>(tabs_.size()))
        return juce::File();
    return tabs_[static_cast<size_t>(currentTab_)].file;
}

void EditorWindow::Content::saveCurrentTabOrAsk() {
    // **A tab that has never been saved asks where to put it.** The file
    // belongs to the tab, so "Save" on a new tab cannot write over some *other*
    // tab's file -- which is what it used to do, because the file was a
    // property of the window rather than of the tab.
    const auto file = currentFile();
    if (file != juce::File()) {
        saveFile(file);
        return;
    }

    saveAs();
}

void EditorWindow::Content::saveAs() {
    saveAsToFolder(juce::File::getSpecialLocation(juce::File::userDocumentsDirectory));
}

void EditorWindow::Content::saveAsToFolder(const juce::File& folder) {
    // **Save under a name the user chooses**, whatever the tab was called.
    // The suggestion is the tab's own name where it has one, so "Save As" on
    // an opened file starts from that file rather than from a default.
    const juce::String suggestion = currentFile() != juce::File()
                                       ? currentFile().getFileName()
                                       : juce::String("circuit.livewdf");

    chooser_ = std::make_unique<juce::FileChooser>(
        "Save .livewdf circuit", folder.getChildFile(suggestion), kFileWildcard);

    chooser_->launchAsync(juce::FileBrowserComponent::saveMode |
                              juce::FileBrowserComponent::warnAboutOverwriting,
                          [this](const juce::FileChooser& fc) {
                              const auto f = fc.getResult();
                              if (f == juce::File())
                                  return;

                              saveFile(f);
                              if (f.getParentDirectory() == presetFolder_) {
                                  presetLibrary_.setCurrentFile(f);
                                  presetLibrary_.rescan();
                              }
                          });
}

void EditorWindow::Content::newTab() {
    wdf::LiveWDFDocument blank;
    blank.name = "Untitled Circuit";
    blank.inputNode = "in";
    blank.outputNode = "out";
    blank.groundNode = "0";

    // A blank document is genuinely empty, not the canvas's demo. It still goes
    // through the same tab path as an opened file, so its history and engine
    // are reset exactly the same way.
    openDocumentTab(std::move(blank), juce::File());
    editor_.schematic().clear();
    history_.reset(editor_.schematic());
    rebuildNow();
}

void EditorWindow::Content::closeTab(int index) {
    // **The last tab cannot be closed.** An editor with no circuit open has
    // nothing to edit, and a window that vanishes is a window the user did not
    // ask to vanish.
    if (tabs_.size() <= 1)
        return;

    if (index < 0 || index >= static_cast<int>(tabs_.size()))
        return;

    saveCurrentTab();
    tabs_.erase(tabs_.begin() + index);

    if (currentTab_ >= static_cast<int>(tabs_.size()))
        currentTab_ = static_cast<int>(tabs_.size()) - 1;
    else if (index < currentTab_)
        --currentTab_;

    loadTab(currentTab_);
    tabBar_.setTabs(tabNames(), currentTab_);
}

} // namespace livewdf::editor
