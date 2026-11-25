#pragma once

#include <cstddef>

namespace wdf::detail {

inline constexpr int transferPanelWidth = 8;
inline constexpr int smallDrivePanelWidth = transferPanelWidth / 2;

struct TransferPanel {
    int node;
    int colStart;
    int colCount;
    std::size_t valueStart;
};

struct DrivePanel {
    std::size_t rowStart;
    std::size_t valueStart;
    int colStart;
    int colCount;
    int width;
};

void assignDrivePanels(const DrivePanel*, std::size_t, const int*,
                       const double*, const int*, const double*, double*) noexcept;

struct RuntimeKernels {
    bool (*solve)(double*, double*, int);
    bool (*factorLU)(double*, int*, int);
    void (*solveLU)(const double*, const int*, double*, int);
    void (*subtractTransferPanels)(const TransferPanel*, std::size_t, const double*, const int*,
                            const double*, double*);
};

const RuntimeKernels& runtimeKernels() noexcept;

}  // namespace wdf::detail
