#pragma once

#include "wdf/DenseSolve.h"
#include "wdf/DenseTopology.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace wdf::solver {

// Parallel ports can inject exactly the same (or opposite) column into Z.
// If Z = C S, solve for the summed currents S u, then dv = -F - C (S u).
// This preserves the device laws and the full-network Newton residual.
class SharedNewtonColumns {
public:
    void prepare(const std::vector<double>& z, int rows, int cols) {
        *this = {};
        if (cols <= 8 || z.size() != size_t(rows) * cols)
            return;
        for (int j = 0; j < cols; ++j) {
            auto matches = [&](int column, int sign) {
                for (int i = 0; i < rows; ++i) {
                    const double* row = z.data() + size_t(i) * cols;
                    if (row[j] != sign * row[column])
                        return false;
                }
                return true;
            };
            if (matches(j, 0)) {
                members_.push_back({0, 0});
                continue;
            }
            int group = 0, sign = 1;
            for (; group < int(representatives_.size()); ++group) {
                if (matches(representatives_[group], 1)) break;
                if (matches(representatives_[group], -1)) { sign = -1; break; }
            }
            if (group == int(representatives_.size())) representatives_.push_back(j);
            members_.push_back({group, sign});
        }
        const int groups = int(representatives_.size());
        if (!groups || groups * 4 > cols * 3) {
            *this = {};
            return;
        }
        columns_.resize(size_t(rows) * groups);
        matrix_.resize(size_t(groups) * groups);
        rhs_.resize(groups);
        refresh(z, rows, cols);
    }

    // Control changes rewrite Z in place. The worker's partition is reusable
    // only while every column identity still holds in the current transfer.
    void refresh(const std::vector<double>& z, int rows, int cols) noexcept {
        valid_ = false;
        const int groups = int(representatives_.size());
        if (!groups || int(members_.size()) != cols || columns_.size() != size_t(rows) * groups ||
            z.size() != size_t(rows) * cols) return;
        for (int i = 0; i < rows; ++i) {
            const double* row = z.data() + size_t(i) * cols;
            double* compact = columns_.data() + size_t(i) * groups;
            for (int g = 0; g < groups; ++g) compact[g] = row[representatives_[g]];
            for (int j = 0; j < cols; ++j) {
                const auto [group, sign] = members_[j];
                if (row[j] != (sign ? sign * compact[group] : 0.0)) return;
            }
        }
        valid_ = true;
    }
    bool available() const noexcept { return valid_; }

    // Assemble I + S J C and -S J F from the local device Jacobians. Rows
    // eliminated by S do not contribute to either side.
    void assemble(const double* jacobian, const double* f, int ports,
                  const int* activePorts, const std::vector<RootDevice>& devices) noexcept {
        const int groups = int(representatives_.size());
        std::fill(matrix_.begin(), matrix_.end(), 0.);
        std::fill(rhs_.begin(), rhs_.end(), 0.);
        for (int g = 0; g < groups; ++g)
            matrix_[g * groups + g] = 1.;
        for (const RootDevice& device : devices) {
            for (int i = device.activeRowBase; i < device.activeRowBase + device.numActiveRows; ++i) {
                const auto [group, sign] = members_[i];
                if (!sign) continue;
                const double* derivative = jacobian + activePorts[i] * ports;
                double* row = matrix_.data() + group * groups;
                for (int q = device.portBase; q < device.portBase + device.numPorts; ++q) {
                    const double slope = sign * derivative[q];
                    if (slope == 0.) continue;
                    rhs_[group] -= slope * f[q];
                    const double* column = columns_.data() + q * groups;
                    for (int g = 0; g < groups; ++g)
                        row[g] += slope * column[g];
                }
            }
        }
    }

    bool finish(const double* f, int rows, double* dv) noexcept {
        const int groups = int(representatives_.size());
        if (!detail::solveData(matrix_.data(), rhs_.data(), groups))
            return false;
        for (int i = 0; i < rows; ++i) {
            double v = -f[i];
            const double* compact = columns_.data() + size_t(i) * groups;
            for (int g = 0; g < groups; ++g) v -= compact[g] * rhs_[g];
            dv[i] = v;
        }
        return true;
    }
private:
    struct Member { int group, sign; };
    std::vector<Member> members_;
    std::vector<int> representatives_;
    std::vector<double> columns_, matrix_, rhs_;
    bool valid_ = false;
};

} // namespace wdf::solver
