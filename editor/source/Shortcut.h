#pragma once

// What a part's keyboard shortcut means, and how it is written down.
//
// **Why this is its own file.** The shortcut is *in the model* -- a part knows
// how to arm itself -- and it is *read* by the palette and *matched* by the
// canvas. Two readers of one string is exactly the arrangement that drifts, so
// the reading and the writing are here together.
//
// **The form.** A shortcut is either a bare letter ("R") or a letter with the
// command modifier ("Ctrl+G"). "Ctrl" means *the command modifier*, which is Cmd
// on a Mac and Ctrl everywhere else; the model does not know what platform it
// is on, and the display says what the user's keyboard actually says.
//
// **Why the gain block needs a modifier.** The bare letters are spoken for: the
// tools take S, W and X, the view takes F, and the commands take Cmd+Z, Cmd+Y,
// Cmd+D, Cmd+R and Cmd+F. That leaves the alphabet, and "G" itself is wanted by
// the ground symbol -- so the gain block takes "Ctrl+G" and the ground takes
// "G".

#include <juce_gui_basics/juce_gui_basics.h>

#include <string>

namespace livewdf::editor {

/// Does this key press match the shortcut?
///
/// A bare letter matches the letter, with or without shift -- the letter is
/// matched case-insensitively, and a *modified* key does not match a bare
/// letter (so Cmd-R does not arm the resistor).
///
/// A modifier shortcut matches the command modifier plus the letter. The
/// command modifier is the system's: Cmd on a Mac, Ctrl elsewhere, which is
/// what JUCE's `isCommandDown()` means.
bool matchesShortcut(const juce::KeyPress& key, const std::string& shortcut);

/// How the shortcut is written for the user to read: "R", or "Ctrl+G" on
/// Windows and Linux and "Cmd+G" on a Mac.
juce::String displayShortcut(const std::string& shortcut);

/// Does the shortcut use the command modifier? True for "Ctrl+G", false for a
/// bare letter.
///
/// It matters because the *command* shortcuts and the *part* shortcuts are
/// matched in different places: a command-modified key is a command unless a
/// part has claimed that particular combination.
bool shortcutUsesModifier(const std::string& shortcut);

}  // namespace livewdf::editor
