#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <cstddef>

namespace wdf {

/// Fast 1D Lookup Table with Cubic Hermite / Linear interpolation (K-Method).
///
/// Precomputes nonlinear implicit port characteristics v = f(vo, Rp) off
/// the audio thread to replace expensive transcendentals (std::exp, std::log,
/// Wright Omega) with ~5 ALU cycles in the audio callback.
///
/// Reference: pedalkernel-rt/src/stage.rs (KTable), D'Angelo & Chowdhury DAFx 2019.
class KTable1D {
public:
    KTable1D() = default;

    /// Build a 1D table over [minV, maxV] with `steps` entries using an exact evaluator.
    template <typename Func>
    void build(double minV, double maxV, int steps, Func&& func) {
        minV_ = minV;
        maxV_ = maxV;
        steps_ = std::max(steps, 4);
        invStep_ = static_cast<double>(steps_ - 1) / (maxV_ - minV_);
        step_ = (maxV_ - minV_) / static_cast<double>(steps_ - 1);

        table_.resize(static_cast<size_t>(steps_));
        slopes_.resize(static_cast<size_t>(steps_));

        for (int i = 0; i < steps_; ++i) {
            const double v = (i == steps_ - 1) ? maxV_ : (minV_ + i * step_);
            table_[static_cast<size_t>(i)] = func(v);
        }

        // Central difference scaled tangents: d_i = h * y'_i
        for (int i = 0; i < steps_; ++i) {
            if (i == 0) {
                slopes_[0] = table_[1] - table_[0];
            } else if (i == steps_ - 1) {
                slopes_[static_cast<size_t>(i)] =
                    table_[static_cast<size_t>(i)] - table_[static_cast<size_t>(i - 1)];
            } else {
                slopes_[static_cast<size_t>(i)] =
                    0.5 * (table_[static_cast<size_t>(i + 1)] - table_[static_cast<size_t>(i - 1)]);
            }
        }
        ready_ = true;
    }

    /// Fast lookup using cubic Hermite interpolation.
    inline double lookup(double v) const noexcept {
        if (!ready_)
            return 0.0;

        // Asymptotic linear continuation outside table bounds
        if (v <= minV_) {
            return table_[0] + slopes_[0] * invStep_ * (v - minV_);
        }
        if (v >= maxV_) {
            const size_t last = static_cast<size_t>(steps_ - 1);
            return table_[last] + slopes_[last] * invStep_ * (v - maxV_);
        }

        const double p = (v - minV_) * invStep_;
        const int idx = std::min(static_cast<int>(p), steps_ - 2);
        const double f = p - static_cast<double>(idx);

        const double y0 = table_[static_cast<size_t>(idx)];
        const double y1 = table_[static_cast<size_t>(idx + 1)];
        const double m0 = slopes_[static_cast<size_t>(idx)];
        const double m1 = slopes_[static_cast<size_t>(idx + 1)];

        // Cubic Hermite basis functions
        const double f2 = f * f;
        const double f3 = f2 * f;
        const double h00 = 2.0 * f3 - 3.0 * f2 + 1.0;
        const double h10 = f3 - 2.0 * f2 + f;
        const double h01 = -2.0 * f3 + 3.0 * f2;
        const double h11 = f3 - f2;

        return h00 * y0 + h10 * m0 + h01 * y1 + h11 * m1;
    }

    bool isReady() const noexcept { return ready_; }
    double minV() const noexcept { return minV_; }
    double maxV() const noexcept { return maxV_; }
    int steps() const noexcept { return steps_; }

private:
    double minV_ = -30.0;
    double maxV_ = 30.0;
    double step_ = 0.0;
    double invStep_ = 0.0;
    int steps_ = 0;
    bool ready_ = false;
    std::vector<double> table_;
    std::vector<double> slopes_;
};

/// Fast 2D Lookup Table with Bilinear & Bicubic interpolation (K-Method).
///
/// Precomputes 2D nonlinear multi-terminal characteristics (e.g. Triode plate
/// current Ip(Vgk, Vpk), BJT Ic(Vbe, Vce), or 2D root waves a_root(b_tree, ctrl))
/// off the audio thread to eliminate expensive powers, transcendentals, and
/// iterations in the real-time audio callback.
///
/// Reference: pedalkernel-rt/src/stage.rs (KTable lookup_2d), D'Angelo & Chowdhury DAFx 2019.
class KTable2D {
public:
    KTable2D() = default;

