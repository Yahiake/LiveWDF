#pragma once

#include "wdf/LoadedJunctionPlan.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>

namespace wdf::engine {

// Each changed resistor adds at most one to the rank of the root impedance
// update. Four covers both legs of a dual-gang pot. Refresh the maps that load the
// nonlinear junctions. An unsupported rank or ill-conditioned update declines
// the plan, and the caller uses the normal solver.
inline bool updateJunctionLoading(LoadedJunctionPlan& plan,
                                  std::span<const double> impedance,
                                  std::span<const int> independentRows) noexcept {
    constexpr int maxPorts = LoadedJunctionPlan::maxPorts;
    constexpr int maxRank = 4;
    const int ports = static_cast<int>(independentRows.size());
    const int laws = static_cast<int>(plan.laws.size());
    const int poles = static_cast<int>(plan.linearPoles.size());
    if (ports < 1 || ports > maxPorts || laws < 1 || laws > maxPorts || poles > maxPorts ||
        impedance.size() != static_cast<size_t>(ports * ports) ||
        impedance.size() != plan.impedance.size())
        return false;

    // Reuse the saved impedance as scratch. The caller discards a declined
    // plan, so partially transformed maps cannot be used for a later frame.
    auto& difference = plan.impedance;
    double scale = 1.0;
    for (size_t i = 0; i < impedance.size(); ++i) {
        if (!std::isfinite(impedance[i])) return false;
        difference[i] = impedance[i] - difference[i];
        scale = std::max(scale, std::abs(impedance[i]));
    }
    const double rounding = scale * 128 * std::numeric_limits<double>::epsilon();
    std::array<double, maxPorts> u, v, h, jacobian, projection;
    for (int rank = 0;; ++rank) {
        size_t pivot = 0;
        for (size_t i = 1; i < difference.size(); ++i)
            if (std::abs(difference[i]) > std::abs(difference[pivot])) pivot = i;
        if (std::abs(difference[pivot]) <= rounding) break;
        if (rank == maxRank) return false;

        const int row = static_cast<int>(pivot / ports);
        const int col = static_cast<int>(pivot % ports);
        for (int i = 0; i < ports; ++i) {
            u[i] = difference[i * ports + col];
            v[i] = difference[row * ports + i] / difference[pivot];
        }
        for (int i = 0; i < ports; ++i)
            for (int j = 0; j < ports; ++j)
                difference[i * ports + j] -= u[i] * v[j];

        // Znew = Zold + u vᵀ. For vterm = openMap * voc - openMap * Z * i,
        // the Sherman-Morrison denominator is 1 + vᵀ J openMap u.
        std::fill_n(jacobian.data(), ports, 0.0);
        for (const auto& e : plan.linear)
            if (independentRows[e.row] >= 0) jacobian[e.col] += v[e.row] * e.value;
        double denom = 1.0, denomScale = 1.0;
        for (int i = 0; i < ports; ++i) {
            h[i] = 0.0;
            for (int j = 0; j < ports; ++j)
                h[i] += plan.openMap[i * ports + j] * u[j];
            denom += jacobian[i] * h[i];
            denomScale += std::abs(jacobian[i] * h[i]);
        }
        if (!std::isfinite(denom) || std::abs(denom) <= 1e-9 * denomScale)
            return false;
        for (int i = 0; i < ports; ++i) h[i] /= denom;

        auto apply = [&](std::vector<double>& map, int columns) {
            for (int i = 0; i < ports; ++i) {
                const double weight = jacobian[i];
                if (weight == 0.0) continue;
                for (int j = 0; j < columns; ++j)
                    projection[j] += weight * map[i * columns + j];
            }
            for (int i = 0; i < ports; ++i) {
                if (h[i] == 0.0) continue;
                for (int j = 0; j < columns; ++j)
                    map[i * columns + j] -= h[i] * projection[j];
            }
        };
        std::fill_n(projection.data(), ports, 0.0);
        apply(plan.openMap, ports);
        std::fill_n(projection.data(), laws, 0.0);
        for (const auto& e : plan.injection)
            if (independentRows[e.row] >= 0) projection[e.col] += v[e.row] * e.value;
        apply(plan.voltageMap, laws);
        projection[0] = 0.0;
        for (int i = 0; i < ports; ++i)
            if (independentRows[i] >= 0) projection[0] += v[i] * plan.constant[i];
        apply(plan.offset, 1);
        const int affineRows = static_cast<int>(plan.affineRows.size());
        // affineMap has the opposite sign to maps of injected currents.
        for (int j = 0; j < affineRows; ++j)
            projection[j] = -v[plan.affineRows[j]];
        apply(plan.affineMap, affineRows);
    }

    for (int i = 0; i < laws; ++i) {
        auto& law = plan.laws[i];
        double* row = plan.mutual.data() + i * laws;
        for (int j = 0; j < laws; ++j) {
            const int neg = law.sense.negativePort;
            row[j] = law.sense.gain * (plan.voltageMap[law.sense.port * laws + j] -
                                       (neg >= 0 ? plan.voltageMap[neg * laws + j] : 0.0));
            if (!std::isfinite(row[j])) return false;
        }
        const double resistance = -row[i];
        if (!(resistance >= 0.0)) return false;
        if (law.fet >= 0 || law.photo >= 0 || law.pole >= 0) {
            law.resistance = resistance;
            row[i] = 0.0;
        } else {
            // An existing loaded table solves q = f(drive - Rtable*q).
            // Feed (Rtable - Rnew)*q through its coupled-Newton diagonal.
            row[i] += law.resistance;
        }
    }
    plan.packAffineColumns(ports);
    std::copy(impedance.begin(), impedance.end(), difference.begin());
    return true;
}

} // namespace wdf::engine
