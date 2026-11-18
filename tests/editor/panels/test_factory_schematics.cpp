// The factory schematics are shipped inside both binaries. These tests exercise
// the shared installer directly, so a change to the archive layout or the
// version marker cannot silently leave the factory folder half-written.

#include <catch2.hpp>

#include "FactorySchematics.h"
#include "PresetLibraryPanel.h"

using livewdf::FactorySchematics;
using livewdf::editor::PresetLibraryPanel;

namespace {

struct TempFolder {
    juce::File root;

    TempFolder() {
        root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("livewdf_factory_" +
                                 juce::Uuid().toString().replace("-", ""));
        root.createDirectory();
    }

    ~TempFolder() { root.deleteRecursively(); }
};

juce::File firstLivewdf(const juce::File& root) {
    for (const auto& entry :
         juce::RangedDirectoryIterator(root, true, "*.livewdf", juce::File::findFiles))
        return entry.getFile();
    return {};
}

int rowWithText(const PresetLibraryPanel& panel, const juce::String& text) {
    for (int i = 0; i < panel.getRowCount(); ++i)
        if (panel.getRowText(i) == text)
            return i;
    return -1;
}

}  // namespace

TEST_CASE("Factory schematics install into a folder and are idempotent") {
    TempFolder temp;
    const auto factoryFolder = temp.root.getChildFile("Factory");
    REQUIRE(factoryFolder.createDirectory().wasOk());
    const auto firstRunFile = factoryFolder.getChildFile("Already Here.livewdf");
    REQUIRE(firstRunFile.replaceWithText("{}"));

    const auto installed = FactorySchematics::ensureInstalledAt(factoryFolder);
    INFO(installed.getErrorMessage().toStdString());
    REQUIRE(installed.wasOk());
    CHECK(firstRunFile.existsAsFile());

    CHECK(factoryFolder.getChildFile("amps").isDirectory());
    CHECK(factoryFolder.getChildFile("pedals_fx").isDirectory());
    CHECK(factoryFolder.getChildFile(".factory-version").existsAsFile());

    const auto userPreset = temp.root.getChildFile("User Preset.livewdf");
    REQUIRE(userPreset.replaceWithText("{}"));

    const auto sample = firstLivewdf(factoryFolder.getChildFile("amps"));
    REQUIRE(sample.existsAsFile());
    const auto before = sample.getLastModificationTime();

    // A matching marker and intact files must not touch anything: factory
    // files are user-visible, and rewriting them on every launch would make an
    // edited copy look like it had been saved when it had not.
    REQUIRE(FactorySchematics::ensureInstalledAt(factoryFolder).wasOk());
    CHECK(sample.getLastModificationTime() == before);
    CHECK(userPreset.existsAsFile());

    // A missing factory file is repaired even when the marker matches.
    REQUIRE(sample.deleteFile());
    REQUIRE(FactorySchematics::ensureInstalledAt(factoryFolder).wasOk());
    CHECK(sample.existsAsFile());
    CHECK(userPreset.existsAsFile());

    // A stale marker refreshes the factory set without deleting user presets
    // beside the Factory folder.
    const auto marker = factoryFolder.getChildFile(".factory-version");
    REQUIRE(marker.replaceWithText("stale"));
    const auto stale = factoryFolder.getChildFile("amps").getChildFile("Old Factory.livewdf");
    REQUIRE(stale.replaceWithText("{}"));
    REQUIRE(FactorySchematics::ensureInstalledAt(factoryFolder).wasOk());
    CHECK(marker.loadFileAsString().trim() != "stale");
    CHECK(userPreset.existsAsFile());
    CHECK(!stale.existsAsFile());
}

TEST_CASE("Factory circuits get their own preset-library section") {
    TempFolder temp;

    const auto factoryAmp = temp.root.getChildFile("Factory").getChildFile("amps");
    REQUIRE(factoryAmp.createDirectory().wasOk());
    REQUIRE(factoryAmp.getChildFile("Factory Amp.livewdf").replaceWithText("{}"));
    REQUIRE(temp.root.getChildFile("User Preset.livewdf").replaceWithText("{}"));

    PresetLibraryPanel panel;
    panel.setFolder(temp.root);

    CHECK(rowWithText(panel, "FACTORY") >= 0);
    CHECK(rowWithText(panel, "Factory Amp") >= 0);
    CHECK(rowWithText(panel, "User Preset") >= 0);
}
