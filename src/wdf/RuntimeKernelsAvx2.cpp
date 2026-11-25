#include "wdf/RuntimeKernelsX86.h"

#if defined(LIVEWDF_HAS_AVX2_KERNELS)
#include <immintrin.h>

#include "wdf/DenseSolveKernels.h"

namespace wdf::detail {
namespace {

void subtractTransferPanels(const TransferPanel* panels, std::size_t count, const double* values,
                     const int* columns, const double* currents, double* nodes) {
    static_assert(transferPanelWidth == 8);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& panel = panels[i];
        auto lo = _mm256_setzero_pd();
        auto hi = _mm256_setzero_pd();
        const double* coeff = values + panel.valueStart;
        for (int j = 0; j < panel.colCount; ++j) {
            const auto current = _mm256_set1_pd(currents[columns[panel.colStart + j]]);
            lo = _mm256_fmadd_pd(_mm256_loadu_pd(coeff), current, lo);
            hi = _mm256_fmadd_pd(_mm256_loadu_pd(coeff + 4), current, hi);
            coeff += transferPanelWidth;
        }
        double* output = nodes + panel.node;
        _mm256_storeu_pd(output, _mm256_sub_pd(_mm256_loadu_pd(output), lo));
        _mm256_storeu_pd(output + 4, _mm256_sub_pd(_mm256_loadu_pd(output + 4), hi));
    }
}

}  // namespace

constinit const RuntimeKernels avx2Kernels{
    dense::solveData, dense::factorLUData, dense::solveLUData, subtractTransferPanels
};

}  // namespace wdf::detail
#endif
