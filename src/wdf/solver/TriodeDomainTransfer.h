#pragma once

#include "wdf/DenseTopology.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace wdf::solver {

// Tube-domain selection needs the loaded plate and internal-grid voltage for
// each triode. Pack these independent voltage rows while preserving the block
// traversal's current order and the original scalar summation order per port.
class TriodeDomainTransfer {
public:
    void prepare(const std::vector<double>& z, int ports, const std::vector<RootDevice>& devices) {
        *this = {};
        const auto triodes = std::count_if(devices.begin(), devices.end(),
            [](const RootDevice& device) { return device.kind == DeviceKind::Triode; });
        if (triodes < 3) return;
        rows_.resize(size_t(triodes) * 2);
        for (const RootDevice& device : devices) {
            if (device.kind != DeviceKind::Triode) continue;
            rows_[2 * device.compIndex] = device.portBase;
            rows_[2 * device.compIndex + 1] = device.portBase + 2;
        }
        ports_ = ports;
        const size_t paddedRows = (rows_.size() + lanes - 1) / lanes * lanes;
        coefficients_.resize(paddedRows * ports_);
        refresh(z, ports);
    }

    void refresh(const std::vector<double>& z, int ports) noexcept {
        valid_ = !rows_.empty() && ports == ports_ && z.size() == size_t(ports) * ports;
        if (!valid_) return;
        for (size_t r = 0; r < rows_.size(); ++r)
            for (int j = 0; j < ports; ++j)
                coefficients_[(r / lanes * ports + j) * lanes + r % lanes] =
                    z[size_t(rows_[r]) * ports + j];
    }

    bool fold(const BlockView& view, const double* open, const double* currents, double* out) const noexcept {
        if (!valid_) return false;
        for (int b = 0; b < view.count; ++b)
            for (int col : (*view.blocks)[(*view.order)[b]].ports)
                if (!std::isfinite(currents[col])) return false;

        for (size_t r = 0; r < rows_.size(); r += lanes) {
            double sum[lanes]{};
            const size_t count = std::min(lanes, rows_.size() - r);
            for (size_t l = 0; l < count; ++l) sum[l] = open[rows_[r + l]];
            const double* block = coefficients_.data() + r * ports_;
            for (int b = 0; b < view.count; ++b) {
                for (int col : (*view.blocks)[(*view.order)[b]].ports) {
                    const double current = currents[col];
                    if (current == 0.) continue;
                    for (size_t l = 0; l < lanes; ++l) sum[l] -= block[col * lanes + l] * current;
                }
            }
            for (size_t l = 0; l < count; ++l) out[r + l] = sum[l];
        }
        return true;
    }

private:
    static constexpr size_t lanes = 8;
    int ports_ = 0;
    bool valid_ = false;
    std::vector<int> rows_;
    std::vector<double> coefficients_;
};

} // namespace wdf::solver
