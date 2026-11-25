#include "compiler/EmitDelayed.h"

#include "compiler/StagePartition.h"
#include "wdf/Engine.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <numeric>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace livewdf::compiler {
namespace {
using Indices = std::vector<int>;
using wdf::RootLayout;
using wdf::Engine;

Indices portsOf(const RootLayout& layout, const Indices& devices) {
    Indices ports;
    for (const int device : devices) {
        const auto& d = layout.devices.at(device);
        for (int port = d.portBase; port < d.portBase + d.numPorts; ++port)
            ports.push_back(port);
    }
    std::sort(ports.begin(), ports.end());
    return ports;
}

void emitIndices(std::ostream& out, std::string_view name, const Indices& ports) {
    out << "    static constexpr std::array<std::size_t, " << ports.size() << "> " << name << "{";
    for (const int p : ports)
        out << p << ", ";
    out << "};\n";
}

// The ranked map covers device-law input rows. Reconstruct other physical
// rows (including the BJT emitter report) from the original transfer.
void emitCross(std::ostream& out, const StageCoupling& map, const Indices& target,
               const Indices& source, std::string_view name, const Engine& engine) {
    const auto& z = engine.rootImpedance();
    const auto& layout = engine.rootLayout();
    out << "    static void " << name << "(Ports& drive, const Ports& current) {\n";
    for (int axis = 0; axis < map.rank; ++axis) {
        out << "        const double boundary" << axis << " = 0.";
        for (size_t c = 0; c < map.currentPorts.size(); ++c) {
            double v = map.rightFactor.at(axis * map.currentPorts.size() + c);
            if (v != 0.)
                out << " + (" << v << ") * current[" << map.currentPorts[c] << "]";
        }
        out << ";\n";
    }
    for (const int r : target) {
        out << "        drive[" << r << "] -= 0.";
        auto it = std::find(map.voltagePorts.begin(), map.voltagePorts.end(), r);
        if (it != map.voltagePorts.end()) {
            const auto local = size_t(it - map.voltagePorts.begin());
            for (int axis = 0; axis < map.rank; ++axis) {
                double v = map.leftFactor.at(local * map.rank + axis);
                if (v != 0.) out << " + (" << v << ") * boundary" << axis;
            }
        } else {
            for (const int c : source) {
                if (layout.redRow[c] < 0) continue;
                double v = z[size_t(r) * layout.K + c];
                if (v != 0.) out << " + (" << v << ") * current[" << c << "]";
            }
        }
        out << ";\n";
    }
    out << "    }\n";
}
} // namespace

