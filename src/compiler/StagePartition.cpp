#include "compiler/StagePartition.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <tuple>

namespace livewdf::compiler {
namespace {

using DeviceSet = std::uint64_t;
struct Ports { std::vector<int> voltage, current; };

// Match the terminal laws and independent network currents. The third BJT
// voltage never enters its law, and its emitter current is already counted in
// the CE and BE return paths. A FET gate or op-amp input draws no port current.
bool portCounts(wdf::RootKind kind, int width, int& voltage, int& current) {
    using Kind = wdf::RootKind;
    switch (kind) {
        case Kind::Diode:
        case Kind::ControlledRes: voltage = current = 1; return width == 1;
        case Kind::Jfet:
        case Kind::Mosfet: voltage = 2; current = 1; return width == 2;
        case Kind::Bjt: voltage = current = 2; return width == 3;
        case Kind::OpAmp: voltage = 3; current = 1; return width == 3;
        // Conservatively keep every terminal for valves.
        case Kind::Triode:
        case Kind::Pentode: voltage = current = 3; return width == 3;
    }
    return false;
}

void addClusterCuts(std::vector<double> affinity, int n, std::set<DeviceSet>& cuts) {
    std::vector<DeviceSet> clusters(n);
    for (int i = 0; i < n; ++i)
        clusters[i] = DeviceSet{1} << i;
    const DeviceSet all = std::numeric_limits<DeviceSet>::max() >> (64 - n);
    const int minimum = std::max(2, (n + 3) / 4);
    auto add = [&](DeviceSet side) {
        const int size = std::popcount(side);
        if (size >= minimum && n - size >= minimum)
            cuts.insert(side & 1 ? side : all ^ side);
    };

    // Average-linkage clustering proposes a small set of cuts. Near the top
    // of the tree also consider joining two clusters separated by a weak load.
    for (int remaining = n; remaining > 1; --remaining) {
        if (remaining <= 8)
            for (int i = 0; i < n; ++i)
                for (int j = i + 1; j < n; ++j)
                    if (clusters[i] && clusters[j])
                        add(clusters[i] | clusters[j]);
        double strongest = -1.0;
        int a = 0, b = 0;
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j)
                if (clusters[i] && clusters[j] && affinity[i*n+j] > strongest) {
                    strongest = affinity[i*n+j];
                    a = i; b = j;
                }
        const int na = std::popcount(clusters[a]), nb = std::popcount(clusters[b]);
        const double weight = static_cast<double>(na) / (na + nb);
        for (int i = 0; i < n; ++i)
            if (clusters[i])
                affinity[i*n+a] = affinity[a*n+i] =
                    std::lerp(affinity[i*n+b], affinity[i*n+a], weight);
        clusters[a] |= clusters[b];
        clusters[b] = 0;
        add(clusters[a]);
    }
}

bool factor(StageCoupling& c, std::span<const double> z, int k) {
    const int m = static_cast<int>(c.voltagePorts.size()), n = static_cast<int>(c.currentPorts.size());
    std::vector<double> residual;
    residual.reserve(m * n);
    for (int row : c.voltagePorts)
        for (int col : c.currentPorts)
            residual.push_back(z[row*k+col]);
    auto largest = [&] {
        return std::max_element(residual.begin(), residual.end(),
            [](double a, double b) { return std::abs(a) < std::abs(b); });
    };
    auto pivot = largest();
    const double tolerance = 32 * std::numeric_limits<double>::epsilon() *
                             std::max(m, n) * std::abs(*pivot);
    std::vector<double> columns;
    while (std::abs(*pivot) > tolerance) {
        if (c.rank == 2)
            return false;
        const int pos = static_cast<int>(pivot - residual.begin());
        const int row = pos / n, col = pos % n;
        const double p = *pivot;
        for (int j = 0; j < n; ++j)
            c.rightFactor.push_back(residual[row*n+j] / p);
        for (int i = 0; i < m; ++i) {
            const double u = residual[i*n+col];
            columns.push_back(u);
            for (int j = 0; j < n; ++j)
                residual[i*n+j] -= u * c.rightFactor[c.rank*n+j];
        }
        ++c.rank;
        pivot = largest();
    }
    c.leftFactor.resize(m * c.rank);
    for (int i = 0; i < m; ++i)
        for (int r = 0; r < c.rank; ++r)
            c.leftFactor[i*c.rank+r] = columns[r*m+i];
    // Check the factors in exactly the layout exported to the caller.
    for (int i = 0; i < m; ++i)
        for (int j = 0; j < n; ++j) {
            double sum = 0.0;
            for (int r = 0; r < c.rank; ++r)
                sum += c.leftFactor[i*c.rank+r] * c.rightFactor[r*n+j];
            const double error = std::abs(sum - z[c.voltagePorts[i]*k+c.currentPorts[j]]);
            if (!(error <= tolerance))
                return false;
            c.maxError = std::max(c.maxError, error);
        }
    return true;
}

}  // namespace

