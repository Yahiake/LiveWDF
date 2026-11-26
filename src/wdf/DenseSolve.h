#pragma once

#include <algorithm>
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

inline constexpr int maxCompactSolveSize = 64;

template <typename Solve>
inline bool solveDroppingZeroCorrections(double* matrix, double* rhs, int size, Solve solve) {
    if (size > maxCompactSolveSize)
        return solve(matrix, rhs, size, nullptr);
    int active[maxCompactSolveSize], count = 0;
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
        return solve(matrix, rhs, size, nullptr);
    // Identity rows with zero RHS fix those corrections to zero. The sorted
    // indices let us compact in place and scatter the solution backwards.
    for (int i = 0; i < count; ++i) {
        const double* row = matrix + active[i] * size;
        for (int j = 0; j < count; ++j)
            matrix[i * count + j] = row[active[j]];
        rhs[i] = rhs[active[i]];
    }
    if (!solve(matrix, rhs, count, active))
        return false;
    int j = count - 1;
    for (int i = size - 1; i >= 0; --i)
        rhs[i] = j >= 0 && active[j] == i ? rhs[j--] : 0.;
    return true;
}

inline bool solveDroppingZeroCorrections(double* matrix, double* rhs, int size) {
    return solveDroppingZeroCorrections(matrix, rhs, size,
        [](double* a, double* b, int n, const int*) { return n == 0 || solveData(a, b, n); });
}

// Factor once, then solve for each right-hand side.
bool factorLUData(double* matrix, int* pivots, int size);
void solveLUData(const double* matrix, const int* pivots, double* rhs, int size);

// Keep the eliminated identity rows with the pivots. If a later RHS would
// move an omitted row, the caller must assemble and factor the current matrix.
class CorrectionLU {
public:
    bool factorAndSolve(double* matrix, double* rhs, int size) noexcept {
        if (size <= 0 || size > maxCompactSolveSize)
            return false;
        size_ = size;
        return solveDroppingZeroCorrections(matrix, rhs, size,
            [&](double* a, double* b, int n, const int* active) {
                count_ = n;
                if (active)
                    std::copy_n(active, n, active_);
                if (!n)
                    return true;
                if (!factorLUData(a, pivots_, n))
                    return false;
                solveLUData(a, pivots_, b, n);
                return true;
            });
    }

    bool solve(const double* matrix, double* rhs) const noexcept {
        if (count_ == size_) {
            solveLUData(matrix, pivots_, rhs, count_);
            return true;
        }
        int j = 0;
        for (int i = 0; i < size_; ++i) {
            if (j < count_ && active_[j] == i)
                rhs[j++] = rhs[i];
            else if (rhs[i] != 0.)
                return false;
        }
        if (count_)
            solveLUData(matrix, pivots_, rhs, count_);
        for (int i = size_ - 1; i >= 0; --i)
            rhs[i] = j > 0 && active_[j - 1] == i ? rhs[--j] : 0.;
        return true;
    }

private:
    int active_[maxCompactSolveSize], pivots_[maxCompactSolveSize];
    int size_, count_;
};

}  // namespace wdf::detail
