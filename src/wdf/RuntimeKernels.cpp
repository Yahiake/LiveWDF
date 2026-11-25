#include "wdf/RuntimeKernelsX86.h"
#include "wdf/DenseSolve.h"
#include "wdf/DenseSolveKernels.h"

#if defined(LIVEWDF_HAS_AVX2_KERNELS)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace wdf::detail {
namespace {

void applyRootPanels(const RootPanel* panels, std::size_t count, const double* values,
                     const int* columns, const double* currents, double* nodes) {
    for (std::size_t i = 0; i < count; ++i) {
        const auto& panel = panels[i];
        double sums[rootPanelWidth]{};
        const double* coeff = values + panel.valueStart;
        for (int j = 0; j < panel.colCount; ++j) {
            const double current = currents[columns[panel.colStart + j]];
            for (int q = 0; q < rootPanelWidth; ++q)
                sums[q] += coeff[q] * current;
            coeff += rootPanelWidth;
        }
        for (int q = 0; q < rootPanelWidth; ++q)
            nodes[panel.node + q] -= sums[q];
    }
}

constexpr RuntimeKernels baselineKernels{
    dense::solveData, dense::factorLUData, dense::solveLUData, applyRootPanels
};

#if defined(LIVEWDF_HAS_AVX2_KERNELS)
bool supportsAvx2Fma() noexcept {
    constexpr unsigned fma = 1u << 12;
    constexpr unsigned osxsave = 1u << 27;
    constexpr unsigned avx = 1u << 28;
    constexpr unsigned requiredExtensions = fma | osxsave | avx;
    constexpr unsigned xmmAndYmmState = 0x6;
    constexpr unsigned avx2 = 1u << 5;

#if defined(_MSC_VER)
    int registers[4];
    __cpuid(registers, 0);
    if (registers[0] < 7)
        return false;
    __cpuid(registers, 1);
    if ((registers[2] & requiredExtensions) != requiredExtensions ||
        (_xgetbv(0) & xmmAndYmmState) != xmmAndYmmState)
        return false;
    __cpuidex(registers, 7, 0);
    // /arch:AVX2 also permits BMI instructions in MSVC-generated code.
    constexpr unsigned requiredLeaf7 = avx2 | (1u << 3) | (1u << 8);
    return (registers[1] & requiredLeaf7) == requiredLeaf7;
#else
    unsigned eax, ebx, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) ||
        (ecx & requiredExtensions) != requiredExtensions)
        return false;
    // Only execute XGETBV after confirming the OS enabled XSAVE.
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    if ((eax & xmmAndYmmState) != xmmAndYmmState)
        return false;
    return __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) && (ebx & avx2);
#endif
}
#endif

}  // namespace

const RuntimeKernels& runtimeKernels() noexcept {
#if defined(LIVEWDF_HAS_AVX2_KERNELS)
    static const auto& selected = supportsAvx2Fma() ? avx2Kernels : baselineKernels;
    return selected;
#else
    return baselineKernels;
#endif
}

bool solveData(double* matrix, double* rhs, int size) {
    return runtimeKernels().solve(matrix, rhs, size);
}

bool factorLUData(double* matrix, int* pivots, int size) {
    return runtimeKernels().factorLU(matrix, pivots, size);
}

void solveLUData(const double* matrix, const int* pivots, double* rhs, int size) {
    runtimeKernels().solveLU(matrix, pivots, rhs, size);
}

}  // namespace wdf::detail
