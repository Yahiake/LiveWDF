#include "Shortcut.h"

namespace livewdf::editor {

namespace {

/// The letters a shortcut is built from, and the modifier that goes with them.
struct Parsed {
    char letter = 0;
    bool withCommand = false;
    bool valid = false;
};

Parsed parse(const std::string& shortcut) {
    Parsed out;

    if (shortcut.empty())
        return out;

    std::string rest = shortcut;

    // "Ctrl+G" -- the command modifier and a letter. The name is the canonical
    // one; what the user's keyboard says is the display's business.
    const std::string modifier = "Ctrl+";
    if (rest.rfind(modifier, 0) == 0) {
        out.withCommand = true;
        rest = rest.substr(modifier.size());
    }

    if (rest.size() != 1)
        return out;

    out.letter = static_cast<char>(std::toupper(static_cast<unsigned char>(rest[0])));
    out.valid = out.letter >= 'A' && out.letter <= 'Z';
    return out;
}

/// Does the key press's *text* match the letter, the way a keyboard layout
/// rather than a code position decides?
///
/// The same rule the editor's other letters use: the character the layout
/// produced is matched first, and the key code is the fallback -- so R is
/// whichever key types an "r" in front of you rather than a position on an
/// American keyboard, and Caps Lock does not break it.
bool letterMatches(const juce::KeyPress& key, char letter) {
    const auto wanted = static_cast<juce::juce_wchar>(letter);
    const auto produced = key.getTextCharacter();
    if (produced != 0 && produced != juce::juce_wchar('\r') && produced != juce::juce_wchar('\n'))
        return static_cast<juce::juce_wchar>(std::toupper(static_cast<unsigned char>(produced))) == wanted;

    return key.getKeyCode() == static_cast<int>(wanted);
}

}  // namespace

bool matchesShortcut(const juce::KeyPress& key, const std::string& shortcut) {
    const auto p = parse(shortcut);
    if (!p.valid)
        return false;

    // A modified key press does not match a bare letter: Cmd-R must not arm the
    // resistor.
    if (!p.withCommand && key.getModifiers().isCommandDown())
        return false;

    if (p.withCommand && !key.getModifiers().isCommandDown())
        return false;

    return letterMatches(key, p.letter);
}

juce::String displayShortcut(const std::string& shortcut) {
    const auto p = parse(shortcut);
    if (!p.valid)
        return {};

    const juce::String letter(juce::String::charToString(p.letter));
    if (!p.withCommand)
        return letter;

    // The command modifier is the system's: Cmd on a Mac, Ctrl elsewhere.
#if JUCE_MAC
    return juce::String("Cmd+") + letter;
#else
    return juce::String("Ctrl+") + letter;
#endif
}

bool shortcutUsesModifier(const std::string& shortcut) {
    return parse(shortcut).withCommand;
}

}  // namespace livewdf::editor
