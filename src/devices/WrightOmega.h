#pragma once

// ==============================================================================
// Wright Omega function: w > 0 satisfying w + ln(w) = z.
//
// Initial approximation derived from Stefano D'Angelo / Jatin Chowdhury
// (wdf_compiler / DAFx 2019, MIT License).
// Asymptotic branches and Halley refinement: LiveWDF (AGPLv3).
//
// Solves the principal branch W0(e^z) to machine precision (< 1e-14 residual).
// ==============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wdf {

namespace detail {

/// Evaluates degree-3 polynomial a*x^3 + b*x^2 + c*x + d using Estrin's scheme.
inline double estrin3(double a, double b, double c, double d, double x) noexcept {
    const double x2 = x * x;
    return (a * x + b) * x2 + (c * x + d);
}

/// Fast polynomial approximation of 2^x on x in [0, 1].
inline double pow2_approx(double x) noexcept {
    constexpr double alpha = 0.07944154167983575;
    constexpr double beta = 0.2274112777602189;
    constexpr double gamma = 0.6931471805599453;
    constexpr double zeta = 1.0;
    return estrin3(alpha, beta, gamma, zeta, x);
}

/// Fast bit-manipulation exp(x) approximation.
inline double exp_approx(double x) noexcept {
    x = std::max(-126.0, 1.4426950408889634 * x);  // x * log2(e)
    union {
        uint64_t i;
        double d;
    } v{};
    const auto xi = static_cast<int64_t>(x);
    const int64_t l = (x < 0.0) ? xi - 1 : xi;
    const double d = x - static_cast<double>(l);
    v.i = static_cast<uint64_t>(l + 1023) << 52;
    return v.d * pow2_approx(d);
}

/// D'Angelo / Chowdhury 4th-order Wright Omega approximation seed.
inline double omega4_seed(double x) noexcept {
    constexpr double x1 = -3.341459552768620;
    constexpr double x2 = 8.0;
    constexpr double a = -1.314293149877800e-3;
    constexpr double b = 4.775931364975583e-2;
    constexpr double c = 3.631952663804445e-1;
    constexpr double d = 6.313183464296682e-1;

    double y;
    if (x < x1) {
        y = 0.0;
    } else if (x < x2) {
        y = estrin3(a, b, c, d, x);
    } else {
        y = x - std::log(x);
    }
    return y - (y - exp_approx(x - y)) / (y + 1.0);
}

}  // namespace detail

/// Evaluates the Wright Omega function w(z) satisfying w + ln(w) = z.
///
/// Refined with 2 cubic Halley steps to machine precision (residual < 1e-14)
/// across all operating regimes.
inline double wrightOmega(double z) noexcept {
    if (z < -36.0)
        return std::exp(z);

    double w = detail::omega4_seed(z);
    if (w <= 0.0)
        w = std::exp(z);

    // Two Halley refinement steps: cubic convergence to machine precision (< 7e-15).
    for (int i = 0; i < 2; ++i) {
        const double r = z - w - std::log(w);
        const double wp1 = w + 1.0;
        w += 2.0 * r * w * wp1 / (2.0 * wp1 * wp1 - r);
    }
    return w;
}

}  // namespace wdf
