#include "EditorWindow.h"
#include "EditorWindowInternal.h"
#include "DiagLog.h"
#include "EditorLayout.h"
#include "EditorLookAndFeel.h"
#include "ElementAccess.h"
#include "gui/Fonts.h"

#include <cmath>

namespace livewdf::editor {

namespace {

/// The first combo box anywhere under `root`. The inspector's rows live inside a
/// viewport now, so they are grandchildren of the panel rather than children.
juce::ComboBox* findFirstPicker(juce::Component& root) {
    for (auto* child : root.getChildren()) {
        if (auto* box = dynamic_cast<juce::ComboBox*>(child))
            return box;

        if (auto* nested = findFirstPicker(*child))
            return nested;
    }
    return nullptr;
}

/// The button with this text, found in the window's tree. The self-test drives
/// the control the user sees rather than a private method beside it.
juce::Button* findButton(juce::Component& root, const juce::String& text) {
    for (auto* child : root.getChildren()) {
        if (auto* button = dynamic_cast<juce::Button*>(child))
            if (button->getButtonText() == text)
                return button;

        if (auto* nested = findButton(*child, text))
            return nested;
    }
    return nullptr;
}

/// The same, for a button whose label carries a count ("Messages 3").
juce::Button* findButtonStartingWith(juce::Component& root, const juce::String& prefix) {
    for (auto* child : root.getChildren()) {
        if (auto* button = dynamic_cast<juce::Button*>(child))
            if (button->getButtonText().startsWith(prefix))
                return button;

        if (auto* nested = findButtonStartingWith(*child, prefix))
            return nested;
    }
    return nullptr;
}

/// A mouse event built the way JUCE 9 wants one: from a real source, aimed at
/// `target` from `position`, with the button bits in the modifiers. The self-test
/// drives gestures the way a user does, so the event it builds has to be the
/// shape the user's is.
juce::MouseEvent makeGestureEvent(const juce::MouseInputSource& source, juce::Component& target,
                                  juce::Point<float> position, const juce::ModifierKeys& mods) {
    return juce::MouseEvent(source, position, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                            &target, &target, juce::Time::getCurrentTime(), position,
                            juce::Time::getCurrentTime(), 1, false);
}

}  // namespace

// ---------------------------------------------------------------------------
// The self-test
//
// These drive the window the way a user does. They exist because the paths
// they cover -- a picker firing a change, the host rebuilding the panel in
// answer, the canvas replacing the document underneath -- are not reachable
// from a unit test, and a crash in one of them took the editor down.
// ---------------------------------------------------------------------------

bool EditorWindow::selfTestPlacePart(const juce::String& type) {
    auto& editor = content_.editor();

    // Put it on the sheet, the way the palette does: arm the type, then place.
    editor.armPlacement(type.toStdString());

    // **Each part gets its own place.** Stacked on top of each other they hide
    // one another, and the hit test -- correctly -- finds the one on top, so a
    // click aimed at the triode would find the resistor and the self-test
    // would report a failure that is really about the test.
    const int column = selfTestColumn_++;
    if (!editor.placePartAt(4 + column * 8, 4))
        return false;

    for (const auto& part : editor.schematic().getParts())
        if (part.type == type.toStdString())
            return true;
    return false;
}

bool EditorWindow::selfTestSelectPart(const juce::String& type) {
    auto& editor = content_.editor();

    // **A real click.** The crash was on a click, and a click is more than a
    // selection: it goes through the hit test, the gesture, the tool, and the
    // canvas's own idea of what a click means. Calling `selectPart` would skip
    // all of that, and would not have found the bug.
    //
    // The event is built from the *real* mouse source, because JUCE 9's
    // `MouseEvent` is built around one and a made-up source is not the same
    // thing the user has.
    const auto& sources = juce::Desktop::getInstance().getMouseSources();
    if (sources.isEmpty())
        return false;

    // Placing a part leaves the Place tool armed -- that is deliberate, so a
    // row of resistors is one gesture. A *click* with the Place tool armed
    // places another part rather than selecting, so the tool goes back to Select
    // first, which is what the user does.
    editor.setTool(SchematicEditor::Tool::Select);

    for (const auto& part : editor.schematic().getParts()) {
        if (part.type != type.toStdString())
            continue;

        const auto centre = editor.partCentrePixel(part);

        const juce::ModifierKeys mods = juce::ModifierKeys::leftButtonModifier;

        // **Say what the click found.** A self-test that only says "failed"
        // makes the reader guess; one that says "the click found nothing" says
        // the hit test is wrong, and one that says "the click found it but
        // nothing got selected" says the selection is wrong.
        const auto hit = editor.partAtPixel(centre);
        if (hit != part.id) {
            selfTestNote_ = "the click found '" + hit + "', not '" + part.id + "'";
            return false;
        }

        const auto source = sources.getReference(0);

        editor.mouseDown(juce::MouseEvent(source, centre, mods,
                                          1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                          &editor, &editor,
                                          juce::Time::getCurrentTime(), centre,
                                          juce::Time::getCurrentTime(), 1, false));
        editor.mouseUp(juce::MouseEvent(source, centre, mods,
                                        1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                        &editor, &editor,
                                        juce::Time::getCurrentTime(), centre,
                                        juce::Time::getCurrentTime(), 1, false));

        if (editor.selectedPartId() != part.id) {
            selfTestNote_ = "the click hit '" + part.id + "' but the selection is '"
                          + editor.selectedPartId() + "'";
            return false;
        }

        return true;
    }
    return false;
}

bool EditorWindow::selfTestArmByShortcut(char letter, bool withCommand,
                                         const juce::String& expectedType) {
    auto& editor = content_.editor();

    // **A real key press, delivered the way the OS delivers one**: the key code
    // is the character, and the text character is what the layout produced. The
    // canvas matches on the text character first, so a press built any other
    // way would not exercise the same path the user's does.
    const auto code = static_cast<int>(static_cast<juce::juce_wchar>(letter));
    const auto mods = withCommand ? juce::ModifierKeys::ctrlModifier
                                   : juce::ModifierKeys();

    editor.keyPressed(juce::KeyPress(code, mods, static_cast<juce::juce_wchar>(letter)));

    const auto armed = editor.pendingPlacement();
    if (armed != expectedType.toStdString()) {
        selfTestNote_ = "the key press armed '" + juce::String(armed) + "', not '"
                      + expectedType + "'";
        return false;
    }

    return true;
}

bool EditorWindow::selfTestSelectWire() {
    auto& editor = content_.editor();

    const auto& wires = editor.schematic().getWires();
    if (wires.empty()) {
        selfTestNote_ = "the sheet has no wire to click";
        return false;
    }

    // **A real click**, on the middle of a wire. The event is built from the
    // real mouse source, because JUCE 9's `MouseEvent` is built around one and
    // a made-up source is not the same thing the user has.
    const auto& sources = juce::Desktop::getInstance().getMouseSources();
    if (sources.isEmpty())
        return false;

    const auto& w = wires.front();
    const auto middle = editor.gridToPixel((w.ax + w.bx) / 2, (w.ay + w.by) / 2);

    editor.setTool(SchematicEditor::Tool::Select);

    const auto& source = sources.getReference(0);
    const juce::ModifierKeys mods = juce::ModifierKeys::leftButtonModifier;

    editor.mouseDown(juce::MouseEvent(source, middle, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                      &editor, &editor, juce::Time::getCurrentTime(), middle,
                                      juce::Time::getCurrentTime(), 1, false));
    editor.mouseUp(juce::MouseEvent(source, middle, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    &editor, &editor, juce::Time::getCurrentTime(), middle,
                                    juce::Time::getCurrentTime(), 1, false));

    // **The wire is what got selected** -- and no part.
    //
    // This is the complaint the hit boxes were fixed for: a part used to claim
    // a quarter of the space around itself, so a wire that reached a part
    // could not be picked at all near the part -- the part won every time.
    if (editor.selectedWireIds().size() != 1) {
        selfTestNote_ = "the click selected "
                      + juce::String(editor.selectedWireIds().size()) + " wires, not one";
        return false;
    }

    if (!editor.selectedPartId().empty()) {
        selfTestNote_ = "the click selected the part '" + editor.selectedPartId()
                      + "' as well as the wire";
        return false;
    }

    return true;
}

bool EditorWindow::selfTestMissPart() {
    auto& editor = content_.editor();

    // A resistor: a narrow symbol, so "beside it" is somewhere a click can
    // land without being on it.
    const wdf::SchematicComponent* resistor = nullptr;
    for (const auto& part : editor.schematic().getParts())
        if (part.type == "res") {
            resistor = &part;
            break;
        }

    if (resistor == nullptr) {
        selfTestNote_ = "the sheet has no resistor to click beside";
        return false;
    }

    // **A point near it that is on nothing.** The offsets are tried in turn and
    // the first that lands on no part is used -- so the check does not depend on
    // where the sheet happens to put its parts, and it cannot pass by clicking
    // into the void.
    juce::Point<float> beside;
    bool found = false;

    for (const auto& offset : {juce::Point<float>(-3.0f, 0.0f), juce::Point<float>(3.0f, 0.0f),
                               juce::Point<float>(0.0f, -4.0f), juce::Point<float>(0.0f, 4.0f),
                               juce::Point<float>(-4.0f, -4.0f), juce::Point<float>(4.0f, 4.0f)}) {
        const auto candidate = editor.partCentrePixel(*resistor)
                               + offset * static_cast<float>(editor.gridSize());

        if (editor.partAtPixel(candidate).empty()) {
            beside = candidate;
            found = true;
            break;
        }
    }

    if (!found) {
        selfTestNote_ = "there was nowhere to click beside the resistor";
        return false;
    }

    const auto& sources = juce::Desktop::getInstance().getMouseSources();
    if (sources.isEmpty())
        return false;

    editor.setTool(SchematicEditor::Tool::Select);

    const auto& source = sources.getReference(0);
    const juce::ModifierKeys mods = juce::ModifierKeys::leftButtonModifier;

    editor.mouseDown(juce::MouseEvent(source, beside, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                      &editor, &editor, juce::Time::getCurrentTime(), beside,
                                      juce::Time::getCurrentTime(), 1, false));
    editor.mouseUp(juce::MouseEvent(source, beside, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    &editor, &editor, juce::Time::getCurrentTime(), beside,
                                    juce::Time::getCurrentTime(), 1, false));

    // **Nothing is selected.** A part used to claim a whole grid square of
    // space around itself, so clicking near one grabbed it.
    if (!editor.selectedPartId().empty()) {
        selfTestNote_ = "the click beside the resistor selected '" + editor.selectedPartId()
                      + "'";
        return false;
    }

    return true;
}

bool EditorWindow::selfTestDragPart() {
    auto& editor = content_.editor();

    // A resistor: a narrow symbol, easy to grab in the middle.
    const wdf::SchematicComponent* resistor = nullptr;
    for (const auto& part : editor.schematic().getParts())
        if (part.type == "res") {
            resistor = &part;
            break;
        }

    if (resistor == nullptr) {
        selfTestNote_ = "the sheet has no resistor to drag";
        return false;
    }

    const auto& sources = juce::Desktop::getInstance().getMouseSources();
    if (sources.isEmpty())
        return false;

    editor.setTool(SchematicEditor::Tool::Select);

    // The part's centre, and three squares below it -- a whole number of
    // squares, so the drag is a real move and not a fraction of one.
    //
    // **The position before the drag, taken by value**: the pointer is into
    // the parts vector, and the drag moves the part in it.
    const auto startY = resistor->visual.y;
    const auto centre = editor.partCentrePixel(*resistor);
    const auto target = editor.gridToPixel(
        static_cast<int>(std::lround(resistor->visual.x)),
        static_cast<int>(std::lround(resistor->visual.y)) + 3);

    const auto& source = sources.getReference(0);
    const juce::ModifierKeys mods = juce::ModifierKeys::leftButtonModifier;

    editor.mouseDown(juce::MouseEvent(source, centre, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                      &editor, &editor, juce::Time::getCurrentTime(), centre,
                                      juce::Time::getCurrentTime(), 1, false));
    editor.mouseDrag(juce::MouseEvent(source, target, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                      &editor, &editor, juce::Time::getCurrentTime(), target,
                                      juce::Time::getCurrentTime(), 1, false));
    editor.mouseUp(juce::MouseEvent(source, target, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                   &editor, &editor, juce::Time::getCurrentTime(), target,
                                   juce::Time::getCurrentTime(), 1, false));

    // **The part moved** ...
    const auto moved = editor.schematic().getParts();
    for (const auto& part : moved)
        if (part.id == resistor->id)
            if (part.visual.y == startY) {
                selfTestNote_ = "the drag did not move the part";
                return false;
            }

    // ... **and the sheet says so**: the Rebuild button is unlocked, because
    // the host was told.
    if (!content_.isRebuildPending()) {
        selfTestNote_ = "the drag moved the part but the sheet was not marked as changed";
        return false;
    }

    return true;
}

bool EditorWindow::selfTestRebuild() {
    // The Rebuild button's action, through the same seam the button uses.
    content_.rebuild();
    return content_.editor().schematic().getParts().size() > 0;
}

bool EditorWindow::selfTestPresetLibrary() {
    auto* button = findButton(content_, "Presets");
    if (button == nullptr) {
        selfTestNote_ = "the tab strip has no Presets button";
        return false;
    }

    // Call the click handler directly rather than `triggerClick`: the latter
    // posts a message, and this step's answer would be checked before the
    // message loop had delivered it.
    if (!button->onClick) {
        selfTestNote_ = "the Presets button has no click handler";
        return false;
    }

    button->onClick();

    auto* panel = content_.findChildWithID("presetLibrary");
    if (panel == nullptr || !panel->isVisible()) {
        selfTestNote_ = "the Presets button did not show the library";
        return false;
    }

    if (panel->getWidth() < 200 || panel->getHeight() < 120) {
        selfTestNote_ = "the preset library opened with a size of "
                      + juce::String(panel->getWidth()) + "x"
                      + juce::String(panel->getHeight());
        return false;
    }

    button->onClick();
    if (panel->isVisible()) {
        selfTestNote_ = "the Presets button did not close the library";
        return false;
    }

    return true;
}

bool EditorWindow::selfTestMessageConsole() {
    auto* button = findButtonStartingWith(content_, "Messages");
    if (button == nullptr) {
        selfTestNote_ = "the status bar has no Messages button";
        return false;
    }
    if (!button->onClick) {
        selfTestNote_ = "the Messages button has no click handler";
        return false;
    }

    auto* console = content_.findChildWithID("messageConsole");
    if (console == nullptr) {
        selfTestNote_ = "the window has no message console";
        return false;
    }

    const bool wasVisible = console->isVisible();
    button->onClick();

    if (console->isVisible() == wasVisible) {
        selfTestNote_ = "the Messages button did not toggle the console";
        return false;
    }
    if (console->isVisible() && console->getHeight() < 120) {
        selfTestNote_ = "the open console is only "
                      + juce::String(console->getHeight()) + " pixels tall";
        return false;
    }

    button->onClick();
    return console->isVisible() == wasVisible;
}

bool EditorWindow::selfTestMessageConsoleAutoOpen() {
    // A fresh sheet with one part and no wire: the build either rejects it or
    // reports it unconnected, and either way the drawer has something to say.
    content_.newTab();
    if (!selfTestPlacePart("res")) {
        selfTestNote_ = "could not put the unconnected resistor on the fresh sheet";
        return false;
    }

    content_.rebuild();

    auto* console = dynamic_cast<MessageConsole*>(content_.findChildWithID("messageConsole"));
    if (console == nullptr) {
        selfTestNote_ = "the window has no message console";
        return false;
    }
    if (!console->isVisible()) {
        selfTestNote_ = "a build with a problem left the console closed";
        return false;
    }
    if (!console->hasProblems() || console->problemCount() < 1) {
        selfTestNote_ = "the console opened but counted no problem";
        return false;
    }

    auto* button = findButtonStartingWith(content_, "Messages");
    if (button == nullptr || !button->onClick) {
        selfTestNote_ = "the status bar has no working Messages button";
        return false;
    }

    button->onClick();
    if (console->isVisible()) {
        selfTestNote_ = "the Messages button did not close the auto-opened console";
        return false;
    }

    return true;
}

bool EditorWindow::selfTestChangeFirstChoice(const juce::String& type) {
    auto& editor = content_.editor();
    auto& inspector = content_.inspector();

    // The picker's own control, found the way anything else would find it: in
    // the inspector's tree (the rows scroll inside a viewport).
    juce::ComboBox* box = findFirstPicker(inspector);

    if (box == nullptr) {
        selfTestNote_ = "the inspector has no picker for a " + type;
        return false;
    }

    // Choose a different entry, and let the change be delivered.
    const int current = box->getSelectedId();
    const int other = current == 1 ? 2 : 1;
    if (other > box->getNumItems())
        return false;

    // **The popup's own path.** When the user picks an item from a ComboBox's
    // dropdown, JUCE calls `setSelectedId` with the *default* notification --
    // which is asynchronous -- and the message loop delivers it a moment later.
    // So the change is posted here too, and the self-test's message-loop step
    // is what delivers it.
    //
    // That matters: the crash was in the *delivery*, not in the choosing. The
    // delivery is where the host reacts, and the host's reaction rebuilds the
    // panel -- which destroys the very ComboBox whose callback is running.
    box->setSelectedId(other);

    // The part must carry the new choice.
    for (const auto& part : editor.schematic().getParts())
        if (part.type == type.toStdString())
            return true;

    return false;
}

// ---------------------------------------------------------------------------
// Undo
// ---------------------------------------------------------------------------

bool EditorWindow::selfTestUndoRedo() {
    auto& editor = content_.editor();

    // **A tab of its own.** Undo never reaches across a tab, so this step's
    // answer must not depend on the edits the earlier steps left on the demo
    // sheet -- and on a fresh tab the history starts empty, which is the state
    // a user's first drag is actually in.
    content_.newTab();

    if (!selfTestPlacePart("res")) {
        selfTestNote_ = "the resistor could not be put on the fresh sheet";
        return false;
    }

    // The part and where it is, taken by value: the drag replaces the parts
    // vector, so nothing pointing into it survives the gesture.
    std::string id;
    float startX = 0.0f;
    float startY = 0.0f;
    for (const auto& part : editor.schematic().getParts()) {
        if (part.type != "res")
            continue;
        id = part.id;
        startX = part.visual.x;
        startY = part.visual.y;
        break;
    }
    if (id.empty()) {
        selfTestNote_ = "the fresh sheet has no resistor to drag";
        return false;
    }

    // The part's height above the sheet, by id rather than by pointer.
    auto partY = [&id, &editor] {
        for (const auto& part : editor.schematic().getParts())
            if (part.id == id)
                return static_cast<double>(part.visual.y);
        return -1.0;
    };

    auto* undoButton = findButton(content_, "Undo");
    auto* redoButton = findButton(content_, "Redo");
    if (undoButton == nullptr || redoButton == nullptr || !undoButton->onClick
        || !redoButton->onClick) {
        selfTestNote_ = "the toolbar has no working Undo/Redo buttons";
        return false;
    }

    // **The placement is a landed step**, so Undo is offered before the drag
    // begins -- and that is what makes the drag's own step distinguishable: the
    // button has to go dark again while the pointer is down.
    if (!undoButton->isEnabled()) {
        selfTestNote_ = "placing a part left Undo dark";
        return false;
    }

    const auto& sources = juce::Desktop::getInstance().getMouseSources();
    if (sources.isEmpty())
        return false;

    const auto& source = sources.getReference(0);
    const juce::ModifierKeys held = juce::ModifierKeys::leftButtonModifier;

    editor.setTool(SchematicEditor::Tool::Select);

    // Three squares below: a whole gesture, not a fraction of one.
    const auto centre = editor.gridToPixel(static_cast<int>(std::lround(startX)),
                                           static_cast<int>(std::lround(startY)));
    const auto target = editor.gridToPixel(static_cast<int>(std::lround(startX)),
                                           static_cast<int>(std::lround(startY)) + 3);

    editor.mouseDown(makeGestureEvent(source, editor, centre, held));
    editor.mouseDrag(makeGestureEvent(source, editor, target, held));

    if (!editor.isMidGesture()) {
        selfTestNote_ = "the canvas did not report a gesture in progress";
        return false;
    }

    // **Undo mid-drag is not a step**, so the label says the same: it is dark
    // while the pointer is down, even though the placement before the drag was
    // a step the drag has not taken yet.
    if (undoButton->isEnabled()) {
        selfTestNote_ = "Undo was offered in the middle of a drag";
        return false;
    }

    // The user's route is the keyboard, and it used to reach the history: the
    // part snapped back under a pointer that was still moving, and the rest of
    // the drag started from where it was put down. The drawing must not move.
    const auto yBeforeKey = partY();
    editor.keyPressed(juce::KeyPress(static_cast<int>(static_cast<juce::juce_wchar>('z')),
                                     juce::ModifierKeys::ctrlModifier,
                                     static_cast<juce::juce_wchar>('z')));
    if (partY() != yBeforeKey) {
        selfTestNote_ = "undoing in the middle of a drag moved the sheet";
        return false;
    }

    // The gesture lands, and only here does its step exist.
    editor.mouseUp(makeGestureEvent(source, editor, target, held));

    if (!undoButton->isEnabled()) {
        selfTestNote_ = "the drag ended but Undo was not offered";
        return false;
    }

    // **One press takes the whole drag back**, not one square at a time.
    undoButton->onClick();
    if (partY() != static_cast<double>(startY)) {
        selfTestNote_ = "Undo left the part where the drag had put it";
        return false;
    }
    if (!redoButton->isEnabled()) {
        selfTestNote_ = "undoing the drag left Redo dark";
        return false;
    }

    redoButton->onClick();
    if (partY() == static_cast<double>(startY)) {
        selfTestNote_ = "redo did not put the gesture back";
        return false;
    }

    return true;
}

bool EditorWindow::selfTestInspectorValueEdit() {
    content_.newTab();

    if (!selfTestPlacePart("res")) {
        selfTestNote_ = "the resistor could not be put on the fresh sheet";
        return false;
    }

    // Aim the inspector at it the way a user does: by selecting it.
    if (!selfTestSelectPart("res")) {
        selfTestNote_ = "the resistor on the fresh sheet could not be clicked";
        return false;
    }

    // Type a new number into the panel's own field. A placed resistor is 1K, so
    // 30K is a change and the panel has something to tell the host.
    content_.inspector().quickValueEdit("30K");

    // **The panel defers the notification**, so this step only posts it: the
    // message loop between the self-test's steps is what lets the canvas reload
    // and the history record the edit.
    return true;
}

bool EditorWindow::selfTestInspectorUndo() {
    auto& editor = content_.editor();

    // The resistor's number, re-found each time: undo and redo replace the parts
    // vector.
    auto number = [&editor] {
        for (const auto& part : editor.schematic().getParts())
            if (part.type == "res")
                return valueOf(part);
        return -1.0;
    };

    const double edited = 30000.0;
    if (number() != edited) {
        selfTestNote_ = "the inspector's edit had not landed when Undo was checked";
        return false;
    }

    auto* undoButton = findButton(content_, "Undo");
    if (undoButton == nullptr || !undoButton->onClick) {
        selfTestNote_ = "the toolbar has no working Undo button";
        return false;
    }

    // **An inspector edit is an undo step too.** The panel edits the document and
    // never passes through the canvas's notify, so without its own record this is
    // the one visible edit Undo could not take back.
    if (!undoButton->isEnabled()) {
        selfTestNote_ = "the inspector's edit left Undo dark";
        return false;
    }

    undoButton->onClick();
    if (number() == edited) {
        selfTestNote_ = "undo left the typed number in place";
        return false;
    }

    auto* redoButton = findButton(content_, "Redo");
    if (redoButton == nullptr || !redoButton->isEnabled()) {
        selfTestNote_ = "undoing an inspector edit left Redo dark";
        return false;
    }

    redoButton->onClick();
    if (number() != edited) {
        selfTestNote_ = "redo did not put the typed number back";
        return false;
    }

    return true;
}

} // namespace livewdf::editor