#pragma once

#include "circuit/Network.h"
#include "wdf/DenseSolve.h"

namespace wdf::engine {

/// Resistor-only changes add B D Bᵀ to the nodal matrix Y. Compile U = Y⁻¹ B
/// and H = Bᵀ U once. At a control update, factor I + D H, where D contains
/// the changed conductances. This also accounts for the changed source stamp
/// of a resistor connected to an ideal voltage source.
class AdjustableTransfer {
public:
    void prepare(const Network& net, const Dense& elementTransfer, int nodes,
                 const std::vector<double>& rootTransfer, const std::vector<double>& impedance,
                 const std::vector<int>& resistors) {
        *this = {};
        if (resistors.empty() || nodes == 0)
            return;
        nodes_ = nodes;
        roots_ = static_cast<int>(net.rp.size());
        baselineZ_ = impedance;
        for (const int index : resistors) {
            if (!(net.eG[index] > 0.0) || !std::isfinite(net.eG[index])) {
                ports_.clear();
                return;
            }
            ports_.push_back({index, net.epA[index], net.epB[index], net.eG[index]});
        }

        const size_t count = ports_.size();
        response_.assign(count * nodes_, 0.0);
        interaction_.assign(count * count, 0.0);
        rootResponse_.assign(count * roots_, 0.0);
        rootInput_.assign(count * roots_, 0.0);
        for (size_t k = 0; k < count; ++k) {
            const auto& p = ports_[k];
            double* u = response_.data() + k * nodes_;
            for (int n = 0; n < nodes_; ++n)
                u[n] = elementTransfer[n][p.index] / p.conductance;
            for (size_t j = 0; j < count; ++j)
                interaction_[j * count + k] = across(ports_[j].a, ports_[j].b, u);
            for (int r = 0; r < roots_; ++r) {
                const auto& port = net.rp[r];
                rootResponse_[k * roots_ + r] = across(port[0], port[1], u);
                rootInput_[k * roots_ + r] = (p.a < 0 ? 0.0 : rootTransfer[p.a * roots_ + r]) -
                                           (p.b < 0 ? 0.0 : rootTransfer[p.b * roots_ + r]);
            }
        }
        active_.reserve(count);
        delta_.resize(count);
        lu_.resize(count * count);
        pivots_.resize(count);
        rhs_.resize(count);
        rootCorrection_.resize(count * roots_);
        candidateZ_.resize(baselineZ_.size());
    }

    bool ready() const noexcept { return !ports_.empty(); }
    bool active() const noexcept { return !active_.empty(); }

    bool update(const std::vector<double>& conductances, std::vector<double>& impedance) noexcept {
        active_.clear();
        for (size_t i = 0; i < ports_.size(); ++i) {
            const auto& port = ports_[i];
            if (port.a == port.b)
                continue;
            if (!std::isfinite(conductances[port.index]))
                return false;
            const double d = conductances[port.index] - port.conductance;
            if (d != 0.0) {
                delta_[active_.size()] = d;
                active_.push_back(i);
            }
        }

        const int m = static_cast<int>(active_.size());
        const size_t count = ports_.size();
        for (int i = 0; i < m; ++i)
            for (int j = 0; j < m; ++j)
                lu_[i * m + j] = (i == j ? 1.0 : 0.0) +
                    delta_[i] * interaction_[active_[i] * count + active_[j]];
        if (m != 0 && !detail::factorLUData(lu_.data(), pivots_.data(), m))
            return false;

        for (int c = 0; c < roots_ && m != 0; ++c) {
            for (int i = 0; i < m; ++i)
                rhs_[i] = delta_[i] * rootInput_[active_[i] * roots_ + c];
            detail::solveLUData(lu_.data(), pivots_.data(), rhs_.data(), m);
            for (int i = 0; i < m; ++i) {
                if (!std::isfinite(rhs_[i]))
                    return false;
                rootCorrection_[i * roots_ + c] = rhs_[i];
            }
        }
        std::copy(baselineZ_.begin(), baselineZ_.end(), candidateZ_.begin());
        for (int r = 0; r < roots_; ++r)
            for (int i = 0; i < m; ++i) {
                const double z = rootResponse_[active_[i] * roots_ + r];
                if (z == 0.0)
                    continue;
                for (int c = 0; c < roots_; ++c)
                    candidateZ_[r * roots_ + c] -= z * rootCorrection_[i * roots_ + c];
            }
        if (!std::all_of(candidateZ_.begin(), candidateZ_.end(), [](double value) { return std::isfinite(value); }))
            return false;
        std::copy(candidateZ_.begin(), candidateZ_.end(), impedance.begin());
        return true;
    }

    void correctOpenCircuit(const double* offsets, double* nodes) noexcept {
        const int m = static_cast<int>(active_.size());
        for (int i = 0; i < m; ++i) {
            const auto& p = ports_[active_[i]];
            rhs_[i] = -delta_[i] * (offsets[p.index] + across(p.a, p.b, nodes));
        }
        detail::solveLUData(lu_.data(), pivots_.data(), rhs_.data(), m);
        addResponse(nodes);
    }

    void correctLoadedNodes(const double* currents, double* nodes) noexcept {
        for (size_t i = 0; i < active_.size(); ++i) {
            const double* root = rootCorrection_.data() + i * roots_;
            double sum = 0.0;
            for (int r = 0; r < roots_; ++r)
                sum += root[r] * currents[r];
            rhs_[i] = sum;
        }
        addResponse(nodes);
    }

private:
    struct Port { int index, a, b; double conductance; };

    static double across(int a, int b, const double* nodes) noexcept {
        return (a < 0 ? 0.0 : nodes[a]) - (b < 0 ? 0.0 : nodes[b]);
    }

    void addResponse(double* nodes) const noexcept {
        for (size_t i = 0; i < active_.size(); ++i) {
            const double* u = response_.data() + active_[i] * nodes_;
            for (int n = 0; n < nodes_; ++n)
                nodes[n] += u[n] * rhs_[i];
        }
    }

    int nodes_ = 0, roots_ = 0;
    std::vector<Port> ports_;
    std::vector<size_t> active_;
    std::vector<int> pivots_;
    std::vector<double> response_, interaction_, rootResponse_, rootInput_, baselineZ_;
    std::vector<double> delta_, lu_, rhs_, rootCorrection_, candidateZ_;
};

} // namespace wdf::engine
