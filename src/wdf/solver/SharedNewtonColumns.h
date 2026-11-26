#pragma once

#include "wdf/DenseSolve.h"
#include "wdf/DenseTopology.h"
#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace wdf::solver {

// Parallel ports can inject exactly the same (or opposite) column into Z.
// If Z = C S, solve for the summed currents S u, then dv = -F - C (S u).
// This preserves the device laws and the full-network Newton residual.
class SharedNewtonColumns {
public:
    void prepare(const std::vector<double>& z, int rows, int cols, std::span<const RootDevice> devices = {}) {
        *this = {};
        if (cols <= 8 || z.size() != size_t(rows) * cols)
            return;
        // The two sensing terminals of the op-amp stamp draw identically zero
        // current. Their voltages still enter its output law and the full residual.
        std::vector<bool> zeroCurrent(cols);
        for (const auto& device : devices) {
            if (device.kind == DeviceKind::OpAmp && device.numActiveRows == 3) {
                zeroCurrent[device.activeRowBase + 1] = true;
                zeroCurrent[device.activeRowBase + 2] = true;
            }
        }
        for (int j = 0; j < cols; ++j) {
            if (zeroCurrent[j]) {
                members_.push_back({-1, 0});
                continue;
            }
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
            // Large independent roots still benefit from evaluating their port
            // voltages in parallel. Their Newton factorization stays unchanged.
            if (groups && rows >= 12) {
                plannedRows_ = rows;
                rhs_.resize(cols);
                packed_.resize(size_t((rows + lanes - 1) / lanes) * cols * lanes);
                refresh(z, rows, cols);
            }
            return;
        }
        columns_.resize(size_t(rows) * groups);
        packed_.resize(size_t((rows + lanes - 1) / lanes) * groups * lanes);
        matrix_.resize(size_t(groups) * groups);
        rhs_.resize(groups);
        refresh(z, rows, cols);
    }

    // Control changes rewrite Z in place. The worker's partition is reusable
    // only while every column identity still holds in the current transfer.
    void refresh(const std::vector<double>& z, int rows, int cols) noexcept {
        valid_ = false;
        const int groups = int(representatives_.size());
        if (!groups && plannedRows_ > 0 && rows == plannedRows_ && cols == int(rhs_.size()) &&
            z.size() == size_t(rows) * cols) {
            pack(z, rows, cols);
            valid_ = true;
            return;
        }
        if (!groups || int(members_.size()) != cols || columns_.size() != size_t(rows) * groups ||
            z.size() != size_t(rows) * cols) return;
        for (int i = 0; i < rows; ++i) {
            const double* row = z.data() + size_t(i) * cols;
            double* compact = columns_.data() + size_t(i) * groups;
            for (int g = 0; g < groups; ++g) compact[g] = row[representatives_[g]];
            for (int j = 0; j < cols; ++j) {
                const auto [group, sign] = members_[j];
                if (group >= 0 && row[j] != (sign ? sign * compact[group] : 0.0)) return;
            }
        }
        pack(columns_, rows, groups);
        valid_ = true;
    }
    bool available() const noexcept { return valid_ && !representatives_.empty(); }
    bool hasPackedResidual() const noexcept { return valid_; }

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

    // F = v - vOc + C (S i). Use the same grouping for residual checks and
    // Newton corrections, including each line-search trial.
    bool residual(const double* v, const double* vOc, const double* currents,
                  const int* activePorts, int rows, double* f) noexcept {
        if (members_.empty()) {
            for (size_t j = 0; j < rhs_.size(); ++j) rhs_[j] = currents[activePorts[j]];
            multiply(packed_.data(), rhs_.data(), int(rhs_.size()), v, vOc, rows, f);
            return true;
        }
        std::fill(rhs_.begin(), rhs_.end(), 0.);
        for (size_t j = 0; j < members_.size(); ++j) {
            const auto [group, sign] = members_[j];
            if (sign) rhs_[group] += sign * currents[activePorts[j]];
            else if (group < 0 && currents[activePorts[j]] != 0.) return false;
        }
        multiply(packed_.data(), rhs_.data(), int(rhs_.size()), v, vOc, rows, f);
        return true;
    }

    bool finish(const double* f, int rows, double* dv) noexcept {
        if (!detail::solveDroppingZeroCorrections(matrix_.data(), rhs_.data(), int(representatives_.size())))
            return false;
        multiply(packed_.data(), rhs_.data(), int(rhs_.size()), f, nullptr, rows, dv);
        return true;
    }
private:
    static constexpr int lanes = 16;

    void pack(const std::vector<double>& source, int rows, int groups) noexcept {
        for (int r = 0; r < rows; ++r) {
            const int block = r / lanes, lane = r % lanes;
            for (int g = 0; g < groups; ++g)
                packed_[(size_t(block) * groups + g) * lanes + lane] = source[size_t(r) * groups + g];
        }
    }

    static void multiply(const double* coefficients, const double* current, int groups,
                         const double* base, const double* vOc, int rows, double* result) noexcept {
        for (int r = 0; r < rows; r += lanes) {
            const int count = std::min(lanes, rows - r);
            const double* block = coefficients + size_t(r) * groups;
            double sum[lanes]{};
            if (vOc) {
                for (int l = 0; l < count; ++l) sum[l] = base[r + l] - vOc[r + l];
                for (int g = 0; g < groups; ++g)
                    for (int l = 0; l < lanes; ++l)
                        sum[l] += block[g * lanes + l] * current[g];
            } else {
                for (int l = 0; l < count; ++l) sum[l] = -base[r + l];
                for (int g = 0; g < groups; ++g)
                    for (int l = 0; l < lanes; ++l)
                        sum[l] -= block[g * lanes + l] * current[g];
            }
            for (int l = 0; l < count; ++l) result[r + l] = sum[l];
        }
    }

    struct Member { int group, sign; };
    std::vector<Member> members_;
    std::vector<int> representatives_;
    std::vector<double> columns_, packed_, matrix_, rhs_;
    int plannedRows_ = 0;
    bool valid_ = false;
};

} // namespace wdf::solver