std::optional<StagePartition> discoverStagePartition(const wdf::RootLayout& layout,
                                                     std::span<const double> impedance,
                                                     double sampleRate, long long linearRevision) {
    if (layout.devices.size() < 4 || layout.devices.size() > 64 ||
        !(std::isfinite(sampleRate) && sampleRate > 0.0) || linearRevision < 0)
        return std::nullopt;
    const int n = static_cast<int>(layout.devices.size()), k = layout.K;
    if (k < n || k > 3*n || impedance.size() != static_cast<size_t>(k)*k ||
        !std::all_of(impedance.begin(), impedance.end(), [](double x) { return std::isfinite(x); }))
        return std::nullopt;
    std::vector<Ports> ports(n);
    int next = 0;
    for (int d = 0; d < n; ++d) {
        const auto& device = layout.devices[d];
        int v, i;
        if (device.portBase != next || !portCounts(device.kind, device.numPorts, v, i))
            return std::nullopt;
        for (int j = 0; j < v; ++j) ports[d].voltage.push_back(next+j);
        for (int j = 0; j < i; ++j) ports[d].current.push_back(next+j);
        next += device.numPorts;
    }
    if (next != k)
        return std::nullopt;
    std::vector<double> affinity(n*n);
    for (int d = 0; d < n; ++d)
        for (int e = 0; e < n; ++e)
            for (int v : ports[d].voltage)
                for (int i : ports[e].current)
                    affinity[d*n+e] = affinity[e*n+d] =
                        std::max(affinity[d*n+e], std::abs(impedance[v*k+i]));
    std::set<DeviceSet> cuts;
    addClusterCuts(affinity, n, cuts);
    // Both linear and logarithmic scales matter when megohm and ohm paths
    // share a circuit. Rank is always evaluated on the original coefficients.
    for (auto& a : affinity) a = std::log1p(a);
    addClusterCuts(std::move(affinity), n, cuts);

    std::optional<StagePartition> best;
    std::tuple<int,int,int,int,DeviceSet> bestScore;
    for (DeviceSet cut : cuts) {
        StagePartition plan;
        for (int d = 0; d < n; ++d) {
            const int s = (cut & (DeviceSet{1} << d)) ? 0 : 1;
            plan.devices[s].push_back(d);
            auto& rows = plan.toStage[s].voltagePorts;
            auto& cols = plan.toStage[1-s].currentPorts;
            rows.insert(rows.end(), ports[d].voltage.begin(), ports[d].voltage.end());
            cols.insert(cols.end(), ports[d].current.begin(), ports[d].current.end());
        }
        if (!factor(plan.toStage[0], impedance, k) || !factor(plan.toStage[1], impedance, k))
            continue;
        int saved = 0;
        for (const auto& c : plan.toStage)
            saved += static_cast<int>(c.voltagePorts.size()*c.currentPorts.size()) -
                     static_cast<int>(c.leftFactor.size()+c.rightFactor.size());
        if (saved <= 0)
            continue;
        const int a = plan.toStage[0].rank, b = plan.toStage[1].rank;
        const auto score = std::tuple{std::max(a,b), a+b, std::abs(n-2*std::popcount(cut)), -saved, cut};
        if (!best || score < bestScore) {
            bestScore = score;
            plan.sampleRate = sampleRate;
            plan.linearRevision = linearRevision;
            best = std::move(plan);
        }
    }
    return best;
}

}  // namespace livewdf::compiler
