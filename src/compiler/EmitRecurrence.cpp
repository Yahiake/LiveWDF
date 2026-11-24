#include "compiler/EmitRecurrence.h"
#include "wdf/Engine.h"

#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace livewdf::compiler {
namespace {

// Coefficients multiply [1, reactive waves, source voltages, root currents].
using Row = std::vector<double>;
void add(Row& target, const Row& term, double gain) {
    for (size_t i = 0; i < term.size(); ++i)
        target[i] += gain * term[i];
}

struct Names {
    int states, sources;
    std::string operator()(int k) const {
        if (k == 0) return {};
        if (--k < states) return "state[" + std::to_string(k) + "]";
        if ((k -= states) < sources) return "source[" + std::to_string(k) + "]";
        return "current[" + std::to_string(k - sources) + "]";
    }
};

void expression(std::ostream& out, const Row& row, Names names) {
    bool first = true;
    for (size_t i = 0; i < row.size(); ++i) {
        double v = row[i];
        if (v == 0.0) continue;
        if (!std::isfinite(v)) throw std::invalid_argument("non-finite recurrence coefficient");
        out << (first ? (v < 0 ? "-" : "") : (v < 0 ? " - " : " + "));
        if (i == 0 || std::abs(v) != 1) {
            out << std::abs(v);
            if (i != 0) out << " * ";
        }
        out << names(static_cast<int>(i));
        first = false;
    }
    if (first) out << "0.0";
}

void literals(std::ostream& out, const std::vector<double>& values) {
    out << '{';
    for (size_t i = 0; i < values.size(); ++i) {
        if (!std::isfinite(values[i])) throw std::invalid_argument("non-finite initial state or transfer");
        out << (i ? ", " : "") << values[i];
    }
    out << '}';
}
} // namespace

