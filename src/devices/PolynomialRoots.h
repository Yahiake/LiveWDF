#pragma once

// Stable real-root helpers shared by the closed-form transistor models.
//
// The JFET and MOSFET group solves both reduce, in a given operating region, to
// a scalar cubic in the device current. The coefficients are collected from the
// device law and the LiveWDF port equations; this header only finds their real
// roots. It deliberately says nothing about which root is physically valid --
// that check belongs to the device, because the region inequalities are part of
// the model, not part of the algebra.
//
// The cubic path is the same numerically careful Cardano form the JFET has used
// since the first implementation: scale first, fall back to the quadratic path
// when the leading coefficient is negligible, and handle the near-zero
// discriminant explicitly so repeated roots are reported once rather than as a
// cluster of nearly equal values.

#include <algorithm>
#include <cmath>
#include <limits>

namespace wdf::poly {

inline int realQuadraticRoots(double a, double b, double c, double out[3]) noexcept {
    const double scale = std::max({std::abs(a), std::abs(b), std::abs(c)});
    if (!(scale > 0.0) || !std::isfinite(scale))
        return 0;
    a /= scale;
    b /= scale;
    c /= scale;

    constexpr double kDegenerate = 1.0e-14;
    if (std::abs(a) <= kDegenerate) {
        if (std::abs(b) <= kDegenerate)
            return 0;
        out[0] = -c / b;
        return std::isfinite(out[0]) ? 1 : 0;
    }

    double disc = b * b - 4.0 * a * c;
    const double discScale = b * b + std::abs(4.0 * a * c);
    if (disc < 0.0) {
        if (disc > -kDegenerate * (discScale + 1.0e-300))
            disc = 0.0;
        else
            return 0;
    }
    const double sq = std::sqrt(std::max(0.0, disc));
    if (sq == 0.0) {
        out[0] = -b / (2.0 * a);
        return std::isfinite(out[0]) ? 1 : 0;
    }

    // Stable form: avoid subtracting nearly equal roots.
    const double q = -0.5 * (b + (b >= 0.0 ? sq : -sq));
    int n = 0;
    if (q != 0.0) {
        const double r0 = q / a;
        if (std::isfinite(r0))
            out[n++] = r0;
    }
    const double r1 = c / q;
    if (std::isfinite(r1)) {
        bool duplicate = false;
        for (int i = 0; i < n; ++i)
            if (std::abs(out[i] - r1) <= 1.0e-13 * (1.0 + std::abs(r1)))
                duplicate = true;
        if (!duplicate)
            out[n++] = r1;
    }
    return n;
}

inline int realCubicRoots(double a3, double a2, double a1, double a0, double out[3]) noexcept {
    const double scale = std::max({std::abs(a3), std::abs(a2), std::abs(a1), std::abs(a0)});
    if (!(scale > 0.0) || !std::isfinite(scale))
        return 0;
    a3 /= scale;
    a2 /= scale;
    a1 /= scale;
    a0 /= scale;

    constexpr double kDegenerate = 1.0e-13;
    if (std::abs(a3) <= kDegenerate)
        return realQuadraticRoots(a2, a1, a0, out);

    // Depressed cubic x^3 + A x^2 + B x + C = 0 -> y^3 + p y + q = 0.
    const double A = a2 / a3;
    const double B = a1 / a3;
    const double C = a0 / a3;
    if (!std::isfinite(A) || !std::isfinite(B) || !std::isfinite(C))
        return 0;

    const double p = B - A * A / 3.0;
    const double q = 2.0 * A * A * A / 27.0 - A * B / 3.0 + C;
    const double shift = A / 3.0;
    const double q2 = q * q / 4.0;
    const double p3 = p * p * p / 27.0;
    const double disc = q2 + p3;
    if (!std::isfinite(disc))
        return 0;

    const double discTol = 1.0e-14 * (std::abs(q2) + std::abs(p3) + 1.0e-300);
    if (disc > discTol) {
        const double sq = std::sqrt(disc);
        const double u = std::cbrt(-q / 2.0 + sq);
        const double v = std::cbrt(-q / 2.0 - sq);
        out[0] = u + v - shift;
        return std::isfinite(out[0]) ? 1 : 0;
    }

    if (disc < -discTol) {
        if (!(p < 0.0))
            return 0;
        const double m = 2.0 * std::sqrt(-p / 3.0);
        if (!(m > 0.0))
            return 0;
        double arg = 3.0 * q / (p * m);
        arg = std::clamp(arg, -1.0, 1.0);
        const double theta = std::acos(arg) / 3.0;
        constexpr double kTwoPiOver3 = 2.0943951023931954923;
        for (int k = 0; k < 3; ++k) {
            out[k] = m * std::cos(theta - static_cast<double>(k) * kTwoPiOver3) - shift;
            if (!std::isfinite(out[k]))
                return 0;
        }
        return 3;
    }

    // Discriminant is zero (within rounding): a double root and a simple one.
    const double u = std::cbrt(-q / 2.0);
    out[0] = 2.0 * u - shift;
    out[1] = -u - shift;
    if (!std::isfinite(out[0]) || !std::isfinite(out[1]))
        return 0;
    if (std::abs(out[0] - out[1]) <= 1.0e-13 * (1.0 + std::abs(out[1])))
        return 1;
    return 2;
}

}  // namespace wdf::poly
