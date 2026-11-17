#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "SchematicConstants.h"
#include "Glow.h"
#include <vector>

namespace livewdf::glow {

struct CachedPath {
    juce::Path path;
    float length = 0.0f;
    float phase = 0.0f;
    float rangeMin = 0.0f;
    float rangeMax = 1.0f;
    std::vector<juce::Point<float>> samples;
    
    juce::Point<float> getPoint(float t) const;
    void rebuildCache();
    void updateCachedPath(float intensity);
};

class SignalPath {
public:
    SignalPath() = default;
    ~SignalPath() = default;

    std::vector<CachedPath>& getSignalPaths() noexcept { return signalPaths_; }
    const std::vector<CachedPath>& getSignalPaths() const noexcept { return signalPaths_; }

    // Cathodyne: polarity and direction are separate axes. `POLARITY_FORWARD =
    // false` is what `mode % 2 == 0` gives, so a Switch (mode 1) reverses the
    // bead *colour* order and a reversed path (mode >= 2) flips the *position*.
    float getRefVoltage() const noexcept { return refVoltage_; }
    float getDeltaVoltage() const noexcept { return deltaVoltage_; }

    /// The upstream path this one is DC-anchored to, or nullptr for a source.
    void setRef(SignalPath* ref) noexcept { refNode_ = ref; }
    void clearReference() noexcept { setRef(nullptr); }

    void addPath(const juce::Path& path, float rangeMin = 0.0f, float rangeMax = 1.0f);
    void clearPaths() { signalPaths_.clear(); }

    void setPolarity(bool pol) { polarity_ = pol; }
    void setDirection(bool dir) { direction_ = dir; }
    void setMode(int mode);

    void updateSignalPath(float intensity, float voltage, float power = 0.0f);
    void draw(juce::Graphics& g);

    void shufflePhase();

private:
    std::vector<CachedPath> signalPaths_;
    float refVoltage_ = 0.0f;
    float deltaVoltage_ = 0.0f;
    float power_ = 0.0f;
    bool polarity_ = false;   // POLARITY_FORWARD
    bool direction_ = false;  // DIRECTION_FORWARD
    SignalPath* refNode_ = nullptr;
};

} // namespace livewdf::glow
