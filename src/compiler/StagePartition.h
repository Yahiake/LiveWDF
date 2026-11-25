#pragma once

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "circuit/RootLayout.h"

namespace livewdf::compiler {

// A cross block of the loaded root impedance Z, in physical root-port order.
// Z[voltagePorts,currentPorts] = leftFactor * rightFactor within maxError
// ohms per coefficient. Both factors use row-major storage.
struct StageCoupling {
    std::vector<int> voltagePorts, currentPorts;
    int rank = 0;
    std::vector<double> leftFactor, rightFactor;
    double maxError = 0.0;
};

struct StagePartition {
    std::array<std::vector<int>, 2> devices;
    std::array<StageCoupling, 2> toStage;
    double sampleRate = 0.0;
    long long linearRevision = 0;
};

// Preparation only. Factors belong to this root layout and loaded impedance.
// Discard on a circuit change or transfer rebuild, including rate/control edits.
std::optional<StagePartition> discoverStagePartition(const wdf::RootLayout& layout,
                                                     std::span<const double> impedance,
                                                     double sampleRate, long long linearRevision);

}  // namespace livewdf::compiler