void emitRecurrence(std::ostream& output, const wdf::Engine& e, int inputSource,
                    const std::string& outputNet, bool externalRoots) {
    const int ns = e.sourceCount(), nx = e.capCount() + e.inductorCount(), k = e.rootCount();
    if (inputSource < 0 || inputSource >= ns)
        throw std::invalid_argument("audio input must be a voltage source");
    if (e.currentSourceCount() || !e.compiled().photoSensePorts.empty() ||
        !e.compiled().vcaSensePorts.empty() || !e.compiled().otaSensePorts.empty() ||
        !e.compiled().dbx200BiasPorts.empty() || e.hasAdjustedLiveTransfer())
        throw std::invalid_argument("time-varying current sources and live control transfers are unsupported");

    std::vector<double> biasNodes, biasPorts;
    if (!e.solveDc(biasNodes, biasPorts) || biasNodes.size() != static_cast<size_t>(e.nodeCount()) ||
        biasPorts.size() != static_cast<size_t>(k))
        throw std::invalid_argument("could not establish a DC operating point");
    const bool scalarDiode = k == 1 && e.compiled().diode.size() == 1;
    if (k && !scalarDiode && !externalRoots)
        throw std::invalid_argument(std::to_string(k) + " coupled root ports require --external-roots");
    const auto outputInfo = e.compiled().info.find(outputNet);
    if (outputInfo == e.compiled().info.end()) throw std::invalid_argument("unknown output net");

    const int sourceBase = 1 + nx, currentBase = sourceBase + ns;
    const Row zero(currentBase + k, 0.);
    const auto offset = [&](const wdf::AffineOffset& a) {
        Row row = zero;
        row[0] = a.c0;
        for (auto [source, factor] : a.coef) row[sourceBase + source] += factor;
        return row;
    };
    const auto stateIndex = [&](int p) {
        return e.capOf()[p] >= 0 ? e.capOf()[p] : e.capCount() + e.indOf()[p];
    };

    // Compose the existing open-circuit transfer with its sparse wave/source map.
    std::vector<Row> nodes(e.nodeCount(), zero);
    const auto& live = e.liveElementPorts();
    for (size_t j = 0; j < live.size(); ++j) {
        int p = live[j];
        Row wave = zero;
        add(wave, offset(e.elementOffsets()[p]), -1.);
        if (e.capOf()[p] >= 0 || e.indOf()[p] >= 0)
            wave[1 + stateIndex(p)] = e.capOf()[p] >= 0 ? 1. : -1.;
        for (size_t n = 0; n < nodes.size(); ++n)
            add(nodes[n], wave, e.Xe()[n * live.size() + j]);
    }
    auto across = [&](int a, int b, Row row) {
        if (a >= 0) add(row, nodes.at(a), 1.);
        if (b >= 0) add(row, nodes.at(b), -1.);
        return row;
    };
    std::vector<Row> open;
    for (int r = 0; r < k; ++r) {
        const auto& port = e.rootPorts()[r];
        open.push_back(across(port[0], port[1], offset(e.rootOffsets()[r])));
    }

    for (size_t n = 0; n < nodes.size(); ++n)
        for (int c : e.rootLayout().nodeCol)
            nodes[n][currentBase + c] = -e.Xr()[n * k + c];
    std::vector<Row> next(nx, zero);
    for (int p = 0; p < e.elementCount(); ++p) {
        if (e.capOf()[p] < 0 && e.indOf()[p] < 0) continue;
        int j = stateIndex(p);
        next[j][1+j] = e.capOf()[p] >= 0 ? -1. : 1.;
        add(next[j], across(e.elementPortA()[p], e.elementPortB()[p], offset(e.elementOffsets()[p])), 2.);
    }

    auto& [representative, potential] = outputInfo->second;
    Row outRow = representative.empty() ? zero : nodes.at(e.nodeIndex(representative));
    outRow[0] += potential[0];
    for (int j = 0; j < ns; ++j) outRow[sourceBase+j] += potential[1+j];

    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    out << "// Generated for fixed component values at " << e.fs() << " Hz.\n"
        << "#pragma once\n#include <array>\n#include <cmath>\n#include <cstddef>\n#include <optional>\n";
    if (scalarDiode) out << "#include \"devices/Diode.h\"\n";
    out << "\nstruct CircuitStep {\n"
        << "    using Ports = std::array<double, " << k << ">;\n"
        << "    using Sources = std::array<double, " << ns << ">;\n"
        << "    using State = std::array<double, " << nx << ">;\n"
        << "    struct Solution { Ports voltage{}, current{}; };\n"
        << "    static constexpr double sampleRate = " << e.fs() << ";\n"
        << "    static constexpr unsigned long long linearRevision = " << e.linearRebuilds() << ";\n"
        << "    static constexpr std::size_t inputIndex = " << inputSource << ";\n"
        << "    static constexpr Sources dcSources = ";
    literals(out, e.dcValues());
    out << ";\n    static constexpr Ports initialCurrent = ";
    literals(out, e.rootCurrents());
    out << ";\n    static constexpr std::array<Ports, " << k << "> impedance = {{\n";
    for (int r = 0; r < k; ++r) {
        Row z(k, 0.);
        for (int c : e.rootLayout().redCol) z[c] = e.rootImpedance()[r*k+c];
        out << "        "; literals(out, z); out << ",\n";
    }
    out << "    }};\n    State state = ";
    Row initial = e.capacitorStates();
    initial.insert(initial.end(), e.inductorStates().begin(), e.inductorStates().end());
    literals(out, initial);
    out << ";\n\n    Ports openCircuit([[maybe_unused]] const Sources& source) const {\n        return {\n";
    const Names names{nx, ns};
    for (const auto& row : open) {
        out << "            "; expression(out, row, names); out << ",\n";
    }
    out << "        };\n    }\n\n"
        << "    double advance([[maybe_unused]] const Sources& source, [[maybe_unused]] const Ports& current) {\n"
        << "        const double result = ";
    expression(out, outRow, names);
    out << ";\n        state = {\n";
    for (const auto& row : next) {
        out << "            "; expression(out, row, names); out << ",\n";
    }
    out << "        };\n        return result;\n    }\n";
    if (k == 0) {
        out << "    double step(const Sources& source) { return advance(source, {}); }\n";
    } else if (scalarDiode) {
        const auto& d = e.compiled().diode.front().second;
        const double r = d.rs + e.rootImpedance()[0];
        if (!(r > 0 && std::isfinite(r))) throw std::invalid_argument("invalid loaded diode impedance");
        out << "    const wdf::DiodeModel loadedDiode{" << d.isat << ", " << d.n << ", "
            << r << ", " << d.rp << ", " << d.bv << ", " << d.ibv << "};\n"
            << "    double step(const Sources& source) {\n"
            << "        double i, slope;\n        loadedDiode.eval(openCircuit(source)[0], i, slope);\n"
            << "        return advance(source, {i});\n    }\n";
    }
    if (externalRoots && k) {
        out << R"(
    // The callback supplies the device law and its histories. Validate the
    // loaded network equation before committing this network's reactive state.
    template<class Solve>
    std::optional<double> step(const Sources& source, Solve&& solve, double tolerance = 1e-7) {
        const Ports open = openCircuit(source);
        Solution solution{};
        if (!(tolerance >= 0.) || !std::isfinite(tolerance) || !solve(open, solution)) return {};
        for (std::size_t r = 0; r < open.size(); ++r) {
            double residual = solution.voltage[r] - open[r];
            for (std::size_t c = 0; c < open.size(); ++c)
                residual += impedance[r][c] * solution.current[c];
            if (!(std::abs(residual) <= tolerance)) return {};
        }
        CircuitStep trial = *this;
        const double sample = trial.advance(source, solution.current);
        if (!std::isfinite(sample)) return {};
        for (double x : trial.state)
            if (!std::isfinite(x)) return {};
        state = trial.state;
        return sample;
    }
)";
    }
    out << "};\n";
    output << out.str();
}
} // namespace livewdf::compiler
