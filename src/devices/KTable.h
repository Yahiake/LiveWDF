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

} // namespace wdf
