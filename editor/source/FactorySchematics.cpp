#include "FactorySchematics.h"

#include "FactoryData.h"

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <string_view>

#ifndef LIVEWDF_FACTORY_SCHEMATICS_VERSION
#define LIVEWDF_FACTORY_SCHEMATICS_VERSION "unversioned"
#endif

namespace livewdf {

namespace {

// Older pedal files predate effect subfolders. Keep their installed paths
// stable for linked documents and classify them by their existing names.
const char* pedalEffect(std::string_view name) {
    struct Category {
        const char* label;
        std::initializer_list<std::string_view> names;
    };
    static const Category categories[] = {
        {"Drive & Fuzz", {"Colorsound Power Boost 18V", "MXR Micro Amp",
                          "Boss BD-2 Blues Driver", "Boss SD-1 Super OverDrive", "DOD 250 Overdrive Preamp Gray",
                          "Ibanez TS_808", "Klon Centaur", "Marshall Bluesbreaker Mk I",
                          "Nobels ODR-1 Natural Overdrive", "Boss DS-1A Distortion",
                          "Boss DS-2 Turbo Distortion", "Boss HM-2 Heavy Metal",
                          "Boss MT-2 Metal Zone", "Marshall Guv'nor", "Pro Co RAT LM308",
                          "EHX Big Muff Pi Ram's Head Violet", "Fuzz Face", "Maestro FZ-1 Fuzz-Tone 1962",
                          "Mosrite Fuzzrite Silicon", "Roger Mayer Octavia Transistor",
                          "Sola Sound Tone Bender Professional Mk II", "ZVEX Fuzz Factory"}},
        {"Delay", {"Boss DM-2 Analog Delay", "EHX Deluxe Memory Man", "MXR Carbon Copy Analog Delay"}},
        {"Modulation", {"Boss CE-2 Chorus", "EHX Small Clone MN3007", "MXR M234 Analog Chorus",
                        "Boss BF-2 Flanger", "EHX Electric Mistress 9V", "EHX Small Stone Issue J",
                        "MXR Phase 90 Script", "Shin-ei Univox Uni-Vibe 915", "Boss TR-2 Tremolo"}},
        {"EQ & Filters", {"Boss GE-7 Equalizer", "Boss PQ-4 Parametric EQ",
                          "MXR M108S Ten Band EQ (Rev E Reference)", "Dunlop Cry Baby GCB-95",
                          "Musitronics Mu-Tron III"}},
        {"Dynamics", {"Boss CS-3 Compression Sustainer (August 1986)", "Dan Armstrong Orange Squeezer",
                      "Diamond CPR-1 (Quartz Reference)", "MXR Dyna Comp CA3080",
                      "Origin Effects Cali76 FET Compressor (Circuit Approximation)",
                      "Boss NS-2 Noise Suppressor", "ISP Decimator II G String (Noise Terminator Reference)"}},
    };
    for (const auto& category : categories)
        if (std::find(category.names.begin(), category.names.end(), name) != category.names.end())
            return category.label;
    return "";
}

bool isFeatured(std::string_view name, FactorySchematics::Collection collection) {
    static constexpr std::string_view pedals[] = {
        "Boss CE-2 Chorus", "Boss DM-2 Analog Delay", "Boss GE-7 Equalizer", "Dunlop Cry Baby GCB-95",
        "Ibanez TS_808", "MXR Dyna Comp CA3080", "MXR Phase 90 Script", "Pro Co RAT LM308"
    };
    static constexpr std::string_view studio[] = {
        "API 312 Preamp VP312 Rev A", "Pultec EQP-1A Program Equalizer",
        "Teletronix-LA-2A", "UREI-1176LN-Rev-D"
    };
    if (collection == FactorySchematics::Collection::Pedals)
        return std::find(std::begin(pedals), std::end(pedals), name) != std::end(pedals);
    if (collection == FactorySchematics::Collection::Studio)
        return std::find(std::begin(studio), std::end(studio), name) != std::end(studio);
    return false;
}

FactorySchematics::Preset describe(const juce::File& file, const juce::String& relative) {
    using Collection = FactorySchematics::Collection;
    const auto parts = juce::StringArray::fromTokens(relative, "/", {});
    const auto collection = parts[0].toLowerCase();
    const auto effect = parts.size() > 2 ? parts[1] : juce::String();
    const auto name = file.getFileNameWithoutExtension();
    auto group = Collection::Other;
    juce::String label;

    if (collection == "pedals_fx") {
        group = Collection::Pedals;
        label = effect.isEmpty() ? juce::String(pedalEffect(name.toRawUTF8())) : effect;
    } else if (collection == "studio") {
        group = Collection::Studio;
        if (effect.equalsIgnoreCase("eq"))
            label = "EQ";
        else if (effect.equalsIgnoreCase("enhancement"))
            label = "Saturation & Enhancement";
        else
            label = effect.substring(0, 1).toUpperCase() + effect.substring(1);
    } else if (collection == "amps") {
        group = Collection::Amps;
        label = effect;
    }

    return {file, group, label, isFeatured(name.toRawUTF8(), group)};
}

const char* findFactoryArchive(int& size) {
    for (int i = 0; i < FactoryData::namedResourceListSize; ++i) {
        const char* name = FactoryData::namedResourceList[i];
        const char* original = FactoryData::getNamedResourceOriginalFilename(name);
        if (original == nullptr || !juce::String(original).endsWithIgnoreCase(".zip"))
            continue;

        int bytes = 0;
        const char* data = FactoryData::getNamedResource(name, bytes);
        if (data != nullptr && bytes > 0) {
            size = bytes;
            return data;
        }
    }

    return nullptr;
}

} // anonymous

juce::File FactorySchematics::folder() {
#if JUCE_ANDROID
    const auto location = juce::File::userApplicationDataDirectory;
#else
    const auto location = juce::File::userDocumentsDirectory;
#endif
    return juce::File::getSpecialLocation(location)
        .getChildFile("LiveWDF")
        .getChildFile("Factory");
}

juce::Result FactorySchematics::ensureInstalled() {
    return ensureInstalledAt(folder());
}

std::vector<FactorySchematics::Preset> FactorySchematics::list(const juce::File& root) {
    std::vector<Preset> files;
    if (!root.isDirectory())
        return files;

    juce::StringArray hiddenPaths;
    hiddenPaths.addLines(root.getChildFile(".factory-hidden").loadFileAsString());
    hiddenPaths.trim();

    for (const auto& entry :
         juce::RangedDirectoryIterator(root, true, "*.livewdf", juce::File::findFiles)) {
        const auto file = entry.getFile();
        if (file.getFileName().startsWithChar('.'))
            continue;
        const auto relative = file.getRelativePathFrom(root).replaceCharacter('\\', '/');
        if (!hiddenPaths.contains(relative))
            files.push_back(describe(file, relative));
    }

    std::sort(files.begin(), files.end(), [](const Preset& a, const Preset& b) {
        if (a.collection != b.collection)
            return a.collection < b.collection;
        if (const auto order = a.effect.compareNatural(b.effect); order != 0)
            return order < 0;
        if (const auto order = a.file.getFileName().compareNatural(b.file.getFileName()); order != 0)
            return order < 0;
        return a.file.getFullPathName() < b.file.getFullPathName();
    });
    return files;
}

juce::String FactorySchematics::collectionName(Collection collection) {
    switch (collection) {
        case Collection::Pedals: return "Pedals";
        case Collection::Studio: return "Studio";
        case Collection::Amps: return "Amps";
        case Collection::Other: return "Other factory";
    }
    return {};
}

juce::Result FactorySchematics::ensureInstalledAt(const juce::File& targetFolder) {
    const auto create = targetFolder.createDirectory();
    if (create.failed())
        return create;

    int archiveSize = 0;
    const char* archive = findFactoryArchive(archiveSize);
    if (archive == nullptr)
        return juce::Result::fail("Factory schematics are missing from this build.");

    juce::MemoryInputStream stream(archive, static_cast<size_t>(archiveSize), false);
    juce::ZipFile zip(stream);
    if (zip.getNumEntries() <= 0)
        return juce::Result::fail("The embedded factory schematic archive is empty.");

    const juce::String version(LIVEWDF_FACTORY_SCHEMATICS_VERSION);
    const auto marker = targetFolder.getChildFile(".factory-version");
    const bool markerExists = marker.existsAsFile();
    const bool versionMatches =
        markerExists && marker.loadFileAsString().trim() == version;

    if (versionMatches) {
        bool complete = true;
        for (int i = 0; i < zip.getNumEntries(); ++i) {
            const auto* entry = zip.getEntry(i);
            if (entry == nullptr || entry->filename.endsWithChar('/'))
                continue;

            const auto file = targetFolder.getChildFile(entry->filename);
            if (!file.existsAsFile() || file.getSize() != entry->uncompressedSize) {
                complete = false;
                break;
            }
        }

        if (complete)
            return juce::Result::ok();
    } else if (markerExists) {
        // A new archive may have renamed or removed a factory circuit. Remove
        // the old `.livewdf` files first so the folder mirrors the new archive
        // instead of accumulating stale entries. On a true first run there is
        // no marker, and an existing folder may contain files the user put
        // there; leave those alone.
        for (const auto& entry :
             juce::RangedDirectoryIterator(targetFolder, true, "*.livewdf",
                                           juce::File::findFiles))
            entry.getFile().deleteFile();
    }

    if (const auto unpack = zip.uncompressTo(targetFolder, true); unpack.failed())
        return unpack;

    if (!marker.replaceWithText(version))
        return juce::Result::fail("Could not write the factory schematic version marker.");

    return juce::Result::ok();
}

} // namespace livewdf
