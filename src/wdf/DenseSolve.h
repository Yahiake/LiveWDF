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

// Factor once, then solve for each right-hand side.
bool factorLUData(double* matrix, int* pivots, int size);
void solveLUData(const double* matrix, const int* pivots, double* rhs, int size);

}  // namespace wdf::detail
