#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace livewdf {

/// The factory circuits that ship with the editor and the plugin.
///
/// The repository keeps them as ordinary `.livewdf` files under `schematics/`.
/// An installed binary cannot see that checkout, so the build embeds a zip of
/// the folder and this helper writes it into the user's Documents directory on
/// first use (or when the factory contents change).
///
/// The same folder is shared by both applications: whichever one runs first
/// installs the files, and the other one sees them. User presets live beside
/// `Factory/`, not inside it.
class FactorySchematics {
public:
    enum class Collection { Amps, Pedals, Studio, Other };

    struct Preset {
        juce::File file;
        Collection collection;
        juce::String effect;
        bool featured;
    };

    /// `Documents/LiveWDF/Factory`.
    static juce::File folder();

    /// Create the folder and unpack the embedded archive if it is missing or
    /// stale. Safe to call on every launch.
    static juce::Result ensureInstalled();

    /// Install into a caller-supplied folder instead of Documents. This is the
    /// same operation `ensureInstalled()` performs and exists so tests (and a
    /// future command-line installer) do not have to write to the user's
    /// Documents folder.
    static juce::Result ensureInstalledAt(const juce::File& targetFolder);

    /// Classify and sort the installed catalog without reading the circuits.
    static std::vector<Preset> list(const juce::File& root, bool showPreviewPresets = false);
    static juce::String collectionName(Collection collection);
};

} // namespace livewdf
