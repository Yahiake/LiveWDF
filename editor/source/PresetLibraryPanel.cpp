#include "PresetLibraryPanel.h"

#include "FactorySchematics.h"
#include "gui/Fonts.h"
#include "gui/glow/Glow.h"

#include <algorithm>
#include <utility>

namespace livewdf::editor {

namespace {
constexpr int kRowH = 24;
constexpr int kSearchH = 26;
constexpr int kFilterH = 24;
constexpr int kPad = 9;
constexpr int kMaxFiles = 2000;

/** Natural order: "Bass 9" before "Bass 10". */
void sortFiles(juce::Array<juce::File>& files, const juce::File& root) {
    std::sort(files.begin(), files.end(), [&root](const juce::File& a, const juce::File& b) {
        const auto relativeA = a.getRelativePathFrom(root);
        const auto relativeB = b.getRelativePathFrom(root);
        const auto order = relativeA.compareNatural(relativeB);
        return order != 0 ? order < 0 : relativeA < relativeB;
    });
}

int phraseMatch(const juce::String& name, const juce::String& phrase) {
    if (name.equalsIgnoreCase(phrase))
        return 3;
    if (name.startsWithIgnoreCase(phrase))
        return 2;
    return name.containsIgnoreCase(phrase) ? 1 : 0;
}
}  // namespace

FactoryPresetStepper::FactoryPresetStepper(juce::Component& parent, juce::Label& name)
    : name_(name) {
    previous_.setTitle("Previous factory preset");
    next_.setTitle("Next factory preset");
    previous_.setTooltip(previous_.getTitle());
    next_.setTooltip(next_.getTitle());
    previous_.onClick = [this] { step(-1); };
    next_.onClick = [this] { step(1); };
    parent.addChildComponent(previous_);
    parent.addChildComponent(next_);
}

void FactoryPresetStepper::setFactoryFolder(const juce::File& folder) {
    factoryFolder_ = folder;
    browseOrder_.clear();
    defaultOrder_.clear();
    defaultOrderLoaded_ = false;
    updateButtons();
}

void FactoryPresetStepper::setCurrentFile(const juce::File& file) {
    if (currentFile_ == file)
        return;
    currentFile_ = file;
    updateButtons();
}

void FactoryPresetStepper::setBrowseOrder(Order order) {
    browseOrder_ = std::move(order);
    updateButtons();
}

void FactoryPresetStepper::setName(const juce::String& name) {
    name_.setText(name, juce::dontSendNotification);
    setBounds(bounds_);
}

void FactoryPresetStepper::setBounds(juce::Rectangle<int> bounds) {
    bounds_ = bounds;
    if (previous_.isVisible()) {
        const int buttonWidth = juce::jmin(24, bounds.getWidth() / 4);
        const auto nameWidth = juce::roundToInt(
            juce::GlyphArrangement::getStringWidth(name_.getFont(), name_.getText()))
                             + name_.getBorderSize().getLeftAndRight() + 4;
        const int width = juce::jmin(bounds.getWidth(), nameWidth + 2 * buttonWidth);
        bounds = name_.getJustificationType().testFlags(juce::Justification::right)
                   ? bounds.removeFromRight(width) : bounds.removeFromLeft(width);
        previous_.setBounds(bounds.removeFromLeft(buttonWidth).reduced(1, 0));
        next_.setBounds(bounds.removeFromRight(buttonWidth).reduced(1, 0));
    }
    name_.setBounds(bounds);
}

void FactoryPresetStepper::updateButtons() {
    const bool inFactory = factoryFolder_ != juce::File() && currentFile_ != juce::File()
                        && currentFile_.isAChildOf(factoryFolder_);
    if (inFactory && !defaultOrderLoaded_
        && std::find(browseOrder_.begin(), browseOrder_.end(), currentFile_) == browseOrder_.end()) {
        auto presets = FactorySchematics::list(factoryFolder_);
        std::stable_partition(presets.begin(), presets.end(),
                              [](const auto& preset) { return preset.featured; });
        for (const auto& preset : presets)
            defaultOrder_.push_back(preset.file);
        defaultOrderLoaded_ = true;
    }
    const auto containsCurrent = [this](const Order& order) {
        return std::find(order.begin(), order.end(), currentFile_) != order.end();
    };
    const bool visible = inFactory && (containsCurrent(browseOrder_) || containsCurrent(defaultOrder_));
    previous_.setVisible(visible);
    next_.setVisible(visible);
    previous_.setEnabled(visible && adjacent(-1) != juce::File());
    next_.setEnabled(visible && adjacent(1) != juce::File());
    setBounds(bounds_);
}

juce::File FactoryPresetStepper::adjacent(int offset) const {
    const auto browsePosition = std::find(browseOrder_.begin(), browseOrder_.end(), currentFile_);
    const bool inView = browsePosition != browseOrder_.end();
    const auto& order = inView ? browseOrder_ : defaultOrder_;
    const auto current = inView ? browsePosition : std::find(order.begin(), order.end(), currentFile_);
    if (current == order.end())
        return {};
    for (auto index = current - order.begin() + offset;
         index >= 0 && index < order.end() - order.begin(); index += offset) {
        const auto& file = order[static_cast<size_t>(index)];
        if (file.existsAsFile())
            return file;
    }
    return {};
}

void FactoryPresetStepper::step(int offset) {
    if (!previous_.isVisible())
        return;
    const auto file = adjacent(offset);
    if (file != juce::File() && onPresetChosen)
        onPresetChosen(file);
    else
        updateButtons();
}

PresetLibraryPanel::PresetLibraryPanel(Mode mode) : mode_(mode) {
    setOpaque(false);
    setInterceptsMouseClicks(true, true);

    search_.setFont(Fonts::mono(12.0f));
    search_.setJustification(juce::Justification::centredLeft);
    search_.setBorder(juce::BorderSize<int>(0));
    search_.setIndents(9, 0);
    search_.setTextToShowWhenEmpty("Search presets...", glow::getColourTextSecondary());
    search_.setTitle("Search presets");
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

    effectSelector_.setTitle("Effect type");
    effectSelector_.setColour(juce::ComboBox::backgroundColourId, glow::getColourBackground());
    effectSelector_.setColour(juce::ComboBox::outlineColourId, glow::getColourBorder());
    effectSelector_.setColour(juce::ComboBox::textColourId, glow::getColourTextPrimary());
    effectSelector_.setColour(juce::ComboBox::arrowColourId, glow::getColourTextSecondary());
    effectSelector_.onChange = [this] { applyFilter(); };
    addAndMakeVisible(effectSelector_);

    for (size_t i = 0; i < kScopes.size(); ++i) {
        const auto scope = kScopes[i];
        if (scope == Scope::User && mode_ == Mode::Factory)
            continue;
        auto& button = scopeButtons_[i];
        button.setButtonText(scopeName(scope));
        button.setColour(juce::TextButton::buttonColourId, glow::getColourBackground());
        button.setColour(juce::TextButton::buttonOnColourId, glow::getColourElevated());
        button.setColour(juce::TextButton::textColourOffId, glow::getColourTextSecondary());
        button.setColour(juce::TextButton::textColourOnId, glow::getColourAccent());
        button.onClick = [this, scope] { selectScope(scope); };
        button.setToggleState(scope == scope_, juce::dontSendNotification);
        addAndMakeVisible(button);
    }

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

void PresetLibraryPanel::setFactoryFolder(const juce::File& folder) {
    factoryFolder_ = folder;
    rescan();
}

juce::File PresetLibraryPanel::factoryFolder() const {
    if (factoryFolder_ != juce::File())
        return factoryFolder_;
    return folder_ == juce::File() ? juce::File() : folder_.getChildFile("Factory");
}

void PresetLibraryPanel::setCurrentFile(const juce::File& file) {
    currentFile_ = file;
    list_.repaint();
}

FactoryPresetStepper::Order PresetLibraryPanel::visibleFactoryOrder() const {
    FactoryPresetStepper::Order files;
    for (const auto& entry : shown_)
        if (entry.kind == Kind::File && entry.scope != Scope::User)
            files.push_back(entry.file);
    return files;
}

PresetLibraryPanel::View PresetLibraryPanel::view() const {
    return {scope_, effectSelector_.getText(), searchText_};
}

void PresetLibraryPanel::setView(const View& view) {
    const auto scope = std::find(kScopes.begin(), kScopes.end(), view.scope);
    if (scope != kScopes.end()) {
        const auto& button = scopeButtons_[static_cast<size_t>(scope - kScopes.begin())];
        if (button.isVisible() && button.isEnabled())
            selectScope(view.scope);
    }
    for (int i = 0; i < effectSelector_.getNumItems(); ++i) {
        if (effectSelector_.getItemText(i) == view.effect) {
            effectSelector_.setSelectedItemIndex(i, juce::dontSendNotification);
            break;
        }
    }
    setSearchText(view.search);
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
    case Kind::OpenFactoryFolder:
        factoryFolder().revealToUser();
        if (onDismiss) onDismiss();
        break;
    case Kind::Section:
    case Kind::Note:
        break;
    }

    list_.deselectAllRows();
}

juce::String PresetLibraryPanel::scopeName(Scope scope) {
    using Collection = FactorySchematics::Collection;
    switch (scope) {
        case Scope::All: return "All";
        case Scope::Featured: return "Featured";
        case Scope::User: return "My presets";
        case Scope::Pedals: return FactorySchematics::collectionName(Collection::Pedals);
        case Scope::Studio: return FactorySchematics::collectionName(Collection::Studio);
        case Scope::Amps: return FactorySchematics::collectionName(Collection::Amps);
        case Scope::Other: return FactorySchematics::collectionName(Collection::Other);
    }
    return {};
}

void PresetLibraryPanel::rebuild() {
    files_.clear();
    const auto factoryRoot = factoryFolder();
    if (mode_ == Mode::Editor && folder_.isDirectory()) {
        juce::Array<juce::File> presets;

        for (const auto& entry : juce::RangedDirectoryIterator(
                 folder_, true, "*.livewdf", juce::File::findFiles)) {
            const auto file = entry.getFile();
            if (file.getFileName().startsWithChar('.') || file.isAChildOf(factoryRoot))
                continue;
            presets.add(file);
            if (presets.size() >= kMaxFiles)
                break;
        }
        sortFiles(presets, folder_);
        for (const auto& file : presets) {
            const auto parent = file.getParentDirectory();
            files_.push_back({Kind::File, file.getFileNameWithoutExtension(), file,
                              parent == folder_ ? juce::String() : parent.getRelativePathFrom(folder_),
                              Scope::User, {}, false});
        }
    }

    for (const auto& preset : FactorySchematics::list(factoryRoot)) {
        using Collection = FactorySchematics::Collection;
        const auto scope = preset.collection == Collection::Pedals ? Scope::Pedals
                         : preset.collection == Collection::Studio ? Scope::Studio
                         : preset.collection == Collection::Amps ? Scope::Amps : Scope::Other;
        files_.push_back({Kind::File, preset.file.getFileNameWithoutExtension(), preset.file,
                          {}, scope, preset.effect, preset.featured});
    }
    auto availableScope = scope_;
    for (size_t i = 0; i < kScopes.size(); ++i) {
        const auto scope = kScopes[i];
        const auto hasPresets = std::any_of(files_.begin(), files_.end(), [scope](const Entry& entry) {
            return scope == Scope::Featured ? entry.featured : entry.scope == scope;
        });
        const auto enabled = scope == Scope::All || scope == Scope::User || hasPresets;
        scopeButtons_[i].setEnabled(enabled);
        if (!enabled && scope == scope_)
            availableScope = Scope::All;
    }
    selectScope(availableScope);
}

void PresetLibraryPanel::selectScope(Scope scope) {
    scope_ = scope;
    for (size_t i = 0; i < kScopes.size(); ++i)
        scopeButtons_[i].setToggleState(kScopes[i] == scope_, juce::dontSendNotification);
    updateEffects();
    applyFilter();
}

bool PresetLibraryPanel::matchesScope(const Entry& entry) const {
    if (scope_ == Scope::Featured)
        return entry.featured;
    return scope_ == Scope::All || entry.scope == scope_;
}

void PresetLibraryPanel::updateEffects() {
    juce::StringArray effects;
    for (const auto& entry : files_)
        if (matchesScope(entry) && entry.effect.isNotEmpty())
            effects.addIfNotAlreadyThere(entry.effect, true);
    effects.sortNatural();

    const auto selectedEffect = effectSelector_.getText();
    effectSelector_.clear(juce::dontSendNotification);
    effectSelector_.addItem("All types", 1);
    effectSelector_.addItemList(effects, 2);
    effectSelector_.setSelectedId(effects.indexOf(selectedEffect, true) + 2,
                                  juce::dontSendNotification);
    effectSelector_.setEnabled(!effects.isEmpty());
}

void PresetLibraryPanel::applyFilter() {
    shown_.clear();
    const bool allEffects = effectSelector_.getSelectedId() == 1;
    const bool browsingAll = scope_ == Scope::All && allEffects && searchText_.isEmpty();
    const auto words = juce::StringArray::fromTokens(searchText_, true);
    const auto addRow = [this](Kind kind, juce::String label) {
        shown_.push_back({kind, std::move(label), {}, {}, Scope::User, {}, false});
    };

    if (browsingAll && mode_ == Mode::Editor) {
        addRow(Kind::NewSheet, "New empty sheet");
        addRow(Kind::Starter, "Starter: RC lowpass");
    }
    const auto beforeFiles = shown_.size();

    if (browsingAll) {
        bool featured = false;
        for (const auto& entry : files_) {
            if (!entry.featured)
                continue;
            if (!featured)
                addRow(Kind::Section, "FEATURED");
            featured = true;
            shown_.push_back(entry);
        }
    }

    auto lastScope = Scope::All;
    for (const auto& entry : files_) {
        if (!matchesScope(entry) || (browsingAll && entry.featured)
            || (!allEffects && !entry.effect.equalsIgnoreCase(effectSelector_.getText())))
            continue;

        const auto searchable = entry.label + " " + entry.detail + " " + entry.effect
                              + " " + scopeName(entry.scope);
        if (!std::all_of(words.begin(), words.end(), [&searchable](const juce::String& word) {
                return searchable.containsIgnoreCase(word);
            }))
            continue;

        if (browsingAll && entry.scope != lastScope) {
            addRow(Kind::Section, scopeName(entry.scope).toUpperCase());
            lastScope = entry.scope;
        }
        shown_.push_back(entry);
    }

    if (!words.isEmpty()) {
        const auto phrase = words.joinIntoString(" ");
        const auto rank = [&phrase, &words](const Entry& entry) {
            const auto nameHits = std::count_if(words.begin(), words.end(), [&entry](const auto& word) {
                return entry.label.containsIgnoreCase(word);
            });
            return std::pair{phraseMatch(entry.label, phrase), nameHits};
        };
        std::stable_sort(shown_.begin(), shown_.end(), [&rank](const Entry& a, const Entry& b) {
            return rank(a) > rank(b);
        });
    }

    if (shown_.size() == beforeFiles) {
        if (searchText_.isNotEmpty())
            addRow(Kind::Note, "No presets match \"" + searchText_ + "\"");
        else if (scope_ == Scope::User || (browsingAll && mode_ == Mode::Editor))
            addRow(Kind::Note, "No user presets yet");
        else
            addRow(Kind::Note, "No factory presets found");
    }

    if (searchText_.isEmpty() && allEffects) {
        if (mode_ == Mode::Editor && (scope_ == Scope::All || scope_ == Scope::User)) {
            addRow(Kind::Section, "LIBRARY");
            addRow(Kind::ChooseFolder, "Choose preset folder...");
            addRow(Kind::SaveCurrent, "Save current as preset...");
        } else if (mode_ == Mode::Factory && scope_ == Scope::All && factoryFolder().isDirectory()) {
            addRow(Kind::OpenFactoryFolder, "Open factory folder...");
        }
    }

    list_.deselectAllRows();
    list_.updateContent();
    list_.scrollToEnsureRowIsOnscreen(0);
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
    auto content = getLocalBounds().reduced(kPad);
    auto searchRow = content.removeFromTop(kSearchH);
    effectSelector_.setBounds(searchRow.removeFromRight(juce::jmin(210, searchRow.getWidth() / 2)));
    search_.setBounds(searchRow.withTrimmedRight(5));
    content.removeFromTop(5);
    auto scopes = content.removeFromTop(kFilterH);
    const auto count = mode_ == Mode::Editor ? kScopes.size() : kScopes.size() - 1;
    constexpr int gap = 4;
    const int buttonWidth = (scopes.getWidth() - gap * static_cast<int>(count - 1))
                          / static_cast<int>(count);
    for (size_t i = 0; i < count; ++i) {
        scopeButtons_[i].setBounds(scopes.removeFromLeft(i + 1 == count ? scopes.getWidth() : buttonWidth));
        scopes.removeFromLeft(gap);
    }
    content.removeFromTop(5);
    list_.setBounds(content);
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
        const auto& detail = entry.effect.isNotEmpty() ? entry.effect : entry.detail;

        if (current) {
            g.setColour(glow::getColourAccent());
            g.fillEllipse(4.0f, static_cast<float>(height) * 0.5f - 2.0f, 4.0f, 4.0f);
        }

        g.setColour(current ? glow::getColourAccent() : glow::getColourTextPrimary());
        g.setFont(Fonts::mono(12.0f));

        const int detailW =
            detail.isEmpty()
                ? 0
                : static_cast<int>(juce::jmin(
                      140.0f, juce::GlyphArrangement::getStringWidth(Fonts::mono(10.0f),
                                                                     detail) +
                                  8.0f));
        g.drawText(entry.label, area.withTrimmedLeft(left).withTrimmedRight(detailW + 8),
                   juce::Justification::centredLeft, true);

        if (detail.isNotEmpty()) {
            g.setColour(glow::getColourTextSecondary().withAlpha(0.7f));
            g.setFont(Fonts::mono(10.0f));
            g.drawText(detail, area.withTrimmedRight(6),
                       juce::Justification::centredRight, true);
        }
        break;
    }

    case Kind::NewSheet:
    case Kind::Starter:
    case Kind::ChooseFolder:
    case Kind::SaveCurrent:
    case Kind::OpenFactoryFolder:
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

}  // namespace livewdf::editor
