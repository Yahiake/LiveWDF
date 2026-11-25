#pragma once

#include <cstddef>

namespace wdf::detail {

inline constexpr int rootPanelWidth = 8;

struct RootPanel {
    int node;
    int colStart;
    int colCount;
    std::size_t valueStart;
};

struct RuntimeKernels {
    bool (*solve)(double*, double*, int);
    bool (*factorLU)(double*, int*, int);
    void (*solveLU)(const double*, const int*, double*, int);
    void (*applyRootPanels)(const RootPanel*, std::size_t, const double*, const int*,
                            const double*, double*);
};

const RuntimeKernels& runtimeKernels() noexcept;

}  // namespace wdf::detail
