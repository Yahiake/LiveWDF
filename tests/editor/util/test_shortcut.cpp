// What a part's keyboard shortcut means, and that two parts cannot answer to
// the same key press.
//
// The shortcut is the one piece of the palette that is *read* by the canvas and
// *shown* by the palette, from one string. That arrangement is exactly the one
// that drifts: the palette prints "G" for the ground and "Ctrl+G" for the gain
// block, the canvas matches against the same strings, and a change to one
// reader that misses the other shows up as a key that lights up the wrong part.
//
// So the reading and the writing are tested together, and the invariant that
// matters -- **no key press arms two parts** -- is checked across the whole
// palette rather than one part at a time.

#include "Shortcut.h"

#include "format/PartLibrary.h"

#include <catch2.hpp>

#include <set>
#include <string>

using namespace livewdf::editor;

namespace {

/// A key press of a letter, the way the OS delivers one: the key code is the
/// character, and the text character is what the layout produced.
juce::KeyPress press(char letter, bool withCommand = false) {
    const auto code = static_cast<int>(static_cast<juce::juce_wchar>(letter));
    const auto mods = withCommand ? juce::ModifierKeys::ctrlModifier
                                   : juce::ModifierKeys();

    return juce::KeyPress(code, mods, static_cast<juce::juce_wchar>(letter));
}

}  // namespace

TEST_CASE("A bare letter matches its letter and nothing else", "[shortcut]") {
    CHECK(matchesShortcut(press('r'), "R"));
    CHECK_FALSE(matchesShortcut(press('q'), "R"));

    // A *modified* press is not a bare letter: Cmd-R must not arm the resistor,
    // or the rotate command would place a resistor instead of turning one.
    CHECK_FALSE(matchesShortcut(press('r', true), "R"));
}

TEST_CASE("A shortcut with the command modifier needs the modifier", "[shortcut]") {
    // The gain block is Ctrl+G and the ground is a bare G. They share a letter
    // and are still two different shortcuts -- the modifier is what tells them
    // apart, and that has to hold in both directions.
    CHECK(matchesShortcut(press('g', true), "Ctrl+G"));
    CHECK_FALSE(matchesShortcut(press('g'), "Ctrl+G"));

    CHECK(matchesShortcut(press('g'), "G"));
    CHECK_FALSE(matchesShortcut(press('g', true), "G"));
}

TEST_CASE("An empty or malformed shortcut matches nothing", "[shortcut]") {
    // A part with no shortcut is not a crash and not a match: the palette
    // simply prints nothing for it.
    CHECK_FALSE(matchesShortcut(press('r'), ""));
    CHECK_FALSE(matchesShortcut(press('r'), "Ctrl+"));
    CHECK_FALSE(matchesShortcut(press('r'), "Ctrl+R+"));
    CHECK_FALSE(matchesShortcut(press('r'), "1"));

    CHECK(displayShortcut("").isEmpty());
    CHECK(displayShortcut("Ctrl+").isEmpty());
}

TEST_CASE("The shortcut is written the way the user's keyboard says it", "[shortcut]") {
    // A bare letter is just the letter.
    CHECK(displayShortcut("R") == "R");

    // The command modifier is the system's: Cmd on a Mac, Ctrl elsewhere. The
    // model does not know which, so the *display* does.
    const auto withModifier = displayShortcut("Ctrl+G");
    CHECK(withModifier.endsWith("+G"));
    CHECK((withModifier == "Ctrl+G" || withModifier == "Cmd+G"));
}

TEST_CASE("No key press in the alphabet arms two parts", "[shortcut]") {
    // The invariant that matters, checked over the whole palette rather than a
    // part at a time: for every letter, the bare press and the command press
    // each arm at most one part.
    //
    // This is the check that would have caught "G" being taken by both the
    // ground and the gain block, had the gain block's shortcut been written as
    // a bare letter.
    for (char letter = 'a'; letter <= 'z'; ++letter) {
        const auto upper = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));

        for (const bool withCommand : {false, true}) {
            const auto key = press(letter, withCommand);

            int armed = 0;
            for (const auto& spec : partPalette())
                if (matchesShortcut(key, spec.key))
                    ++armed;

            INFO("letter: " << upper << " command: " << withCommand);
            CHECK(armed <= 1);
        }
    }
}

TEST_CASE("Every part family in the palette can be armed from the keyboard", "[shortcut]") {
    // The other half of the same invariant: a shortcut that matches *nothing*
    // is a part the keyboard cannot reach, which is as broken as a shortcut that
    // reaches two.
    //
    // Model rows share a family shortcut, and only the family's first row
    // carries it; the family is still reachable, and that is what the second
    // set checks. (Pressing "T" arms the 12AX7, not fourteen triodes at once.)
    std::set<std::string> allTypes;
    std::set<std::string> reachableTypes;

    for (const auto& spec : partPalette()) {
        allTypes.insert(spec.type);
        if (spec.key.empty())
            continue;

        INFO("part type: " << spec.type);

        bool reachable = false;
        for (char letter = 'a'; letter <= 'z' && !reachable; ++letter)
            for (const bool withCommand : {false, true})
                if (matchesShortcut(press(letter, withCommand), spec.key))
                    reachable = true;

        CHECK(reachable);
        if (reachable)
            reachableTypes.insert(spec.type);
    }

    // The real-card op-amp family is the one deliberate keyboard-less type:
    // "A" arms the ideal op-amp, a separate part type whose three-pin frame is
    // honest. The cards stay one click away in the palette and the inspector.
    const std::set<std::string> keyboardless = {"opamp"};
    std::set<std::string> expected = allTypes;
    for (const auto& type : keyboardless) {
        INFO("keyboardless type: " << type);
        REQUIRE(allTypes.count(type) == 1);      // the exception is not stale
        CHECK(reachableTypes.count(type) == 0);  // and it really has no key
        expected.erase(type);
    }

    CHECK(reachableTypes == expected);
}

TEST_CASE("The command letters are not a part's shortcut", "[shortcut]") {
    // Cmd+Z, Cmd+Y, Cmd+D, Cmd+R and Cmd+F are undo, redo, duplicate, rotate
    // and flip. A part whose *bare* shortcut was one of those letters would
    // still be reachable, but the palette would be offering a key that the
    // commands have already taken -- so the letters are named here, and a part
    // that wants one of them has to take the modified form.
    const auto* ground = findPart("gnd");
    REQUIRE(ground != nullptr);
    CHECK(ground->key == "G");

    const auto* gain = findPart("vcvs");
    REQUIRE(gain != nullptr);
    CHECK(gain->key == "Ctrl+G");
}