void emitDelayed(std::ostream& output, const wdf::Engine& engine, double sampleRate, bool reverse) {
    if (sampleRate != engine.fs())
        throw std::invalid_argument("stage partition sample rate differs from the compiled engine");
    const auto& layout = engine.rootLayout();
    const auto partition = livewdf::compiler::discoverStagePartition(
        layout, engine.rootImpedance(), sampleRate, engine.linearRebuilds());
    if (!partition)
        throw std::invalid_argument("no supported two-stage feedback partition for these roots");
    const unsigned first = reverse ? 1 : 0, second = 1 - first;
    const auto a = portsOf(layout, partition->devices[first]), b = portsOf(layout, partition->devices[second]);
    if (a.empty() || b.empty() || a.size() + b.size() != size_t(layout.K))
        throw std::logic_error("incomplete two-stage root partition");

    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    out << R"(
// First solves with the preceding internal sample's cross-stage current.
// Second receives the current sample's first-stage solution.
struct DelayedCircuitStep {
    using Ports = CircuitStep::Ports;
    using Sources = CircuitStep::Sources;
    using Solution = CircuitStep::Solution;
    template<std::size_t N> struct StageSolution { std::array<double, N> voltage, current; };
)";
    emitIndices(out, "firstPorts", a);
    emitIndices(out, "secondPorts", b);
    Indices all(layout.K); std::iota(all.begin(), all.end(), 0);
    emitIndices(out, "allPorts", all);

    out << R"(
    const CircuitStep& circuit() const noexcept { return network_; }
    const Ports& previousCurrent() const noexcept { return previousCurrent_; }

    // Solve and validate are trial evaluations. External device histories
    // must only be committed by the caller after the complete step succeeds.
    // solve(ids, localZ, drive, voltageSeed) returns optional<StageSolution<N>>.
    // validate(voltage, current) checks all original terminal current laws.
    template<class Solve, class Validate>
    std::optional<double> step(const Sources& source, Solve&& solve, Validate&& validate, double tolerance = 1e-7) {
        if (!(tolerance >= 0.) || !std::isfinite(tolerance)) return {};
        const Ports open = network_.openCircuit(source);
        Ports drive = open;
        Solution solution{};
        driveFirst(drive, previousCurrent_);
        if (!solvePart<firstPorts>(drive, solution, solve)) return {};
        driveSecond(drive, solution.current);
        if (!solvePart<secondPorts>(drive, solution, solve)) return {};
        return finish(source, open, solution, validate, tolerance, true);
    }

    // An explicitly requested simultaneous solve also seeds feedback for a
    // subsequent delayed sample at the same internal rate.
    template<class Solve, class Validate>
    std::optional<double> stepExact(const Sources& source, Solve&& solve, Validate&& validate, double tolerance = 1e-7) {
        if (!(tolerance >= 0.) || !std::isfinite(tolerance)) return {};
        const Ports open = network_.openCircuit(source);
        Solution solution{};
        if (!solvePart<allPorts>(open, solution, solve)) return {};
        return finish(source, open, solution, validate, tolerance, false);
    }

private:
    CircuitStep network_{};
    Ports previousCurrent_ = CircuitStep::initialCurrent;
)";
    emitCross(out, partition->toStage[first], a, b, "driveFirst", engine);
    emitCross(out, partition->toStage[second], b, a, "driveSecond", engine);
    out << R"(
    template<const auto& ids, class Solve>
    bool solvePart(const Ports& drive, Solution& solution, Solve& solve) const {
        constexpr std::size_t n = ids.size();
        static constexpr auto localZ = [] {
            std::array<std::array<double, n>, n> z{};
            for (std::size_t r = 0; r < n; ++r)
                for (std::size_t c = 0; c < n; ++c)
                    z[r][c] = CircuitStep::impedance[ids[r]][ids[c]];
            return z;
        }();
        std::array<double, n> localDrive{}, seed{};
        for (std::size_t r = 0; r < n; ++r) {
            localDrive[r] = drive[ids[r]];
            seed[r] = localDrive[r];
            for (std::size_t c = 0; c < n; ++c)
                seed[r] -= localZ[r][c] * previousCurrent_[ids[c]];
            if (!std::isfinite(seed[r]) || !std::isfinite(localDrive[r])) return false;
        }
        const auto result = solve(ids, localZ, localDrive, seed);
        if (!result) return false;
        for (std::size_t r = 0; r < n; ++r) {
            if (!std::isfinite(result->voltage[r]) || !std::isfinite(result->current[r])) return false;
            solution.voltage[ids[r]] = result->voltage[r];
            solution.current[ids[r]] = result->current[r];
        }
        return true;
    }

    template<class Validate>
    std::optional<double> finish(const Sources& source, const Ports& open, const Solution& solution,
                                 Validate& validate, double tolerance, bool delayed) {
        static constexpr auto first = [] {
            std::array<bool, allPorts.size()> mask{};
            for (auto p : firstPorts) mask[p] = true;
            return mask;
        }();
        for (std::size_t r = 0; r < open.size(); ++r) {
            double residual = solution.voltage[r] - open[r];
            for (std::size_t c = 0; c < open.size(); ++c) {
                const bool lag = delayed && first[r] && !first[c];
                residual += CircuitStep::impedance[r][c] *
                            (lag ? previousCurrent_[c] : solution.current[c]);
            }
            if (!(std::abs(residual) <= tolerance)) return {};
        }
        if (!validate(solution.voltage, solution.current)) return {};
        auto next = network_;
        const double y = next.advance(source, solution.current);
        if (!std::isfinite(y)) return {};
        for (double x : next.state) if (!std::isfinite(x)) return {};
        network_ = next;
        previousCurrent_ = solution.current;
        return y;
    }
};
)";
    output << out.str();
}
} // namespace livewdf::compiler
