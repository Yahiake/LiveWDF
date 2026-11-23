#pragma once

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <vector>

namespace wdf {

/// Reapply a network's source stamps and pivoted elimination to one live RHS.
/// This avoids multiplying by a dense inverse when its factors are sparse.
struct FactoredTransfer {
    struct Term {
        int index;
        double value;
    };
    struct Row {
        int pivot = 0;
        int forwardEnd = 0;
        int upperStart = 0;
        int inputEnd = 0;
        double diagonal = 1.0;
    };

    std::vector<Row> rows;
    std::vector<int> order;
    std::vector<int> degree;
    std::vector<std::vector<double>> permutedY, permutedRhs;
    std::vector<double> work;
    std::vector<Term> input, forward, upper;
    bool active = false;

    void orderSystem(std::vector<std::vector<double>>& y, std::vector<std::vector<double>>& rhs) {
        const int n = static_cast<int>(y.size());
        degree.assign(n, 0);
        order.resize(n);
        work.resize(n);
        std::iota(order.begin(), order.end(), 0);
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                if (y[i][j] != 0.0 || y[j][i] != 0.0)
                    ++degree[i];
        // Eliminate leaves and two-terminal branch nodes before their shared buses.
        // The index breaks ties consistently when the same controls are rebuilt.
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return degree[a] == degree[b] ? a < b : degree[a] < degree[b];
        });
        permutedY.resize(n);
        permutedRhs.resize(n);
        for (int i = 0; i < n; ++i) {
            permutedY[i].swap(y[order[i]]);
            permutedRhs[i].swap(rhs[order[i]]);
        }
        for (auto& row : permutedY) {
            for (int j = 0; j < n; ++j)
                work[j] = row[order[j]];
            std::copy(work.begin(), work.end(), row.begin());
        }
        y.swap(permutedY);
        rhs.swap(permutedRhs);
    }

    void prepare(const std::vector<std::vector<double>>& rhs, const std::vector<int>& liveCols) {
        rows.assign(rhs.size(), {});
        input.clear();
        forward.clear();
        upper.clear();
        active = false;
        for (size_t i = 0; i < rows.size(); ++i) {
            for (int t = 0; t < static_cast<int>(liveCols.size()); ++t) {
                const double value = rhs[i][liveCols[t]];
                if (value != 0.0)
                    input.push_back({t, value});
            }
            rows[i].inputEnd = static_cast<int>(input.size());
        }
    }

    void finish(const std::vector<std::vector<double>>& y) {
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            rows[i].diagonal = y[i][i];
            rows[i].upperStart = static_cast<int>(upper.size());
            for (int j = i + 1; j < static_cast<int>(rows.size()); ++j)
                if (y[i][j] != 0.0)
                    upper.push_back({j, y[i][j]});
        }
    }

    size_t operations() const { return input.size() + forward.size() + upper.size() + rows.size(); }

    void apply(const double* waves, double* output) noexcept {
        double* out = work.data();
        int j = 0;
        const int n = static_cast<int>(rows.size());
        for (int i = 0; i < n; ++i) {
            double v = 0.0;
            for (; j < rows[i].inputEnd; ++j)
                v += input[j].value * waves[input[j].index];
            out[i] = v;
        }
        j = 0;
        for (int i = 0; i < n; ++i) {
            if (rows[i].pivot != i)
                std::swap(out[i], out[rows[i].pivot]);
            const double v = out[i];
            for (; j < rows[i].forwardEnd; ++j)
                out[forward[j].index] -= forward[j].value * v;
        }
        int end = static_cast<int>(upper.size());
        for (int i = n - 1; i >= 0; --i) {
            double v = out[i];
            for (j = rows[i].upperStart; j < end; ++j)
                v -= upper[j].value * out[upper[j].index];
            out[i] = v / rows[i].diagonal;
            end = rows[i].upperStart;
            output[order[i]] = out[i];
        }
    }
};
} // namespace wdf
