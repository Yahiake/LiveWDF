#pragma once

// Union-find with symbolic potentials: v(n) = v(rep(n)) + pot(n).

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "spice/Circuit.h"

namespace wdf {

// --------------------------------------------------------------------------- //
// Union-find with symbolic potentials: v(n) = v(rep(n)) + pot(n)
// --------------------------------------------------------------------------- //

/// `pot` is a length `nsrc + 1` vector: index 0 is the constant part, index s+1
/// the coefficient of `src[s]`. Trees are walked without path compression --
/// they are at most `nsrc` deep and compression would need the node's own
/// potential rewriting, which is not worth the bookkeeping.
class Potentials {
public:
    explicit Potentials(int nsrc) : nsrc_(nsrc), width_(nsrc + 1) {
        unit_.assign(width_, std::vector<double>(width_, 0.0));
        for (int s = 0; s < nsrc; ++s)
            unit_[s + 1][s + 1] = 1.0;
    }

    void find(const std::string& n, std::string& root, std::vector<double>& pot) {
        add(n);
        std::vector<double> acc(width_, 0.0);
        std::string cur = n;
        while (parent_[cur] != cur) {
            const std::vector<double>& p = pot_[cur];
            for (int k = 0; k < width_; ++k)
                acc[k] += p[k];
            cur = parent_[cur];
        }
        root = cur;
        pot = std::move(acc);
    }

    /// Impose v(a) - v(b) = c, where `c` is itself affine (a source's value).
    void unite(const std::string& a, const std::string& b, const std::vector<double>& c) {
        std::string ra, rb;
        std::vector<double> oa, ob;
        find(a, ra, oa);
        find(b, rb, ob);
        if (ra == rb) {
            double worst = 0.0;
            for (int k = 0; k < width_; ++k)
                worst = std::max(worst, std::fabs((oa[k] - ob[k]) - c[k]));
            if (worst > 1e-9)
                throw std::runtime_error("inconsistent ideal sources: v(" + a + ")-v(" + b + ")");
            return;
        }
        if (gnd_[rb] && !gnd_[ra]) {
            parent_[ra] = rb;
            std::vector<double>& p = pot_[ra];
            for (int k = 0; k < width_; ++k)
                p[k] = c[k] - oa[k] + ob[k];
        } else {
            parent_[rb] = ra;
            std::vector<double>& p = pot_[rb];
            for (int k = 0; k < width_; ++k)
                p[k] = oa[k] - ob[k] - c[k];
            gnd_[ra] = gnd_[ra] || gnd_[rb];
        }
    }

    /// Coefficient vector imposing v(a) - v(b) = src[s] (all other sources 0).
    const std::vector<double>& unit(int s) const { return unit_[s + 1]; }

private:
    void add(const std::string& n) {
        if (parent_.find(n) == parent_.end()) {
            parent_[n] = n;
            pot_[n].assign(width_, 0.0);
            gnd_[n] = (n == kGroundName);
        }
    }

    int nsrc_, width_;
    std::unordered_map<std::string, std::string> parent_;
    std::unordered_map<std::string, std::vector<double>> pot_;
    std::unordered_map<std::string, bool> gnd_;
    std::vector<std::vector<double>> unit_;
};

} // namespace wdf
