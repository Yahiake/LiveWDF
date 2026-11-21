#pragma once

// Dense linear algebra (setup only -- never in the audio loop).

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace wdf {

// --------------------------------------------------------------------------- //
// Dense linear algebra (setup only -- never in the audio loop)
// --------------------------------------------------------------------------- //

using Dense = std::vector<std::vector<double>>;

/// Gaussian elimination with partial pivoting on A (n x n) in place, applying
/// the same row operations to B (n x m). Returns false if a pivot is zero.
inline bool solveInPlace(Dense& A, Dense& B) {
    const int n = static_cast<int>(A.size());
    const int m = B.empty() ? 0 : static_cast<int>(B[0].size());
    for (int k = 0; k < n; ++k) {
        int p = k;
        for (int i = k + 1; i < n; ++i)
            if (std::fabs(A[i][k]) > std::fabs(A[p][k]))
                p = i;
        if (A[p][k] == 0.0)
            return false;
        if (p != k) {
            std::swap(A[p], A[k]);
            std::swap(B[p], B[k]);
        }
        const double piv = A[k][k];
        for (int i = k + 1; i < n; ++i) {
            const double f = A[i][k] / piv;
            if (f == 0.0)
                continue;
            for (int j = k; j < n; ++j)
                A[i][j] -= f * A[k][j];
            for (int j = 0; j < m; ++j)
                B[i][j] -= f * B[k][j];
        }
    }
    for (int i = n - 1; i >= 0; --i) {
        for (int j = 0; j < m; ++j) {
            double s = B[i][j];
            for (int k = i + 1; k < n; ++k)
                s -= A[i][k] * B[k][j];
            B[i][j] = s / A[i][i];
        }
    }
    return true;
}

/// Largest absolute residual |A*X - B| for an unfactored system and its
/// solution. Diagnostic only (setup/tests), never called in the audio loop.
/// A successful solve with zero pivot can still be inaccurate; this is the
/// check that pivot finiteness alone cannot provide.
inline double maxBackwardError(const Dense& A, const Dense& B, const Dense& X) {
    const int n = static_cast<int>(A.size());
    if (n == 0 || B.empty() || X.empty())
        return 0.0;
    const int m = static_cast<int>(B[0].size());
    double worst = 0.0;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < m; ++j) {
            double s = 0.0;
            for (int k = 0; k < n; ++k)
                s += A[i][k] * X[k][j];
            worst = std::max(worst, std::fabs(s - B[i][j]));
        }
    }
    return worst;
}

} // namespace wdf
