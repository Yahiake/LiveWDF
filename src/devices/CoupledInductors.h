#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace wdf {

/// One WDF port's state: incoming wave `a`, outgoing wave `b`, and the conductance/resistance
/// pair used by the linear network. This replaces the old `chowdsp_wdf::wdft::WDFMembers` bag:
/// coupled inductors only need these four numbers and the local wave law.
struct PortWave {
    double a = 0.0;  // incoming wave
    double b = 0.0;  // outgoing wave
    double G = 0.0;  // conductance (siemens)
    double R = 0.0;  // resistance (ohms)
};

/// W magnetically coupled inductors with one coupling coefficient k for every pair, as the SPICE
/// OT/K model:
///
///   L_ii = L_i,
///   L_ij = k * sqrt(L_i * L_j)       for i != j,
///   G_L  = (T / 2) * L^{-1},
///   R_L  = (2 / T) * L.
///
/// Each winding is a one-port WDF element with
///
///   b[n] = -a[n - 1]
///
/// and the magnetic coupling lives in the port conductance matrix `G_L`. The Sherman-Morrison
/// closed form below is kept because it is the old SPICE-K oracle used by the test suite.
///
/// Status: compatibility helper for SPICE `K` validation and tests. The transformer path uses
/// `CoupledInductorMatrices` with explicit matrices.
template <int W>
class CoupledInductors {
public:
    /// One coupled winding. `wdf.G` is the diagonal entry of `G_L`.
    struct Winding {
        PortWave wdf;
        double z = 0.0;  // previous incoming wave: a[n - 1]

        inline double reflected() noexcept {
            wdf.b = -z;
            return wdf.b;
        }

        inline void incident(double incoming) noexcept {
            wdf.a = incoming;
            z = incoming;
        }
    };

    CoupledInductors(const std::array<double, W>& inductance, double coupling)
        : L_(inductance), k_(coupling) {
        prepare(48000.0);
    }

    void prepare(double sampleRate) {
        if (!(sampleRate > 0.0))
            throw std::runtime_error("coupled inductor sample rate must be positive");

        const double T = 1.0 / sampleRate;
        const double oneMinusK = 1.0 - k_;
        if (!(std::fabs(oneMinusK) > std::numeric_limits<double>::min()))
            throw std::runtime_error("coupled inductor coupling coefficient is invalid");

        const double c = k_ / (oneMinusK * (oneMinusK + k_ * static_cast<double>(W)));
        for (int i = 0; i < W; ++i) {
            for (int j = 0; j < W; ++j) {
                const double inv =
                    (i == j ? 1.0 / (oneMinusK * L_[static_cast<size_t>(i)]) : 0.0) -
                    c / std::sqrt(L_[static_cast<size_t>(i)] * L_[static_cast<size_t>(j)]);

                G_[static_cast<size_t>(i)][static_cast<size_t>(j)] = 0.5 * T * inv;
                R_[static_cast<size_t>(i)][static_cast<size_t>(j)] =
                    2.0 / T *
                    (i == j
                         ? L_[static_cast<size_t>(i)]
                         : k_ * std::sqrt(L_[static_cast<size_t>(i)] *
                                          L_[static_cast<size_t>(j)]));
            }
        }

        for (int i = 0; i < W; ++i) {
            w[static_cast<size_t>(i)].wdf.G = G_[static_cast<size_t>(i)][static_cast<size_t>(i)];
            w[static_cast<size_t>(i)].wdf.R = 1.0 / G_[static_cast<size_t>(i)][static_cast<size_t>(i)];
            w[static_cast<size_t>(i)].z = 0.0;
        }
    }

    /// Converts the trapezoidal states for a time step T' = ratio * T, keeping the last port
    /// voltages and currents: R_L = (2 / T) * L scales by 1 / ratio, so the state
    /// z = v + R_L * i becomes v + (z - v) / ratio, with v = (a + b) / 2 of the last step.
    void rescaleState(double ratio) noexcept {
        for (auto& x : w) {
            const double v = 0.5 * (x.wdf.a + x.wdf.b);
            x.z = v + (x.z - v) / ratio;
        }
    }

    /// Port conductance matrix entry G_L[i][j] (siemens).
    double conductance(int i, int j) const noexcept {
        if (i < 0 || j < 0 || i >= W || j >= W)
            return 0.0;
        return G_[static_cast<size_t>(i)][static_cast<size_t>(j)];
    }

    /// DC state for winding currents i (amperes, into each winding's positive terminal) with
    /// zero voltage: a = R_L * i, so the reflected b = -a keeps the port voltage at zero.
    void setDcCurrents(const std::array<double, W>& currents) noexcept {
        for (int r = 0; r < W; ++r) {
            double a = 0.0;
            for (int c = 0; c < W; ++c)
                a += R_[static_cast<size_t>(r)][static_cast<size_t>(c)] *
                     currents[static_cast<size_t>(c)];

            w[static_cast<size_t>(r)].z = a;
            w[static_cast<size_t>(r)].wdf.a = a;
            w[static_cast<size_t>(r)].wdf.b = -a;
        }
    }

    std::array<Winding, W> w{};

private:
    std::array<double, W> L_{};
    double k_ = 0.0;
    std::array<std::array<double, W>, W> G_{};
    std::array<std::array<double, W>, W> R_{};
};

