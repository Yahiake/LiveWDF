#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "wdf/DenseTopology.h"
#include "wdf/solver/PushPullTries.h"

namespace wdf::solver {

inline bool quadricKnotStepEnabled() noexcept {
    static const bool enabled = livewdfPpTry("knots");
    return enabled;
}

inline bool polynomialKnotStepEnabled() noexcept {
    static const bool enabled = livewdfPpTry("knots2");
    return enabled;
}

struct QuadricKnotSteps {
    std::array<double, 64> values{};
    int knots = 0;
    bool active = false;
    bool polynomial = false;
};

inline QuadricKnotSteps quadricKnotSteps(
    const std::vector<int>& devices,
    const std::vector<RootDevice>& rootDevices,
    const Compiled& comp,
    const std::vector<Pentode>& pentodes,
    const std::vector<unsigned char>& triodeDomainPin,
    const double* v,
    const double* d) noexcept
{
    constexpr double pastVolts = 0.1;
    QuadricKnotSteps result;
    result.polynomial = polynomialKnotStepEnabled();
    bool hasQuadric = false;
    bool hasPolynomialPentode = false;
    const auto add = [&](double x, double dx) {
        if (dx == 0.0 || result.knots == static_cast<int>(result.values.size()) - 1)
            return;
        const double crossing = -x / dx;
        if (!(crossing > 0.0 && crossing < 1.0))
            return;
        const double candidate = result.polynomial ? crossing : crossing + pastVolts / std::abs(dx);
        if (candidate < 1.0)
            result.values[static_cast<size_t>(result.knots++)] = candidate;
    };

    for (int di : devices) {
        const RootDevice& dev = rootDevices[static_cast<size_t>(di)];
        const int o = dev.portBase;
        if (dev.kind == DeviceKind::Triode && triodeDomainPin[static_cast<size_t>(o)] == 0) {
            hasQuadric = true;
            const TriodeModel& model = comp.tri[dev.compIndex].model;
            add(v[o], d[o]);
            add(v[o] + model.cg * v[o + 2] + model.c0,
                d[o] + model.cg * d[o + 2]);
            for (const auto& line : TriodeModel::kGridLines)
                add(v[o + 2] - line.on, d[o + 2]);
            for (const auto& a : TriodeModel::kGridLines)
                for (const auto& b : TriodeModel::kGridLines)
                    if (a.r < b.r)
                        add((v[o + 2] - a.on) / a.r - (v[o + 2] - b.on) / b.r,
                            d[o + 2] / a.r - d[o + 2] / b.r);
        } else if (dev.kind == DeviceKind::Pentode) {
            const Pentode& pentode = pentodes[dev.compIndex];
            if (pentode.modelKind != PentodeModelKind::Reefman || !pentode.useReefman ||
                (!pentode.reefman.quadric && !pentode.reefman.polyknee))
                continue;
            hasQuadric = true;
            if (result.polynomial && pentode.reefman.polyknee) {
                hasPolynomialPentode = true;
                pentode.reefman.polyknee->regimeBoundaries(
                    v[o + 1], v[o + 2], v[o], d[o + 1], d[o + 2], d[o], add);
                continue;
            }
            if (!pentode.reefman.quadric)
                continue;
            const QuadricEmission& q = *pentode.reefman.quadric;
            add(v[o], d[o]);
            add(v[o + 2], d[o + 2]);
            for (int j = 0; j < 5; ++j)
                add(v[o + 1] + q.av[j] * v[o + 2] - q.knot[j],
                    d[o + 1] + q.av[j] * d[o + 2]);
            add(v[o + 1] + 37.0 * pentode.reefman.grid_vt, d[o + 1]);
            add(v[o + 1] - 500.0 * pentode.reefman.grid_vt, d[o + 1]);
        }
    }

    if (!hasQuadric)
        return {};
    std::sort(result.values.begin(), result.values.begin() + result.knots);
    result.knots = static_cast<int>(std::unique(
        result.values.begin(), result.values.begin() + result.knots,
        [](double a, double b) { return std::abs(a - b) < 1e-12; }) - result.values.begin());
    result.values[static_cast<size_t>(result.knots)] = 1.0;
    result.active = result.polynomial ? hasPolynomialPentode : result.knots > 0;
    return result;
}

struct QuarticStep {
    double step = 1.0;
    double predicted = INFINITY;
};

inline double polynomialValue(const double* c, int degree, double x) noexcept {
    double y = c[degree];
    while (degree-- > 0)
        y = y * x + c[degree];
    return y;
}

inline void polynomialRoots(const double* c, int degree, double* roots, int& count) noexcept {
    double scale = 0.0;
    for (int i = 0; i <= degree; ++i)
        scale = std::max(scale, std::abs(c[i]));
    while (degree > 0 && std::abs(c[degree]) <= scale * 1e-14)
        --degree;
    if (degree == 0)
        return;
    if (degree == 1) {
        const double x = -c[0] / c[1];
        if (x > 0.0 && x < 1.0)
            roots[count++] = x;
        return;
    }

    double derivative[16]{};
    for (int i = 1; i <= degree; ++i)
        derivative[i - 1] = i * c[i];
    double critical[16]{};
    int criticalCount = 0;
    polynomialRoots(derivative, degree - 1, critical, criticalCount);
    double left = 0.0;
    double fLeft = c[0];
    for (int interval = 0; interval <= criticalCount; ++interval) {
        const double right = interval < criticalCount ? critical[interval] : 1.0;
        const double fRight = polynomialValue(c, degree, right);
        if (fLeft * fRight < 0.0) {
            double a = left, b = right, fa = fLeft;
            for (int j = 0; j < 48; ++j) {
                const double mid = 0.5 * (a + b);
                const double fm = polynomialValue(c, degree, mid);
                if (fa * fm <= 0.0)
                    b = mid;
                else
                    a = mid, fa = fm;
            }
            roots[count++] = 0.5 * (a + b);
        }
        if (interval < criticalCount && std::abs(fRight) <= scale * 1e-12)
            roots[count++] = right;
        left = right;
        fLeft = fRight;
    }
    std::sort(roots, roots + count);
    count = static_cast<int>(std::unique(roots, roots + count,
        [](double a, double b) { return std::abs(a - b) < 1e-10; }) - roots);
}

template <typename Evaluate>
inline QuarticStep quarticKnotStep(const QuadricKnotSteps& steps, int rows,
                                   Evaluate evaluate) noexcept {
    QuarticStep best;
    double bestNorm = INFINITY;
    double lo = 0.0;
    for (int segment = 0; segment <= steps.knots; ++segment) {
        const double hi = steps.values[static_cast<size_t>(segment)];
        double samples[5][64]{};
        for (int j = 0; j < 5; ++j)
            evaluate(lo + (hi - lo) * (0.25 * j), samples[j]);

        double residual[64][5]{};
        for (int row = 0; row < rows; ++row) {
            double difference[5];
            for (int j = 0; j < 5; ++j)
                difference[j] = samples[j][row];
            double basis[5]{1.0};
            int basisDegree = 0;
            for (int order = 0; order < 5; ++order) {
                const double factor = difference[0] * std::pow(4.0, order) /
                    (order == 0 ? 1.0 : order == 1 ? 1.0 : order == 2 ? 2.0 :
                     order == 3 ? 6.0 : 24.0);
                for (int k = 0; k <= basisDegree; ++k)
                    residual[row][k] += factor * basis[k];
                for (int j = 0; j < 4 - order; ++j)
                    difference[j] = difference[j + 1] - difference[j];
                if (order == 4)
                    break;
                for (int k = basisDegree + 1; k > 0; --k)
                    basis[k] = basis[k - 1] - 0.25 * order * basis[k];
                basis[0] *= -0.25 * order;
                ++basisDegree;
            }
        }

        double norm[9]{};
        for (int row = 0; row < rows; ++row)
            for (int a = 0; a < 5; ++a)
                for (int b = 0; b < 5; ++b)
                    norm[a + b] += residual[row][a] * residual[row][b];
        double derivative[8]{};
        for (int i = 1; i < 9; ++i)
            derivative[i - 1] = i * norm[i];
        double candidates[16]{0.0, 1.0};
        int count = 2;
        polynomialRoots(derivative, 7, candidates, count);
        for (int j = 0; j < count; ++j) {
            const double u = candidates[j];
            const double squaredNorm = polynomialValue(norm, 8, u);
            double maximum = 0.0;
            for (int row = 0; row < rows; ++row)
                maximum = std::max(maximum, std::abs(polynomialValue(residual[row], 4, u)));
            if (squaredNorm < bestNorm) {
                bestNorm = squaredNorm;
                best = {lo + (hi - lo) * u, maximum};
            }
        }
        lo = hi;
    }
    return best;
}

}  // namespace wdf::solver
