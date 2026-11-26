#pragma once

#include <cmath>
#include <vector>

namespace wdf::detail {

// In-place Gaussian elimination with partial pivoting. Sizes up to four use
// the unrolled solvers. The scalar case stays inline in the audio loop.
bool solveData(double* matrix, double* rhs, int size);

inline bool solve1(const double* matrix, double* rhs) {
    const double pivot = matrix[0];
    if (pivot == 0.0 || !std::isfinite(pivot))
        return false;
    rhs[0] /= pivot;
    return true;
}

inline bool solveSmall(std::vector<double>& matrix, std::vector<double>& rhs, int size) {
    return solveData(matrix.data(), rhs.data(), size);
}

inline bool solveDroppingZeroCorrections(double* matrix, double* rhs, int size) {
    constexpr int maxSize = 64;
    if (size > maxSize)
        return solveData(matrix, rhs, size);
    int active[maxSize], count = 0;
    for (int i = 0; i < size; ++i) {
        const double* row = matrix + i * size;
        bool zeroCorrection = rhs[i] == 0. && row[i] == 1.;
        if (zeroCorrection) {
            for (int j = 0; j < size; ++j) {
                if (j != i && row[j] != 0.) {
                    zeroCorrection = false;
                    break;
                }
            }
        }
        if (!zeroCorrection) active[count++] = i;
    }
    if (count + 2 >= size)
        return solveData(matrix, rhs, size);
    // Identity rows with zero RHS fix those corrections to zero. The sorted
    // indices let us compact in place and scatter the solution backwards.
    for (int i = 0; i < count; ++i) {
        const double* row = matrix + active[i] * size;
        for (int j = 0; j < count; ++j)
            matrix[i * count + j] = row[active[j]];
        rhs[i] = rhs[active[i]];
    }
    if (count && !solveData(matrix, rhs, count))
        return false;
    int j = count - 1;
    for (int i = size - 1; i >= 0; --i)
        rhs[i] = j >= 0 && active[j] == i ? rhs[j--] : 0.;
    return true;
}

// Factor once, then solve for each right-hand side.
bool factorLUData(double* matrix, int* pivots, int size);
void solveLUData(const double* matrix, const int* pivots, double* rhs, int size);

}  // namespace wdf::detail
