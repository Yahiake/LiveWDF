#include "wdf/EngineStep.h"
#include "wdf/DenseSolve.h"

#include <algorithm>
#include <cmath>

namespace wdf {
namespace {
using Plan = LoadedJunctionPlan;

double sense(const Plan::Term& term, const double* v) {
    const double minus = term.negativePort < 0 ? 0.0 : v[term.negativePort];
    return term.gain * (v[term.port] - minus);
}

double dot(const double* row, const double* data, int n) {
    double value = 0.0;
    for (int i = 0; i < n; ++i)
        value += row[i] * data[i];
    return value;
}

struct Tabulation {
    int bjt = -1;
    bool forward = false;
};

} // namespace

bool EngineStep::hasLoadedJunctionPreview() const noexcept {
    return !loadedJunction_.laws.empty() && loadedJunction_.revision == linearRebuilds() &&
           !hasAdjustedLiveTransfer() && !powerSagEnabled_ && !staticMode_;
}

void EngineStep::swapLoadedJunctionPreview(EngineStep& prepared) noexcept {
    std::swap(loadedJunction_, prepared.loadedJunction_);
    // The caller installs the worker's matching linear transfer first. Check
    // the complete port impedance (also catches the wrong knob position).
    if (K_ != prepared.K_ || fs_ != prepared.fs_ || Z_ != loadedJunction_.impedance)
        loadedJunction_.revision = -1;
    else
        loadedJunction_.revision = linearRebuilds();
}

bool EngineStep::prepareLoadedJunctionPreview() {
    loadedJunction_ = {};
    if (!nD_ || K_ > 64 || hasAdjustedLiveTransfer() || powerSagEnabled_ ||
        !vcas_.empty() || !otas_.empty() || !dbx200Biases_.empty())
        return false;

    Plan p;
    std::vector<Tabulation> recipe;
    p.impedance = Z_;
    p.constant.assign(K_, 0.0);
    std::vector<bool> used(nD_, false);
    for (const auto& d : rootLayout_.devices) {
        const int r = d.portBase, ci = d.compIndex;
        const int j = static_cast<int>(p.laws.size());
        if (d.kind == RootKind::Diode) {
            if (used[ci]) continue;
            p.laws.emplace_back();
            auto& law = p.laws.back();
            law.sense = {r, -1, 1.0};
            law.diode = ci;
            for (const auto& pair : rootLayout_.devices) {
                const int k = pair.compIndex;
                if (pair.kind == RootKind::Diode && k > ci && !used[k] &&
                    isAntiParallelDiodePair(comp_, ci, k) &&
                    std::equal(comp_.diode[ci].first.off.begin(), comp_.diode[ci].first.off.end(),
                               comp_.diode[k].first.off.begin(), comp_.diode[k].first.off.end(),
                               [](double a, double b) { return a == -b; })) {
                    law.opposite = k;
                    law.oppositePort = pair.portBase;
                    used[k] = true;
                    break;
                }
            }
            p.injection.push_back({r, j, .001});
            recipe.emplace_back();
        } else if (d.kind == RootKind::ControlledRes && circuit_.ctrlRes[ci].photo) {
            p.laws.emplace_back();
            p.laws.back().sense = {r, -1, 1.0};
            p.laws.back().photo = ci;
            p.injection.push_back({r, j, .001}); // q in mA, i in A
            recipe.emplace_back();
        } else if (d.kind == RootKind::Bjt) {
            const auto& b = bjts_[ci];
            if (b.hasLowCurrentCorrections() || b.Ikf != 0.0)
                return false;
            const double sign = b.pnp ? -1.0 : 1.0;
            for (int junction = 0; junction < 2; ++junction) {
                const bool forward = junction == 0;
                p.laws.emplace_back();
                p.laws.back().sense = {r + 1, forward ? -1 : r, sign};
                p.laws.back().bjt = ci;
                p.laws.back().forward = forward;
                const double ic = sign * .001 * (forward ? 1.0 : -1.0 / b.alphaR);
                const double ib = sign * .001 / (forward ? b.Bf : b.Br);
                p.injection.push_back({r, j + junction, ic});
                p.injection.push_back({r + 1, j + junction, ib});
                // Emitter current is diagnostic. The CE and BE ports already
                // return it to the emitter in the network's active columns.
                p.injection.push_back({r + 2, j + junction, -ic - ib});
                recipe.push_back({ci, forward});
            }
        } else if (d.kind == RootKind::OpAmp) {
            const auto& amp = opAmps_[ci];
            if (amp.softRails || amp.hasClassABOutput()) return false;
            const double a = std::clamp(opAmpAlpha_[ci], 0.0, 1.0);
            const double g = 1.0 / amp.outputImpedance;
            p.laws.emplace_back();
            auto& law = p.laws.back();
            law.sense = {r + 1, r + 2, a * amp.openLoopGain / (1.0 + a)};
            law.pole = ci;
            law.inversePole = 1.0 / (1.0 + a);
            p.linear.push_back({r, r, g * amp.outputVoltageWeight()});
            p.injection.push_back({r, j, -g});
            recipe.emplace_back();
        } else if (d.kind == RootKind::Jfet) {
            const auto e = comp_.jfet[ci].model.evaluate(vv_[r + 1], vv_[r]);
            if (!e.valid) return false;
            const double c = e.id - e.did_ds * vv_[r] - e.did_gs * vv_[r + 1];
            p.fets.push_back({ci, r, e.did_ds, e.did_gs, c, 0.0});
            p.linear.push_back({r, r, e.did_ds});
            p.linear.push_back({r, r + 1, e.did_gs});
            p.constant[r] = c;
        } else {
            return false;
        }
    }
    const int k = K_, m = static_cast<int>(p.laws.size());
    if (!m || m > 32) return false;
    auto independent = [&](int port) { return rootLayout_.redRow[port] >= 0; };
    // M = I + ZJ for the linear currents i = Jv + c + Cq.
    std::vector<double> lu(k * k, 0.0), x(k);
    std::vector<int> pivot(k);
    for (int i = 0; i < k; ++i) {
        lu[i * k + i] = 1.0;
        for (const auto& e : p.linear)
            lu[i * k + e.col] += Z_[i * k + e.row] * e.value;
    }
    if (!detail::factorLUData(lu.data(), pivot.data(), k)) return false;
    p.openMap.resize(k * k);
    p.voltageMap.resize(k * m);
    p.mutual.resize(m * m);
    p.offset.assign(k, 0.0);
    for (int i = 0; i < k; ++i)
        for (const auto& fet : p.fets)
            p.offset[i] -= Z_[i * k + fet.port] * fet.offset;
    detail::solveLUData(lu.data(), pivot.data(), p.offset.data(), k);
    for (int col = 0; col < k + m; ++col) {
        std::fill(x.begin(), x.end(), 0.0);
        if (col < k) x[col] = 1.0;
        else {
            for (const auto& e : p.injection)
                if (e.col == col - k && independent(e.row))
                    for (int i = 0; i < k; ++i)
                        x[i] -= Z_[i * k + e.row] * e.value;
        }
        detail::solveLUData(lu.data(), pivot.data(), x.data(), k);
        if (!std::all_of(x.begin(), x.end(), [](double v) { return std::isfinite(v); }))
            return false;
        for (int i = 0; i < k; ++i) {
            if (col < k) p.openMap[i * k + col] = x[i];
            else p.voltageMap[i * m + col - k] = x[i];
        }
        if (col >= k)
            for (int i = 0; i < m; ++i)
                p.mutual[i * m + col - k] = sense(p.laws[i].sense, x.data());
    }
    for (auto& fet : p.fets)
        for (int i = 0; i < k; ++i) {
            for (int j = 0; j < k; ++j) x[j] = Z_[j * k + fet.port];
            fet.load = std::max(fet.load, std::abs(dot(&p.openMap[i*k], x.data(), k)));
        }
    for (int j = 0; j < m; ++j) {
        auto& law = p.laws[j];
        law.resistance = -p.mutual[j * m + j];
        if (!(law.resistance >= 0.0)) return false;
        p.mutual[j * m + j] = 0.0;
        if (law.pole >= 0 || law.photo >= 0) continue;
        // Tabulate in terminal voltage to resolve the knee at every impedance.
        // Samples outside the proven monotonically invertible interval use
        // the full root, never a guessed extrapolation of the exponential.
        for (int s = -1520; s <= 1500; ++s) {
            const double v = s < -1500 ? -std::ldexp(1.0, -s - 1500) : .001 * s;
            double i = 0.0, g = 0.0;
            if (law.diode >= 0) {
                comp_.diode[law.diode].second.eval(v, i, g);
                if (law.opposite >= 0) {
                    double reverse;
                    comp_.diode[law.opposite].second.eval(-v, reverse, g);
                    i -= reverse;
                }
            } else {
                const auto& b = bjts_[recipe[j].bjt];
                const double vt = recipe[j].forward ? b.nfVt : b.nrVt;
                i = b.Is * std::expm1(std::min(kBJtMaxExpArg, v / vt));
            }
            i *= 1000.0;
            const double u = v + law.resistance * i;
            if (!std::isfinite(u) || (!law.drive.empty() && u <= law.drive.back())) return false;
            law.drive.push_back(u);
            law.current.push_back(i);
        }
    }
    for (auto& law : p.laws) {
        if (law.pole >= 0 || law.photo >= 0) continue;
        size_t n = 1;
        constexpr int count = 32769;
        law.lookup.reserve(count);
        for (int a = 0; a < count; ++a) {
            const double x = (a - count / 2) / 512.0;
            while (n < law.drive.size() - 1 && x > law.drive[n]) ++n;
            const double f = (x - law.drive[n - 1]) / (law.drive[n] - law.drive[n - 1]);
            law.lookup.push_back(law.current[n - 1] + f * (law.current[n] - law.current[n - 1]));
        }
    }
    p.open.resize(k);
    p.drive.resize(m); p.q.resize(m); p.raw.resize(m); p.correction.resize(m);
    p.matrix.resize(m * m); p.pivots.resize(m); p.low.resize(m); p.high.resize(m);
    p.revision = linearRebuilds();
    loadedJunction_ = std::move(p);
    return true;
}

bool EngineStep::tryLoadedJunctionRoot() noexcept {
    auto& p = loadedJunction_;
    const int k = K_, m = static_cast<int>(p.laws.size());
    ++loadedJunctionAttempts_;
    const bool seed = p.seed;
    p.seed = true; // A declined solve must restart from the native solution next sample.
    for (int i = 0; i < k; ++i)
        p.open[i] = p.offset[i] + dot(&p.openMap[i * k], vOc_.data(), k);
    for (int i = 0; i < m; ++i) {
        const auto& law = p.laws[i];
        p.drive[i] = sense(law.sense, p.open.data());
        if (seed) {
            if (law.pole >= 0) p.q[i] = opAmpVp_[law.pole];
            else if (law.bjt >= 0) {
                const auto& b = bjts_[law.bjt];
                const double vt = law.forward ? b.nfVt : b.nrVt;
                p.q[i] = 1000.0 * b.Is * std::expm1(std::min(kBJtMaxExpArg, sense(law.sense, vv_.data()) / vt));
            } else {
                const double reverse = law.oppositePort >= 0 ? ii_[law.oppositePort] : 0.0;
                p.q[i] = 1000.0 * (ii_[law.sense.port] - reverse);
            }
        }
        if (law.pole < 0) continue;
        const int ci = law.pole;
        if (opAmps_[ci].softRails) return false;
        const double prev = opAmpVp_[ci], maxDv = opAmpMaxDv_[ci];
        const auto& amp = opAmps_[ci];
        p.drive[i] += law.inversePole * prev;
        p.low[i] = amp.railLow();
        p.high[i] = amp.railHigh();
        if (maxDv > 0.0) {
            p.low[i] = std::clamp(prev - maxDv, p.low[i], p.high[i]);
            p.high[i] = std::clamp(prev + maxDv, p.low[i], p.high[i]);
        }
    }
    double error = 0.0;
    for (int iteration = 0; iteration <= 6; ++iteration) {
        error = 0.0;
        for (int i = 0; i < m; ++i) {
            const auto& law = p.laws[i];
            const double* row = &p.mutual[i * m];
            const double x = p.drive[i] + dot(row, p.q.data(), m);
            if (!std::isfinite(x)) return false;
            p.raw[i] = x;
            double y, slope;
            if (law.photo >= 0) {
                // The lamp's conductance comes from the preceding accepted electrical frame.
                // Solve its loaded branch exactly within the simultaneous junction system.
                slope = 1.0 / (law.resistance + .001 * ctrlResValues_[law.photo]);
                y = slope * x;
            } else if (law.pole >= 0) {
                const double gain = 1.0 / (1.0 + law.resistance);
                y = std::clamp(x * gain, p.low[i], p.high[i]);
                slope = (y > p.low[i] && y < p.high[i]) ? gain : 0.0;
            } else if (x >= -32.0 && x < 32.0 && x >= law.drive.front() && x <= law.drive.back()) {
                const double index = (x + 32.0) * 512.0;
                const auto bin = std::min(static_cast<size_t>(index), law.lookup.size() - 2);
                const double delta = law.lookup[bin + 1] - law.lookup[bin];
                slope = delta * 512.0;
                y = law.lookup[bin] + (index - static_cast<double>(bin)) * delta;
            } else {
                if (x < law.drive.front() || x > law.drive.back()) return false;
                auto upper = std::lower_bound(law.drive.begin() + 1, law.drive.end(), x);
                const auto n = static_cast<size_t>(upper - law.drive.begin());
                slope = (law.current[n] - law.current[n - 1]) / (law.drive[n] - law.drive[n - 1]);
                y = law.current[n - 1] + slope * (x - law.drive[n - 1]);
            }
            p.correction[i] = y - p.q[i];
            error = std::max(error, std::abs(p.correction[i]));
            for (int j = 0; j < m; ++j)
                p.matrix[i * m + j] = (i == j ? 1.0 : 0.0) - slope * row[j];
        }
        if (error <= 1e-7) break;
        if (iteration == 6 || !detail::factorLUData(p.matrix.data(), p.pivots.data(), m))
            return false;
        detail::solveLUData(p.matrix.data(), p.pivots.data(), p.correction.data(), m);
        double fraction = 1.0;
        for (int i = 0; i < m; ++i) {
            if (!std::isfinite(p.correction[i])) return false;
            if (p.laws[i].pole < 0) continue;
            const double dz = dot(&p.mutual[i * m], p.correction.data(), m);
            const double gain = 1.0 + p.laws[i].resistance;
            const double lo = p.low[i] * gain, hi = p.high[i] * gain, x = p.raw[i];
            if ((x < lo && x + dz > hi) || (x > hi && x + dz < lo))
                fraction = std::min(fraction, (.5 * (lo + hi) - x) / dz);
        }
        for (int j = 0; j < m; ++j)
            p.q[j] += std::clamp(fraction * p.correction[j], -8.0, 8.0);
    }

    for (int i = 0; i < k; ++i) {
        vt_[i] = p.open[i] + dot(&p.voltageMap[i * m], p.q.data(), m);
        if (!std::isfinite(vt_[i])) return false;
    }
    iv_ = p.constant;
    for (const auto& e : p.linear) iv_[e.row] += e.value * vt_[e.col];
    for (const auto& e : p.injection) iv_[e.row] += e.value * p.q[e.col];
    // A FET tangent can describe a switch at a settled bias. Only keep it
    // while the actual device current still agrees under this load.
    for (const auto& fet : p.fets) {
        const auto e = comp_.jfet[fet.device].model.evaluate(vt_[fet.port + 1], vt_[fet.port]);
        if (!e.valid || std::abs(e.id - iv_[fet.port]) * fet.load > .001)
            return false;
    }
    for (const auto& law : p.laws) {
        if (law.opposite < 0) continue;
        double reverse, conductance;
        comp_.diode[law.opposite].second.eval(-vt_[law.sense.port], reverse, conductance);
        iv_[law.oppositePort] = reverse;
        iv_[law.sense.port] += reverse;
    }
    // The diagnostic is in port volts. The table interpolates intervals of
    // 1 mV and the FET guard bounds its loaded tangent error at 1 mV.
    commitCompiledRoot(.001);
    p.seed = false;
    ++loadedJunctionHits_;
    return true;
}

} // namespace wdf
