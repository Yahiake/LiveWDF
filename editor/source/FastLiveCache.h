#pragma once

#include <juce_core/juce_core.h>
#include "wdf/Engine.h"

namespace livewdf {

/// Runs on the calibration worker. Disk entries retain the measured slope
/// masks. The engine rebuilds and certifies the corresponding matrices.
void buildCachedAffineSetup(wdf::Engine::MixedAffineSetup& left,
                            wdf::Engine::MixedAffineSetup* right,
                            const std::string& document,
                            const juce::File& modelFile,
                            bool gridCurrent);

} // namespace livewdf