namespace detail {
using CoupledMatrix = std::vector<std::vector<double>>;

/// Inverts a square matrix in place with Gauss-Jordan elimination. The coupled-inductor
/// transformer matrices are small (2x2, 3x3), so this simple pivoted inverse is clearer and
/// self-contained than carrying an external WDF/R-type dependency.
inline CoupledMatrix invert(CoupledMatrix work) {
    const std::size_t n = work.size();
    CoupledMatrix inv(n, std::vector<double>(n, 0.0));
    for (std::size_t i = 0; i < n; ++i)
        inv[i][i] = 1.0;

    double scale = 0.0;
    for (const auto& row : work)
        for (double value : row)
            scale = std::max(scale, std::fabs(value));

    if (!(scale > 0.0))
        throw std::runtime_error("coupled inductor matrix has no usable entries");

    const double pivotFloor = scale * 1.0e-12;
    for (std::size_t col = 0; col < n; ++col) {
        std::size_t pivot = col;
        for (std::size_t row = col + 1; row < n; ++row) {
            if (std::fabs(work[row][col]) > std::fabs(work[pivot][col]))
                pivot = row;
        }

        if (!(std::fabs(work[pivot][col]) > pivotFloor))
            throw std::runtime_error("coupled inductor matrix is singular or too ill-conditioned");

        if (pivot != col) {
            std::swap(work[pivot], work[col]);
            std::swap(inv[pivot], inv[col]);
        }

        const double pivotValue = work[col][col];
        for (std::size_t j = 0; j < n; ++j) {
            work[col][j] /= pivotValue;
            inv[col][j] /= pivotValue;
        }

        for (std::size_t row = 0; row < n; ++row) {
            if (row == col)
                continue;

            const double factor = work[row][col];
            if (factor == 0.0)
                continue;

            for (std::size_t j = 0; j < n; ++j) {
                work[row][j] -= factor * work[col][j];
                inv[row][j] -= factor * inv[col][j];
            }
        }
    }

    return inv;
}
}  // namespace detail

/// Dynamic coupled inductors for arbitrary winding count W >= 2.
///
/// The SPICE `K` group uses diagonal inductances and one shared pair coefficient. A real
/// transformer supplies an explicit inductance matrix, which is the only way to represent
/// turns-ratio coupling and leakage without pretending every transformer is a pair of
/// equal-coefficient windings.
struct CoupledInductorMatrices {
    std::vector<double> L;
    double k = 0.0;
    detail::CoupledMatrix Lmat;
    detail::CoupledMatrix G;
    detail::CoupledMatrix R;

    CoupledInductorMatrices() = default;

    CoupledInductorMatrices(const std::vector<double>& inductances, double coupling, double sampleRate)
        : L(inductances), k(coupling) {
        prepareClosed(sampleRate);
    }

    CoupledInductorMatrices(const detail::CoupledMatrix& explicitL, double sampleRate)
        : Lmat(explicitL) {
        prepareExplicit(sampleRate);
    }

    void prepareClosed(double sampleRate) {
        const std::size_t W = L.size();
        G.assign(W, std::vector<double>(W, 0.0));
        R.assign(W, std::vector<double>(W, 0.0));
        if (W < 2)
            return;

        if (!(sampleRate > 0.0))
            throw std::runtime_error("coupled inductor sample rate must be positive");
        for (double value : L) {
            if (!(value > 0.0))
                throw std::runtime_error("coupled inductor diagonal inductance must be positive");
        }

        const double T = 1.0 / sampleRate;
        const double oneMinusK = 1.0 - k;
        if (!(std::fabs(oneMinusK) > std::numeric_limits<double>::min()))
            throw std::runtime_error("coupled inductor coupling coefficient is invalid");

        const double c = k / (oneMinusK * (oneMinusK + k * static_cast<double>(W)));
        for (std::size_t i = 0; i < W; ++i) {
            for (std::size_t j = 0; j < W; ++j) {
                const double inv =
                    (i == j ? 1.0 / (oneMinusK * L[i]) : 0.0) -
                    c / std::sqrt(L[i] * L[j]);

                G[i][j] = 0.5 * T * inv;
                R[i][j] = 2.0 / T * (i == j ? L[i] : k * std::sqrt(L[i] * L[j]));
            }
        }
    }

    void prepareExplicit(double sampleRate) {
        const std::size_t W = Lmat.size();
        G.assign(W, std::vector<double>(W, 0.0));
        R.assign(W, std::vector<double>(W, 0.0));
        if (W < 2)
            return;

        if (!(sampleRate > 0.0))
            throw std::runtime_error("coupled inductor sample rate must be positive");

        for (const auto& row : Lmat) {
            if (row.size() != W)
                throw std::runtime_error("coupled inductor matrix is not square");
        }

        const detail::CoupledMatrix inv = detail::invert(Lmat);
        const double T = 1.0 / sampleRate;
        for (std::size_t i = 0; i < W; ++i) {
            for (std::size_t j = 0; j < W; ++j) {
                G[i][j] = 0.5 * T * inv[i][j];
                R[i][j] = 2.0 / T * Lmat[i][j];
            }
        }
    }
};

}  // namespace wdf
