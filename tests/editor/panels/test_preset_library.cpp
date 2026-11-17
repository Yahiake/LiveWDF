// The preset library lists files and opens them through the host.
//
// It is deliberately a dumb view: these tests check the three promises that
// matter to the editor -- a folder is scanned recursively, a search narrows the
// list without touching the files, and choosing a row reports the file rather
// than opening it itself.

#include <catch2.hpp>

#include "PresetLibraryPanel.h"

using livewdf::editor::PresetLibraryPanel;

namespace {

int rowWithText(const PresetLibraryPanel& panel, const juce::String& text) {
    for (int i = 0; i < panel.getRowCount(); ++i)
        if (panel.getRowText(i) == text)
            return i;
    return -1;
}

struct TempPresetFolder {
    juce::File root;

    TempPresetFolder() {
        root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("livewdf_presets_" +
                                 juce::Uuid().toString().replace("-", ""));
        root.createDirectory();
    }

    ~TempPresetFolder() { root.deleteRecursively(); }
};

}  // namespace

TEST_CASE("The preset library scans a folder recursively and reports a chosen file") {
    TempPresetFolder temp;

    const auto fuzz = temp.root.getChildFile("Fuzz Face.livewdf");
    REQUIRE(fuzz.replaceWithText("{}"));

    const auto sub = temp.root.getChildFile("Sub");
    REQUIRE(sub.createDirectory().wasOk());
    const auto bass = sub.getChildFile("Bass 2.livewdf");
    REQUIRE(bass.replaceWithText("{}"));

    PresetLibraryPanel panel;
    panel.setFolder(temp.root);

    CHECK(panel.getRowCount() > 0);
    CHECK(rowWithText(panel, "New empty sheet") >= 0);
    CHECK(rowWithText(panel, "Starter: RC lowpass") >= 0);
    CHECK(rowWithText(panel, "Fuzz Face") >= 0);
    CHECK(rowWithText(panel, "Bass 2") >= 0);

    const int fuzzRow = rowWithText(panel, "Fuzz Face");
    REQUIRE(fuzzRow >= 0);
    CHECK(panel.isRowFile(fuzzRow));
    CHECK(panel.getRowFile(fuzzRow) == fuzz);

    juce::File chosen;
    panel.onPresetChosen = [&chosen](const juce::File& file) { chosen = file; };
    panel.chooseRow(fuzzRow);
    CHECK(chosen == fuzz);

    panel.setCurrentFile(fuzz);
    CHECK(panel.isRowCurrent(fuzzRow));
}

TEST_CASE("Search narrows the library without changing its rows") {
    TempPresetFolder temp;

    const auto fuzz = temp.root.getChildFile("Fuzz Face.livewdf");
    REQUIRE(fuzz.replaceWithText("{}"));
    const auto bass = temp.root.getChildFile("Bass 2.livewdf");
    REQUIRE(bass.replaceWithText("{}"));

    PresetLibraryPanel panel;
    panel.setFolder(temp.root);

    panel.setSearchText("bass");
    CHECK(panel.getRowCount() == 1);
    CHECK(panel.getRowText(0) == "Bass 2");
    CHECK(panel.getRowFile(0) == bass);

    panel.setSearchText("fuzz");
    CHECK(panel.getRowCount() == 1);
    CHECK(panel.getRowText(0) == "Fuzz Face");

    panel.setSearchText("no-such-preset");
    CHECK(panel.getRowCount() == 1);
    CHECK(panel.getRowText(0).contains("No presets match"));

    panel.setSearchText({});
    CHECK(panel.getRowCount() > 1);
    CHECK(rowWithText(panel, "Fuzz Face") >= 0);
    CHECK(rowWithText(panel, "Bass 2") >= 0);
}

TEST_CASE("An unchosen folder still offers the built-in rows and a way out") {
    PresetLibraryPanel panel;
    panel.setFolder({});

    CHECK(rowWithText(panel, "New empty sheet") >= 0);
    CHECK(rowWithText(panel, "Starter: RC lowpass") >= 0);
    CHECK(rowWithText(panel, "Choose preset folder...") >= 0);
    CHECK(rowWithText(panel, "Save current as preset...") >= 0);

    bool newSheet = false;
    bool starter = false;
    bool chooseFolder = false;

    panel.onNewSheet = [&newSheet] { newSheet = true; };
    panel.onStarterChosen = [&starter] { starter = true; };
    panel.onChooseFolder = [&chooseFolder] { chooseFolder = true; };

    panel.chooseRow(rowWithText(panel, "New empty sheet"));
    panel.chooseRow(rowWithText(panel, "Starter: RC lowpass"));
    panel.chooseRow(rowWithText(panel, "Choose preset folder..."));

    CHECK(newSheet);
    CHECK(starter);
    CHECK(chooseFolder);
}
