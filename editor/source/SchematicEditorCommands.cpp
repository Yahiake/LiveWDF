#include "SchematicEditor.h"
#include "SchematicEditorInternal.h"
#include "EditorLookAndFeel.h"
#include "format/PartLibrary.h"
#include "Shortcut.h"
#include "gui/Fonts.h"
#include "gui/glow/Glow.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace livewdf::editor {
bool SchematicEditor::commandKeyPressed(const juce::KeyPress& key) {
    // isCommandDown() is the whole of "respect the system's binding": JUCE maps
    // it to Command on macOS and Control elsewhere.
    if (keyIs(key, 'z')) {
        // Shift-Cmd-Z is redo everywhere; Ctrl-Y is the other redo Windows users
        // reach for, handled below. The history is the editor's, so the canvas
        // asks rather than acts.
        const bool redo = key.getModifiers().isShiftDown();
        if (redo) {
            if (onRedoRequested) onRedoRequested();
        } else {
            if (onUndoRequested) onUndoRequested();
        }
        return true;
    }

    if (keyIs(key, 'y')) {
        if (onRedoRequested) onRedoRequested();
        return true;
    }

    if (keyIs(key, 'd')) { duplicateSelection(); return true; }
    if (keyIs(key, 'r')) { rotate(); return true; }
    if (keyIs(key, 'f')) { flip(); return true; }

    // **Copy, cut and paste.** The selection goes to the *system* clipboard, so
    // a group can be pasted into another editor process -- which is the case
    // that matters when the answer to "how do I get this sub-circuit from that
    // window" is "copy it".
    if (keyIs(key, 'c')) { copySelection(); return true; }
    if (keyIs(key, 'x')) { cutSelection(); return true; }
    if (keyIs(key, 'v')) { pasteClipboard(); return true; }

    return false;
}

bool SchematicEditor::keyPressed(const juce::KeyPress& key) {
    // **Chopping a number mid-typing comes before deleting anything.** While
    // a value edit is pending, backspace means "the last digit was wrong", not
    // "remove the part" -- deleting the part under a half-typed number is the
    // mistake this guard exists to prevent.
    if (!pendingValue_.empty()
        && (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)) {
        pendingValue_.pop_back();
        return true;
    }

    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) {
        if (deleteSelection())
            return true;
    }

    // **A part shortcut that uses the command modifier is matched first.** The
    // gain block's "Ctrl+G" is a *part* shortcut that happens to use the
    // command modifier, and the commands take Cmd+Z, Cmd+Y, Cmd+D, Cmd+R and
    // Cmd+F -- so the two do not overlap, and the part's shortcut is simply
    // looked for before the command branch claims the key.
    for (const auto& spec : partPalette()) {
        if (shortcutUsesModifier(spec.key) && matchesShortcut(key, spec.key)) {
            armPlacement(spec.type);
            return true;
        }
    }

    // Command turns a letter into an action on what is already there, which
    // leaves the bare letters free to name parts.
    if (key.getModifiers().isCommandDown())
        return commandKeyPressed(key);

    if (keyIs(key, 's')) { setTool(Tool::Select); pendingValue_.clear(); return true; }
    if (keyIs(key, 'w')) { setTool(Tool::Wire); pendingValue_.clear(); return true; }
    if (keyIs(key, 'x')) { setTool(Tool::Delete); pendingValue_.clear(); return true; }

    // F frames the drawing. A verb among nouns, allowed because it moves the
    // *view* rather than the sheet; Cmd-F still flips the selection.
    if (keyIs(key, 'f')) { frameAll(); pendingValue_.clear(); return true; }

    // **Enter commits a number typed over one selected part.** The number sat
    // in `pendingValue_` since the first digit; whoever edits values answers
    // with it, and its own parse decides what "2k7" means -- the same answer
    // the inspector's field gives, so the two routes cannot disagree.
    if (key == juce::KeyPress::returnKey) {
        if (pendingValue_.empty())
            return false;
        if (onQuickValueEdit)
            onQuickValueEdit(juce::String(pendingValue_));
        pendingValue_.clear();
        return true;
    }

    // **A digit typed over exactly one selected part starts a value edit** --
    // typing what you are looking at, rather than hunting for the field to
    // click into. The guard is the selection state: nothing selected, two
    // things selected, or another tool armed all leave digits meaning what
    // they used to (nothing, today: every palette shortcut is a letter, so a
    // digit never named a part). Suffix letters join a number *only* once one
    // has started -- so a bare `K` still arms the Controlled R it names, and a
    // bare `G` still grounds -- while "1meg" stays one number to the parse.
    if (tool_ == Tool::Select && selectedIds_.size() == 1 && selectedWireIds_.empty()) {
        const auto wc = key.getTextCharacter();
        const bool starts = ((wc >= '0' && wc <= '9') || wc == '.'
                             || (pendingValue_.empty() && wc == '-'));
        const bool continues = (!pendingValue_.empty()
                                && ((wc == 'e' || wc == 'E' || wc == 'k' || wc == 'K'
                                     || wc == 'm' || wc == 'M')
                                    || ((wc == 'g' || wc == 'G')
                                        && (pendingValue_.back() == 'e'
                                            || pendingValue_.back() == 'E'))));
        if ((starts || continues) && pendingValue_.size() < 16) {
            pendingValue_.push_back(static_cast<char>(wc));
            return true;
        }
    }

    // Parts by shortcut, arming one for placing exactly as the palette does.
    // The shortcut is `PartSpec::key`, chosen to be unique across the palette
    // and never to collide with the tool letters above; the palette prints it
    // on the row, so this is "discoverable by looking" rather than a hidden
    // second table.
    for (const auto& spec : partPalette()) {
        if (matchesShortcut(key, spec.key)) {
            pendingValue_.clear();   // an abandoned number is abandoned
            armPlacement(spec.type);
            return true;
        }
    }

    if (key == juce::KeyPress::escapeKey) {
        pendingValue_.clear();   // Esc drops a half-typed number, not just the tool
        setTool(Tool::Select);

        // Both lists: clearing only the parts would keep the wires, which is what
        // the box flow wants of a narrower clear but not what Esc means.
        setSelection(std::string());
        setSelectedWires({});
        return true;
    }

    return false;
}

// ============================================================================
// Change notification
// ============================================================================

void SchematicEditor::notifyChanged() {
    dirty_ = true;

    // **The geometry is rebuilt here, not by the caller.**
    //
    // The wires' paths are built in *pixels* (`rebuildGeometry`), so any change
    // to the model can leave them where they were: the parts are drawn from the
    // model through the current view and move with it, and the wires do not.
    // A drag that moved a wire and did not rebuild them showed the wire's two
    // ends moving with the mouse and the wire itself staying where it was --
    // the drawing only became right again when the mouse was released and
    // something else rebuilt it.
    //
    // So the geometry is rebuilt wherever the model changes, and no caller has
    // to remember to.
    rebuildGeometry();

    syncDocument();
    repaint();

    if (onChanged)
        onChanged();
}

void SchematicEditor::notify(const juce::String& text) {
    if (onNotice)
        onNotice(text);
}
} // namespace livewdf::editor