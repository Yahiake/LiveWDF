#pragma once

// ==============================================================================
// Wright Omega function: the branch w > 0 satisfying w + ln(w) = z.
//
// The fast approximations are Stefano D'Angelo's, from "Fast Approximation of
// the Lambert W Function for Virtual Analog Modelling" (Stefano D'Angelo,
// Leonardo Gabrielli, Luca Turchet; Proceedings of the 22nd International
// Conference on Digital Audio Effects, DAFx-19, Birmingham, September 2019;
// https://www.dafx.de/paper-archive/2019/DAFx2019_paper_5.pdf), whose MIT-
// licensed implementation Jatin Chowdhury's wdf_compiler adapts
// (https://github.com/jatin-chowdhury/wdf_compiler).
//
// omega3 selects between the Estrin cubic fit, the zero asymptote and the
// x - log tail; omega4 adds one exp-based correction on top. Against the true
// root w a residual error e leaves e' = w*e^2 / (2*(1+w)) -- one correction
// carries the cubic fit's 5e-2 to 1e-3-ish, and that omega4 is the function
// wdf's diode library runs its whole engine on.
//
// `wrightOmega` puts two cubic Halley refinements over the same seed, e ->
// O(e^3) per step, for machine precision (residual < 1e-14); cold starts and
// closed-form seeds use it, device hot paths call `detail::omega4` directly.
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

/// Fast polynomial approximation of log2(x) on x in [1, 2].
inline double log2_approx(double x) noexcept {
    constexpr double alpha = 0.16413766037543371;
    constexpr double beta = -1.0989447376924738;
    constexpr double gamma = 3.1414212231630434;
    constexpr double zeta = -2.2129436035178939;
    const double x2 = x * x;
    return (alpha * x + beta) * x2 + (gamma * x + zeta);
}

/// Fast ln(x): split the binary exponent -- e = (ex >> 52) - 1023 with the
/// mantissa rebased to [1, 2) -- polynomial log2 of the mantissa, recombine.
/// The D'Angelo paper's bit trick, evaluated at the double exponent bias.
inline double log_approx(double x) noexcept {
    union {
        uint64_t i;
        double d;
    } v{};
    v.d = x;
    const uint64_t ex = v.i & 0x7ff0000000000000ull;
    const int64_t e = static_cast<int64_t>(ex >> 52) - 1023;
    v.i = (v.i - ex) | 0x3ff0000000000000ull;
    return 0.693147180559945 * (static_cast<double>(e) + log2_approx(v.d));
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

/// D'Angelo's omega3 (DAFx-19 paper): zero asymptote, Estrin cubic fit, or the
/// x - log tail, selected on x.
inline double omega3(double x) noexcept {
    constexpr double x1 = -3.341459552768620;
    constexpr double x2 = 8.0;
    constexpr double a = -1.314293149877800e-3;
    constexpr double b = 4.775931364975583e-2;
    constexpr double c = 3.631952663804445e-1;
    constexpr double d = 6.313183464296682e-1;

    if (x < x1)
        return 0.0;
    if (x < x2)
        return estrin3(a, b, c, d, x);
    return x - log_approx(x);
}

/// D'Angelo's omega4 (DAFx-19 paper): one exp-based correction over omega3,
/// quadratic convergence -- the function wdf's diode library (Jatin Chowdhury,
/// wdf_compiler) runs on. Straight-line, no libm call, no loop.
inline double omega4(double x) noexcept {
    const double y = omega3(x);
    return y - (y - exp_approx(x - y)) / (y + 1.0);
}

}  // namespace detail

/// Evaluates the Wright Omega function w(z) satisfying w + ln(w) = z to
/// machine precision: the wdf seed plus two cubic Halley refinements. Cold
/// paths (closed-form seeds, tests) use this; device hot paths call
/// `detail::omega4` directly.
inline double wrightOmega(double z) noexcept {
    if (z < -36.0)
        return std::exp(z);

    double w = detail::omega4(z);
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
