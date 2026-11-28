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

struct QuadricKnotSteps {
    std::array<double, 64> values{};
    int knots = 0;
    bool active = false;
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
    bool hasQuadric = false;
    const auto add = [&](double x, double dx) {
        if (dx == 0.0 || result.knots == static_cast<int>(result.values.size()) - 1)
            return;
        const double crossing = -x / dx;
        if (!(crossing > 0.0 && crossing < 1.0))
            return;
        const double candidate = crossing + pastVolts / std::abs(dx);
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
                !pentode.reefman.quadric)
                continue;
            hasQuadric = true;
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
    result.active = true;
    return result;
}

}  // namespace wdf::solver
