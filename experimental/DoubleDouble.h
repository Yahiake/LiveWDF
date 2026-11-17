#pragma once

#include <cmath>

// The double-double identities below are exact only under strict IEEE
// semantics. MSVC's /fp:fast decomposes std::fma(a, b, -p) back into a mul-add
// and reassociates the compensated sum, which silently collapses DD to plain
// double (twoProd's low limb becomes 0). Pin precise FP for this header even
// when the translation unit is built fast; /fp:precise is the GCC/clang default.
#if defined(_MSC_VER)
#pragma float_control(precise, on, push)
#endif

namespace wdf {

/// Double-double number: value = hi + lo with |lo| <= ulp(hi)/2, about 32
/// significant digits. Ported from `ampsim2` engine/src/mna/DoubleDouble.h
/// (error-free transformations after Dekker / Knuth; products use fma).
///
/// It exists only for *setup* arithmetic, where plain double is not enough for
/// the R-type up-port condition at extreme pot settings (ampsim2 PROGRESS Q11 /
/// DECISIONS D-026). It is never part of the per-sample path.
struct DD {
    double hi = 0.0;
    double lo = 0.0;

    constexpr DD() = default;
    constexpr DD(double h) noexcept : hi(h) {}  // NOLINT: promotes doubles
    constexpr DD(double h, double l) noexcept : hi(h), lo(l) {}

    double toDouble() const noexcept { return hi + lo; }
};

namespace dd_detail {

inline DD twoSum(double a, double b) noexcept {
    const double s = a + b;
    const double bb = s - a;
    return {s, (a - (s - bb)) + (b - bb)};
}

inline DD quickTwoSum(double a, double b) noexcept {
    const double s = a + b;
    return {s, b - (s - a)};
}

inline DD twoProd(double a, double b) noexcept {
    const double p = a * b;
    return {p, std::fma(a, b, -p)};
}

}  // namespace dd_detail

inline DD operator+(DD a, DD b) noexcept {
    DD s = dd_detail::twoSum(a.hi, b.hi);
    const DD t = dd_detail::twoSum(a.lo, b.lo);
    s.lo += t.hi;
    s = dd_detail::quickTwoSum(s.hi, s.lo);
    s.lo += t.lo;
    return dd_detail::quickTwoSum(s.hi, s.lo);
}

inline DD operator-(DD a) noexcept { return {-a.hi, -a.lo}; }
inline DD operator-(DD a, DD b) noexcept { return a + (-b); }

inline DD operator*(DD a, DD b) noexcept {
    DD p = dd_detail::twoProd(a.hi, b.hi);
    p.lo += a.hi * b.lo + a.lo * b.hi;
    return dd_detail::quickTwoSum(p.hi, p.lo);
}

inline DD operator/(DD a, DD b) noexcept {
    const double q1 = a.hi / b.hi;
    DD r = a - b * DD(q1);
    const double q2 = r.hi / b.hi;
    r = r - b * DD(q2);
    const double q3 = r.hi / b.hi;
    return dd_detail::quickTwoSum(q1, q2) + DD(q3);
}

inline DD& operator+=(DD& a, DD b) noexcept { return a = a + b; }
inline DD& operator-=(DD& a, DD b) noexcept { return a = a - b; }
inline double abs(DD a) noexcept { return std::fabs(a.hi); }

}  // namespace wdf

#if defined(_MSC_VER)
#pragma float_control(pop)
#endif
