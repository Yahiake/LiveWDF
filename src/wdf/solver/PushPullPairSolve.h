#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace wdf::solver {

template<class Context>
inline bool solvePushPullDifference(const NewtonBlock& block, Context& ctx,
                                    const std::vector<double>& networkZ, int n) noexcept {
    const RootDevice* left = nullptr;
    const RootDevice* right = nullptr;
    int leftLocal = 0;
    int rightLocal = 0;
    int local = 0;
    for (int di : block.devices) {
        const auto& dev = ctx.rootDevices[static_cast<size_t>(di)];
        if (dev.kind == DeviceKind::Pentode) {
            if (!left) {
                left = &dev;
                leftLocal = local;
            } else if (!right) {
                right = &dev;
                rightLocal = local;
            } else {
                return false;
            }
        }
        local += dev.numPorts;
    }
    if (!left || !right || left->numPorts != 3 || right->numPorts != 3 ||
        !(ctx.pentodes[left->compIndex] == ctx.pentodes[right->compIndex]))
        return false;

    const int lo = left->portBase;
    const int ro = right->portBase;
    const double leftGrid = ctx.blockDv[static_cast<size_t>(lo + 1)];
    const double rightGrid = ctx.blockDv[static_cast<size_t>(ro + 1)];
    if (!(leftGrid * rightGrid < 0.0))
        return false;

    bool plateLimited = false;
    for (int port : {lo, ro}) {
        const double d = ctx.blockDv[static_cast<size_t>(port)];
        const double limit = d >= 0.0 ? ctx.stepUp[static_cast<size_t>(port)]
                                      : ctx.stepDown[static_cast<size_t>(port)];
        plateLimited |= std::abs(d) > limit;
    }
    if (!plateLimited)
        return false;

    std::array<double, 3> sum{};
    std::array<double, 3> difference{};
    std::array<double, 3> direction{};
    double norm2 = 0.0;
    for (int k = 0; k < 3; ++k) {
        const double lv = ctx.blockV[static_cast<size_t>(lo + k)];
        const double rv = ctx.blockV[static_cast<size_t>(ro + k)];
        sum[static_cast<size_t>(k)] = lv + rv;
        difference[static_cast<size_t>(k)] = 0.5 * (lv - rv);
        direction[static_cast<size_t>(k)] =
            0.5 * (ctx.blockDv[static_cast<size_t>(lo + k)] -
                   ctx.blockDv[static_cast<size_t>(ro + k)]);
        norm2 += direction[static_cast<size_t>(k)] * direction[static_cast<size_t>(k)];
    }
    if (!(norm2 > 0.0) || !std::isfinite(norm2))
        return false;

    const auto residual = [&](int port, int localPort) noexcept {
        double f = ctx.blockV[static_cast<size_t>(port)] -
                   ctx.blockBase[static_cast<size_t>(port)];
        if (!networkZ.empty()) {
            const double* row = networkZ.data() + static_cast<size_t>(localPort) * n;
            for (int j = 0; j < n; ++j)
                f += row[j] * ctx.blockI[static_cast<size_t>(block.ports[static_cast<size_t>(j)])];
        } else {
            const double* row = ctx.Z.data() + static_cast<size_t>(port) * ctx.K;
            for (int p : block.ports)
                f += row[p] * ctx.blockI[static_cast<size_t>(p)];
        }
        return f;
    };
    const auto evaluate = [&](double x) noexcept {
        for (int k = 0; k < 3; ++k) {
            const double d = difference[static_cast<size_t>(k)] +
                             x * direction[static_cast<size_t>(k)];
            ctx.blockV[static_cast<size_t>(lo + k)] = 0.5 * sum[static_cast<size_t>(k)] + d;
            ctx.blockV[static_cast<size_t>(ro + k)] = 0.5 * sum[static_cast<size_t>(k)] - d;
        }
        RootResidual::currentsForDevice(
            *left, ctx.comp, ctx.ctrlResValues.data(), ctx.pentodes.data(), ctx.bjts.data(),
            ctx.opAmps.data(), ctx.opAmpVp.data(), ctx.opAmpAlpha.data(), ctx.opAmpMaxDv.data(),
            ctx.staticMode, ctx.triodeDomainPin.data(), ctx.Z.data(), ctx.K,
            ctx.blockV.data(), ctx.blockI.data(), nullptr, ctx.deviceEvals);
        RootResidual::currentsForDevice(
            *right, ctx.comp, ctx.ctrlResValues.data(), ctx.pentodes.data(), ctx.bjts.data(),
            ctx.opAmps.data(), ctx.opAmpVp.data(), ctx.opAmpAlpha.data(), ctx.opAmpMaxDv.data(),
            ctx.staticMode, ctx.triodeDomainPin.data(), ctx.Z.data(), ctx.K,
            ctx.blockV.data(), ctx.blockI.data(), nullptr, ctx.deviceEvals);
        double projected = 0.0;
        for (int k = 0; k < 3; ++k) {
            projected += direction[static_cast<size_t>(k)] *
                (residual(lo + k, leftLocal + k) - residual(ro + k, rightLocal + k));
        }
        return projected / norm2;
    };

    constexpr std::array<double, 7> probes{-4.0, -2.0, -1.0, 0.0, 1.0, 2.0, 4.0};
    std::array<double, probes.size()> values{};
    for (size_t j = 0; j < probes.size(); ++j)
        values[j] = evaluate(probes[j]);

    int bracket = -1;
    double nearest = std::numeric_limits<double>::infinity();
    for (int j = 0; j + 1 < static_cast<int>(probes.size()); ++j) {
        if (!std::isfinite(values[static_cast<size_t>(j)]) ||
            !std::isfinite(values[static_cast<size_t>(j + 1)]) ||
            std::signbit(values[static_cast<size_t>(j)]) ==
                std::signbit(values[static_cast<size_t>(j + 1)]))
            continue;
        const double distance = std::abs(0.5 * (probes[static_cast<size_t>(j)] +
                                                probes[static_cast<size_t>(j + 1)]) - 1.0);
        if (distance < nearest) {
            nearest = distance;
            bracket = j;
        }
    }
    if (bracket < 0) {
        evaluate(0.0);
        return false;
    }

    double a = probes[static_cast<size_t>(bracket)];
    double b = probes[static_cast<size_t>(bracket + 1)];
    double fa = values[static_cast<size_t>(bracket)];
    double fb = values[static_cast<size_t>(bracket + 1)];
    for (int iteration = 0; iteration < 8; ++iteration) {
        double x = (a * fb - b * fa) / (fb - fa);
        const double margin = 0.1 * (b - a);
        if (!std::isfinite(x) || x < a + margin || x > b - margin)
            x = 0.5 * (a + b);
        const double fx = evaluate(x);
        if (!std::isfinite(fx)) {
            evaluate(0.0);
            return false;
        }
        if (std::signbit(fx) == std::signbit(fa)) {
            a = x;
            fa = fx;
        } else {
            b = x;
            fb = fx;
        }
    }
    evaluate(std::abs(fa) < std::abs(fb) ? a : b);
    return true;
}

} // namespace wdf::solver