    /// Build a 2D table over [minX, maxX] x [minY, maxY] with `stepsX` x `stepsY` entries.
    template <typename Func>
    void build(double minX, double maxX, int stepsX,
               double minY, double maxY, int stepsY,
               Func&& func) {
        minX_ = minX;
        maxX_ = maxX;
        minY_ = minY;
        maxY_ = maxY;
        stepsX_ = std::max(stepsX, 2);
        stepsY_ = std::max(stepsY, 2);
        invStepX_ = static_cast<double>(stepsX_ - 1) / (maxX_ - minX_);
        invStepY_ = static_cast<double>(stepsY_ - 1) / (maxY_ - minY_);
        stepX_ = (maxX_ - minX_) / static_cast<double>(stepsX_ - 1);
        stepY_ = (maxY_ - minY_) / static_cast<double>(stepsY_ - 1);

        const size_t total = static_cast<size_t>(stepsX_) * static_cast<size_t>(stepsY_);
        table_.resize(total);

        for (int iy = 0; iy < stepsY_; ++iy) {
            const double y = (iy == stepsY_ - 1) ? maxY_ : (minY_ + iy * stepY_);
            const size_t rowOffset = static_cast<size_t>(iy) * static_cast<size_t>(stepsX_);
            for (int ix = 0; ix < stepsX_; ++ix) {
                const double x = (ix == stepsX_ - 1) ? maxX_ : (minX_ + ix * stepX_);
                table_[rowOffset + static_cast<size_t>(ix)] = func(x, y);
            }
        }
        ready_ = true;
    }

    /// Fast 2D bilinear lookup (~5 ns).
    inline double lookup(double x, double y) const noexcept {
        if (!ready_)
            return 0.0;

        const double px = std::clamp((x - minX_) * invStepX_, 0.0, static_cast<double>(stepsX_ - 1));
        const double py = std::clamp((y - minY_) * invStepY_, 0.0, static_cast<double>(stepsY_ - 1));

        const int ix = std::min(static_cast<int>(px), stepsX_ - 2);
        const int iy = std::min(static_cast<int>(py), stepsY_ - 2);

        const double fx = px - static_cast<double>(ix);
        const double fy = py - static_cast<double>(iy);

        const size_t row0 = static_cast<size_t>(iy) * static_cast<size_t>(stepsX_);
        const size_t row1 = row0 + static_cast<size_t>(stepsX_);

        const double v00 = table_[row0 + static_cast<size_t>(ix)];
        const double v10 = table_[row0 + static_cast<size_t>(ix + 1)];
        const double v01 = table_[row1 + static_cast<size_t>(ix)];
        const double v11 = table_[row1 + static_cast<size_t>(ix + 1)];

        const double t0 = v00 + fx * (v10 - v00);
        const double t1 = v01 + fx * (v11 - v01);
        return t0 + fy * (t1 - t0);
    }

