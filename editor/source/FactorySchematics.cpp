#include "FactorySchematics.h"

#include "FactoryData.h"

#include <algorithm>

#ifndef LIVEWDF_FACTORY_SCHEMATICS_VERSION
#define LIVEWDF_FACTORY_SCHEMATICS_VERSION "unversioned"
#endif

namespace livewdf {

namespace {

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
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("LiveWDF")
        .getChildFile("Factory");
}

juce::Result FactorySchematics::ensureInstalled() {
    return ensureInstalledAt(folder());
}

juce::Array<juce::File> FactorySchematics::list() {
    juce::Array<juce::File> files;
    const auto root = folder();
    if (!root.isDirectory())
        return files;

    for (const auto& entry :
         juce::RangedDirectoryIterator(root, true, "*.livewdf", juce::File::findFiles))
        files.add(entry.getFile());

    std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b) {
        return a.getFullPathName().compareIgnoreCase(b.getFullPathName()) < 0;
    });
    return files;
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
