#include "wdf/RuntimeKernelsX86.h"
#include "wdf/DenseSolve.h"
#include "wdf/DenseSolveKernels.h"

#include <algorithm>

#if defined(LIVEWDF_HAS_AVX2_KERNELS)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace wdf::detail {
namespace {

template <int Width>
void accumulatePanel(const double* values, const int* columns, int count,
                     const double* input, double (&sums)[Width]) noexcept {
    for (int j = 0; j < count; ++j) {
        const double value = input[columns[j]];
        for (int lane = 0; lane < Width; ++lane)
            sums[lane] += values[lane] * value;
        values += Width;
    }
}

void subtractTransferPanels(const TransferPanel* panels, std::size_t count, const double* values,
                            const int* columns, const double* input, double* nodes) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        const auto& panel = panels[i];
        double sums[transferPanelWidth]{};
        accumulatePanel(values + panel.valueStart, columns + panel.colStart, panel.colCount, input, sums);
        for (int lane = 0; lane < transferPanelWidth; ++lane)
            nodes[panel.node + lane] -= sums[lane];
    }
}

template <int Width>
void assignDrivePanel(const DrivePanel& panel, const int* rows, const double* values,
                      const int* columns, const double* input, double* nodes) noexcept {
    double sums[Width]{};
    accumulatePanel(values + panel.valueStart, columns + panel.colStart, panel.colCount, input, sums);
    rows += panel.rowStart;
    if (rows[Width - 1] == rows[0] + Width - 1)
        std::copy_n(sums, Width, nodes + rows[0]);
    else
        for (int lane = 0; lane < Width; ++lane)
            nodes[rows[lane]] = sums[lane];
}

constexpr RuntimeKernels baselineKernels{
    dense::solveData, dense::factorLUData, dense::solveLUData, subtractTransferPanels
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

void assignDrivePanels(const DrivePanel* panels, std::size_t count, const int* rows,
                       const double* values, const int* columns, const double* input, double* nodes) noexcept {
    // Use the same baseline target and per-node accumulation order as the sparse drive.
    for (std::size_t i = 0; i < count; ++i) {
        const auto& panel = panels[i];
        if (panel.width == transferPanelWidth)
            assignDrivePanel<transferPanelWidth>(panel, rows, values, columns, input, nodes);
        else
            assignDrivePanel<smallDrivePanelWidth>(panel, rows, values, columns, input, nodes);
    }
}

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