    /// Fast 2D lookup returning interpolated value and analytical partial derivatives (~8 ns).
    inline void lookupWithDerivatives(double x, double y,
                                      double& val, double& dx, double& dy) const noexcept {
        if (!ready_) {
            val = 0.0;
            dx = 0.0;
            dy = 0.0;
            return;
        }

        const double px = std::clamp((x - minX_) * invStepX_, 0.0, static_cast<double>(stepsX_ - 1));
        const double py = std::clamp((y - minY_) * invStepY_, 0.0, static_cast<double>(stepsY_ - 1));

        const int ix = std::min(static_cast<int>(px), stepsX_ - 2);
        const int iy = std::min(static_cast<int>(py), stepsY_ - 2);

        const double fx = px - static_cast<double>(ix);
        const double fy = py - static_cast<double>(iy);

        const size_t row0 = static_cast<size_t>(iy) * static_cast<size_t>(stepsX_);
        const size_t row1 = row0 + static_cast<size_t>(stepsX_);

        const double v00 = table_[row0 + static_cast<size_t>(ix)];
        const double v10 = table_[row0 + static_cast<size_t>(ix + 1)];
        const double v01 = table_[row1 + static_cast<size_t>(ix)];
        const double v11 = table_[row1 + static_cast<size_t>(ix + 1)];

        const double diffX0 = v10 - v00;
        const double diffX1 = v11 - v01;

        const double t0 = v00 + fx * diffX0;
        const double t1 = v01 + fx * diffX1;

        val = t0 + fy * (t1 - t0);
        dx = ((1.0 - fy) * diffX0 + fy * diffX1) * invStepX_;
        dy = (t1 - t0) * invStepY_;
    }

    /// High-order Bicubic Hermite interpolation (Catmull-Rom tensor product, O(h^4) accuracy, ~15 ns).
    inline double lookupBicubic(double x, double y) const noexcept {
        if (!ready_)
            return 0.0;

        const double px = std::clamp((x - minX_) * invStepX_, 0.0, static_cast<double>(stepsX_ - 1));
        const double py = std::clamp((y - minY_) * invStepY_, 0.0, static_cast<double>(stepsY_ - 1));

        const int ix = std::min(static_cast<int>(px), stepsX_ - 2);
        const int iy = std::min(static_cast<int>(py), stepsY_ - 2);

        const double fx = px - static_cast<double>(ix);
        const double fy = py - static_cast<double>(iy);

        auto at = [this](int xi, int yi) noexcept -> double {
            const int cx = std::clamp(xi, 0, stepsX_ - 1);
            const int cy = std::clamp(yi, 0, stepsY_ - 1);
            return table_[static_cast<size_t>(cy) * static_cast<size_t>(stepsX_) + static_cast<size_t>(cx)];
        };

        // 1D cubic Hermite / Catmull-Rom evaluator along a 4-point slice
        auto hermite4 = [](double p0, double p1, double p2, double p3, double t) noexcept -> double {
            const double c0 = p1;
            const double c1 = 0.5 * (p2 - p0);
            const double c2 = p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3;
            const double c3 = 0.5 * (p3 - p0) + 1.5 * (p1 - p2);
            return ((c3 * t + c2) * t + c1) * t + c0;
        };

        // Interpolate along the 4 rows
        const double r0 = hermite4(at(ix - 1, iy - 1), at(ix, iy - 1), at(ix + 1, iy - 1), at(ix + 2, iy - 1), fx);
        const double r1 = hermite4(at(ix - 1, iy),     at(ix, iy),     at(ix + 1, iy),     at(ix + 2, iy),     fx);
        const double r2 = hermite4(at(ix - 1, iy + 1), at(ix, iy + 1), at(ix + 1, iy + 1), at(ix + 2, iy + 1), fx);
        const double r3 = hermite4(at(ix - 1, iy + 2), at(ix, iy + 2), at(ix + 1, iy + 2), at(ix + 2, iy + 2), fx);

        // Interpolate along the column
        return hermite4(r0, r1, r2, r3, fy);
    }

    bool isReady() const noexcept { return ready_; }
    double minX() const noexcept { return minX_; }
    double maxX() const noexcept { return maxX_; }
    double minY() const noexcept { return minY_; }
    double maxY() const noexcept { return maxY_; }
    int stepsX() const noexcept { return stepsX_; }
    int stepsY() const noexcept { return stepsY_; }
    size_t size() const noexcept { return table_.size(); }

private:
    double minX_ = 0.0;
    double maxX_ = 0.0;
    double minY_ = 0.0;
    double maxY_ = 0.0;
    double stepX_ = 0.0;
    double stepY_ = 0.0;
    double invStepX_ = 0.0;
    double invStepY_ = 0.0;
    int stepsX_ = 0;
    int stepsY_ = 0;
    bool ready_ = false;
    std::vector<double> table_;
};

} // namespace wdf
