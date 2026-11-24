#pragma once

#include "ModalPreview.h"
#include "NonlinearPreview.h"
#include <juce_core/juce_core.h>
#include <memory>
#include <string>
#include <variant>

namespace livewdf {

struct PreviewPlayback {
    using Model = std::variant<wdf::preview::ModalPreview, wdf::preview::NonlinearPreview>;
    std::array<Model, 2> channels;
    double peak = 0.0, bias = 0.0;
    bool experimental() const noexcept { return channels[0].index() == 1; }
};

// Runs on a worker. The input string is the engine's canonical document, including controls.
std::unique_ptr<PreviewPlayback> loadPreviewPlayback(const std::string& document,
                                                      const juce::File& circuit, double sampleRate);

}
