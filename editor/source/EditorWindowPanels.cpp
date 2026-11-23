#include "EditorWindow.h"
#include "EditorWindowInternal.h"
#include "DiagLog.h"
#include "EditorLookAndFeel.h"
#include "gui/Fonts.h"

#include <algorithm>

namespace livewdf::editor {

// ============================================================================
// The message console
// ============================================================================

void EditorWindow::Content::presentMessages(
    juce::String headline, MessageConsole::Severity severity,
    std::vector<MessageConsole::Message> messages) {
    consoleHeadline_ = std::move(headline);
    consoleHeadlineSeverity_ = severity;
    consoleMessages_ = std::move(messages);

    console_.setContent(consoleHeadline_, consoleHeadlineSeverity_, consoleMessages_);

    // **Problems open the drawer; a clean build closes it only if the drawer
    // opened itself.** A user who opened the console to watch a build should
    // not have it vanish under them, but a drawer that appeared because of an
    // error should get out of the way when the error is fixed.
    if (console_.hasProblems() && !consoleOpen_) {
        consoleOpen_ = true;
        consoleAutoOpened_ = true;
    } else if (!console_.hasProblems() && consoleAutoOpened_) {
        consoleOpen_ = false;
        consoleAutoOpened_ = false;
    }

    refreshConsole();
}

void EditorWindow::Content::toggleConsole() {
    consoleOpen_ = !consoleOpen_;
    consoleAutoOpened_ = false;
    refreshConsole();
}

void EditorWindow::Content::refreshConsole() {
    const bool wasVisible = console_.isVisible();
    console_.setVisible(consoleOpen_);

    const int problems = console_.problemCount();
    const juce::String text =
        problems > 0 ? "Messages " + juce::String(problems) : juce::String("Messages");

    if (messagesBtn_.getButtonText() != text)
        messagesBtn_.setButtonText(text);

    messagesBtn_.setToggleState(consoleOpen_, juce::dontSendNotification);

    const auto colour = problems > 0 ? glow::getColourAmber() : glow::getColourTextSecondary();
    messagesBtn_.setColour(juce::TextButton::textColourOffId, colour);
    messagesBtn_.setColour(juce::TextButton::textColourOnId,
                           problems > 0 ? glow::getColourAmber()
                                        : glow::getColourTextPrimary());

    if (wasVisible != consoleOpen_)
        resized();
    else
        console_.repaint();
}

void EditorWindow::Content::addConsoleNotice(const juce::String& text) {
    // Notices are newest-first and capped: the drawer is a readout, not an
    // unbounded log. The build result stays underneath them.
    consoleMessages_.insert(consoleMessages_.begin(),
                            MessageConsole::Message{text, MessageConsole::Severity::Notice, {}});
    constexpr size_t kMaxConsoleRows = 50;
    if (consoleMessages_.size() > kMaxConsoleRows)
        consoleMessages_.pop_back();

    console_.setContent(consoleHeadline_, consoleHeadlineSeverity_, consoleMessages_);

    // Adding a notice can turn a clean console into one with a problem only if
    // the notice itself is a problem; it is not. Do not force the drawer open.
    refreshConsole();
}

// ============================================================================
// The preset library
// ============================================================================

void EditorWindow::Content::placePresetLibrary() {
    auto anchor = presetsBtn_.getBounds().translated(0, presetsBtn_.getHeight() + 6);
    const int width = 500;
    const int bottomLimit = getHeight() - kStatusH - 8;
    const int height = juce::jlimit(120, 480, bottomLimit - anchor.getY());

    juce::Rectangle<int> panel(anchor.getX(), anchor.getY(), width, height);
    if (panel.getRight() > getWidth() - 8)
        panel.setX(getWidth() - width - 8);
    panel.setX(juce::jmax(8, panel.getX()));

    presetLibrary_.setBounds(panel);
    presetDismisser_.setBounds(getLocalBounds());
}

void EditorWindow::Content::showPresetLibrary() {
    presetLibrary_.setFolder(presetFolder_);
    presetLibrary_.setCurrentFile(currentFile());
    presetLibrary_.setSearchText({});
    placePresetLibrary();

    presetDismisser_.setVisible(true);
    presetDismisser_.toFront(false);
    presetLibrary_.setVisible(true);
    presetLibrary_.toFront(false);
    presetLibrary_.focusSearch();
}

void EditorWindow::Content::closePresetLibrary() {
    presetLibrary_.setVisible(false);
    presetDismisser_.setVisible(false);
}

void EditorWindow::Content::togglePresetLibrary() {
    if (presetLibrary_.isVisible())
        closePresetLibrary();
    else
        showPresetLibrary();
}

void EditorWindow::Content::choosePresetFolder() {
    closePresetLibrary();

    folderChooser_ = std::make_unique<juce::FileChooser>(
        "Choose a preset folder", presetFolder_, juce::String());

    folderChooser_->launchAsync(juce::FileBrowserComponent::openMode |
                                    juce::FileBrowserComponent::canSelectDirectories,
                                [this](const juce::FileChooser& fc) {
                                    const auto folder = fc.getResult();
                                    if (!folder.isDirectory())
                                        return;

                                    presetFolder_ = folder;
                                    presetLibrary_.setFolder(folder);
                                    showPresetLibrary();
                                });
}

void EditorWindow::Content::saveCurrentToPresetFolder() {
    const auto result = presetFolder_.createDirectory();
    if (result.failed()) {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "Could not create preset folder",
            presetFolder_.getFullPathName() + "\n" + result.getErrorMessage());
        return;
    }

    presetLibrary_.setFolder(presetFolder_);

    const auto file = currentFile();
    if (file != juce::File() && file.isAChildOf(presetFolder_)) {
        if (saveFile(file))
            presetLibrary_.setCurrentFile(file);
        return;
    }

    // A tab that already has a file elsewhere is saved *under* the preset
    // folder, not moved; the original stays where the user put it.
    saveAsToFolder(presetFolder_);
}

// ============================================================================
// The compact Edit menu
// ============================================================================

void EditorWindow::Content::showEditMenu() {
    const bool selected = editor_.hasSelection();
    const bool canPaste = !editor_.clipboard().isEmpty();

    juce::PopupMenu menu;
    menu.addItem(1, "Duplicate", selected);
    menu.addItem(2, "Rotate", selected);
    menu.addItem(3, "Flip", selected);
    menu.addSeparator();
    menu.addItem(4, "Copy", selected);
    menu.addItem(5, "Cut", selected);
    menu.addItem(6, "Paste", canPaste);
    menu.addSeparator();
    menu.addItem(7, "Delete", selected);

    juce::Component::SafePointer<Content> safe(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&editMenuBtn_).withMinimumWidth(170),
        [safe](int result) {
            if (safe == nullptr || result == 0)
                return;

            switch (result) {
            case 1: safe->editor_.duplicateSelection(); break;
            case 2: safe->editor_.rotate(); break;
            case 3: safe->editor_.flip(); break;
            case 4: safe->editor_.copySelection(); break;
            case 5: safe->editor_.cutSelection(); break;
            case 6: safe->editor_.pasteClipboard(); break;
            case 7: safe->editor_.deleteSelection(); break;
            default: break;
            }

            safe->editor_.grabKeyboardFocus();
        });
}

}  // namespace livewdf::editor
