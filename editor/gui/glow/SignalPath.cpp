#include "SignalPath.h"
#include <cmath>
#include <algorithm>

namespace livewdf::glow {

juce::Point<float> CachedPath::getPoint(float t) const {
    if (samples.size() < 2)
        return {};

    t = std::clamp(t, 0.0f, 1.0f);
    const float pos = t * static_cast<float>(samples.size() - 1);
    const int i0 = static_cast<int>(pos);
    const int i1 = std::min(i0 + 1, static_cast<int>(samples.size() - 1));
    const float frac = pos - static_cast<float>(i0);

    return samples[static_cast<size_t>(i0)] + (samples[static_cast<size_t>(i1)] - samples[static_cast<size_t>(i0)]) * frac;
}

void CachedPath::rebuildCache() {
    length = path.getLength();
    const int numSamples = std::max(2, static_cast<int>(std::ceil(length / SIGNALPATH_SAMPLE_SPACING)));
    samples.clear();
    samples.reserve(static_cast<size_t>(numSamples));

    for (int i = 0; i < numSamples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(numSamples - 1);
        samples.push_back(path.getPointAlongPath(t * length));
    }
}

void CachedPath::updateCachedPath(float intensity) {
    if (length < 1.0f) return;
    // Signed, so a negative intensity flows the beads backwards for free.
    intensity = std::clamp(intensity, -1.0f, 1.0f);
    phase += intensity * SIGNALPATH_SPEED / length;
    phase = std::fmod(phase, 1.0f);
}

void SignalPath::addPath(const juce::Path& p, float rangeMin, float rangeMax) {
    CachedPath cp{ p };
    cp.rangeMin = std::clamp(rangeMin, 0.0f, 1.0f);
    cp.rangeMax = std::clamp(rangeMax, 0.0f, 1.0f);
    cp.rebuildCache();
    signalPaths_.push_back(std::move(cp));
}

void SignalPath::setMode(int mode) {
    // Cathodyne: POLARITY_FORWARD = false is `mode % 2 == 0`; DIRECTION_FORWARD
    // = false is `mode < 2`.
    setPolarity((mode % 2) != 0 ? true : false);
    setDirection(mode >= 2);
}

void SignalPath::shufflePhase() {
    const size_t n = signalPaths_.size();
    for (size_t i = 0; i < n; ++i) {
        signalPaths_[i].phase = static_cast<float>(i) / static_cast<float>(n);
    }
}

void SignalPath::updateSignalPath(float intensity, float voltage, float power) {
    power_ = power > 1.0f ? 1.0f : power;

    // Reference chaining: a path inherits its DC reference from the path that
    // feeds it, so a wire is anchored to what is upstream of it rather than to
    // its own voltage alone.
    if (refNode_ != nullptr) {
        const float ref = refNode_->getRefVoltage();
        const bool dir = refNode_->direction_;
        const float delta = refNode_->getDeltaVoltage();

        refVoltage_ = ref - (dir ? -1.0f : 1.0f) * delta;
        deltaVoltage_ = voltage;
    } else {
        refVoltage_ = voltage;
        deltaVoltage_ = 0.0f;
    }

    for (auto& cp : signalPaths_) {
        cp.updateCachedPath(intensity);
    }
}

void SignalPath::draw(juce::Graphics& g) {
    if (signalPaths_.empty()) return;

    // ===== Cathodyne, exact =====
    const float deltat = deltaVoltage_ / SIGNAL_PATH_MAX_VOLTAGE;
    const float reft = refVoltage_ / SIGNAL_PATH_MAX_VOLTAGE;
    const float beadSize = SIGNALPATH_BEAD_SIZE;
    const float glow = 0.1f + power_ * 0.4f;

    for (const auto& sigPath : signalPaths_) {
        if (sigPath.samples.size() < 2)
            return;

        const int numBeads = std::max(1, static_cast<int>(sigPath.length / SIGNALPATH_BEAD_SPACING));
        const float spacing = 1.0f / static_cast<float>(numBeads);
        const float rangeMin = sigPath.rangeMin;
        const float rangeMax = sigPath.rangeMax;
        const float tRange = rangeMax - rangeMin;

        for (int i = 0; i < numBeads; ++i) {
            const float offset = static_cast<float>(i) * spacing;
            float t = offset + sigPath.phase;
            t -= std::floor(t);
            // Position: direction flips it.
            t = direction_ ? t : 1.0f - t;

            const juce::Point<float> p = sigPath.getPoint(t);

            juce::Path bead;
            bead.addEllipse(p.x - beadSize * 0.5f, p.y - beadSize * 0.5f, beadSize, beadSize);

            // Colour: polarity flips `t` a *second*, independent time, then the
            // local voltage along the wire picks the colour.
            t = polarity_ ? t : 1.0f - t;
            const float colort = reft - (direction_ ? -1.0f : 1.0f) * deltat * (t * tRange + rangeMin);

            const auto color = getVoltageColourGradient(colort);
            drawGlowPath(g, bead, color, glow);
            drawCorePath(g, bead, color, false);
        }
    }
}

} // namespace livewdf::glow
