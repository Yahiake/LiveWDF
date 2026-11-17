#include "Fonts.h"

#include <FontData.h>
#include <array>

namespace livewdf::editor::Fonts {

namespace {

juce::Typeface::Ptr loadResource(const char* resourceName) {
    int size = 0;
    if (const auto* data = FontData::getNamedResource(resourceName, size);
        data != nullptr && size > 0)
    {
        return juce::Typeface::createSystemTypefaceFor(data, static_cast<size_t>(size));
    }
    return {};
}

struct FontCache final : public juce::DeletedAtShutdown {
    static FontCache& get() {
        if (instance == nullptr)
            instance = new FontCache();
        return *instance;
    }

    ~FontCache() override { instance = nullptr; }

    // Slots:
    // 0: Inter Regular
    // 1: Inter Medium
    // 2: Inter SemiBold
    // 3: Inter Bold
    // 4: Mono Regular
    // 5: Mono SemiBold
    std::array<juce::Typeface::Ptr, 6> faces {};
    std::array<bool, 6> tried { false, false, false, false, false, false };

    static FontCache* instance;
};

FontCache* FontCache::instance = nullptr;

}  // namespace

juce::Typeface::Ptr typeface(Family family, Weight weight) {
    auto& cache = FontCache::get();
    size_t slot = 1;

    if (family == Family::UI) {
        switch (weight) {
            case Weight::Light:
            case Weight::Regular:
                slot = 0;
                break;
            case Weight::Medium:
                slot = 1;
                break;
            case Weight::SemiBold:
                slot = 2;
                break;
            case Weight::Bold:
                slot = 3;
                break;
        }
    } else { // Mono
        switch (weight) {
            case Weight::Light:
            case Weight::Regular:
                slot = 4;
                break;
            case Weight::Medium:
            case Weight::SemiBold:
            case Weight::Bold:
                slot = 5;
                break;
        }
    }

    if (!cache.tried[slot]) {
        cache.tried[slot] = true;
        switch (slot) {
            case 0: cache.faces[0] = loadResource("InterRegular_ttf"); break;
            case 1: cache.faces[1] = loadResource("InterMedium_ttf"); break;
            case 2: cache.faces[2] = loadResource("InterSemiBold_ttf"); break;
            case 3: cache.faces[3] = loadResource("InterBold_ttf"); break;
            case 4: cache.faces[4] = loadResource("IBMPlexMonoRegular_ttf"); break;
            case 5: cache.faces[5] = loadResource("IBMPlexMonoSemiBold_ttf"); break;
        }

        // Fallback to whatever is available if loading failed
        if (cache.faces[slot] == nullptr) {
            if (slot >= 4) {
                cache.faces[slot] = loadResource("JuraLight_ttf");
            } else {
                cache.faces[slot] = loadResource("InterMedium_ttf");
                if (cache.faces[slot] == nullptr)
                    cache.faces[slot] = loadResource("JuraLight_ttf");
            }
        }
    }

    return cache.faces[slot];
}

juce::Typeface::Ptr typeface(Family family, bool bold) {
    return typeface(family, bold ? Weight::Bold : Weight::Medium);
}

juce::Typeface::Ptr typeface(Weight weight) {
    return typeface(Family::UI, weight);
}

juce::Font font(Family family, float heightInPixels, Weight weight) {
    if (auto face = typeface(family, weight)) {
        return juce::Font(juce::FontOptions(face).withHeight(heightInPixels));
    }

    auto opts = juce::FontOptions(heightInPixels);
    if (weight == Weight::Bold || weight == Weight::SemiBold) {
        opts = opts.withStyle("Bold");
    }
    return juce::Font(opts);
}

juce::Font font(Family family, float heightInPixels, bool bold) {
    return font(family, heightInPixels, bold ? Weight::Bold : Weight::Medium);
}

juce::Font font(Weight weight, float heightInPixels) {
    return font(Family::UI, heightInPixels, weight);
}

void installDefaultTypeface() {
    juce::LookAndFeel::getDefaultLookAndFeel().setDefaultSansSerifTypeface(typeface(Family::UI, Weight::Medium));
}

}  // namespace livewdf::editor::Fonts
