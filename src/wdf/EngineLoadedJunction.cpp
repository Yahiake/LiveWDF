#include "wdf/EngineStep.h"
#include "wdf/DenseSolve.h"
#include "wdf/LoadedJunctionMotion.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <cstring>

namespace wdf {
namespace {
using Plan = LoadedJunctionPlan;
// Below −12 thermal voltages, the omitted reverse-junction exponential is
// less than 6.2 ppm of its saturation current. Its leakage remains in the model.
double reverseCutoffVoltage(const BjtModel& b, std::span<const double> impedance,
                            int roots, int port, double residualBudget) {
    // Bound the missing IC/IB contribution to vOc - Z*i at every root.
    double peakOhms = 0.;
    for (int i = 0; i < roots; ++i) {
        const auto* row = impedance.data() + i * roots;
        peakOhms = std::max(peakOhms, std::abs(row[port]) / b.alphaR + std::abs(row[port + 1]) / b.Br);
    }
    const double scale = b.Is * peakOhms;
    const double vt = scale > 0. ? std::min(-12., std::log(residualBudget / scale)) : -12.;
    return b.nrVt * vt;
}

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
           !powerSagEnabled_ && !staticMode_;
}

void EngineStep::swapLoadedJunctionPreview(EngineStep& prepared) noexcept {
    std::swap(loadedJunction_, prepared.loadedJunction_);
    // The caller installs the worker's matching linear transfer first. Check
    // the complete port impedance (also catches the wrong knob position).
    if (K_ != prepared.K_ || fs_ != prepared.fs_ || Z_ != loadedJunction_.impedance)
        loadedJunction_.revision = -1;
    else {
        loadedJunction_.revision = linearRebuilds();
        loadedJunction_.controlRevision = liveControlRevision();
    }
}

bool EngineStep::prepareLoadedJunctionPreview(size_t inputSource, bool screenBenefit) {
    loadedJunction_ = {};
    if (!nD_ || K_ > 64 || hasAdjustedLiveTransfer() || powerSagEnabled_ ||
        !vcas_.empty() || !otas_.empty() || !dbx200Biases_.empty())
        return false;

    std::vector<bool> linearAmps(nOpAmp_), bufferTransistors(nBjt_);
    screenJunctionDevices(inputSource, linearAmps, bufferTransistors);
    bool negativeReverse = false;
    if (!buildLoadedJunctionPlan(linearAmps, bufferTransistors, false, negativeReverse)) {
        if (!negativeReverse || !buildLoadedJunctionPlan(linearAmps, bufferTransistors, true, negativeReverse))
            return false;
    }
    if (!screenBenefit)
        return true;
    std::vector<std::uint8_t> crossed(loadedJunction_.reverseCutoffs.empty() ? 0 : nBjt_);
    if (screenJunctionBenefit(inputSource, crossed))
        return true;
    // A transistor that comes out of reverse cutoff during the physical
    // sweep needs its full junction law. Retry once, keeping every other guard.
    if (std::ranges::any_of(crossed, [](auto v) { return v != 0; }) &&
        buildLoadedJunctionPlan(linearAmps, bufferTransistors, true, negativeReverse, crossed) &&
        screenJunctionBenefit(inputSource))
        return true;
    loadedJunction_ = {};
    return false;
}

bool EngineStep::buildLoadedJunctionPlan(const std::vector<bool>& linearAmps,
                                         const std::vector<bool>& bufferTransistors,
                                         bool omitReverse, bool& negativeReverse, std::span<const std::uint8_t> retained) {
    Plan p;
    std::vector<Tabulation> recipe;
    p.impedance = Z_;
    p.constant.assign(K_, 0.0);
    std::vector<bool> used(nD_, false);
    std::vector<int> strings(nD_, -1);
    for (int ci = 0; ci < nD_; ++ci) {
        LoadedDiodeString pair{};
        if (!used[ci] && findLoadedDiodeString(comp_, rootLayout_, ci, used, pair) &&
            strings[pair.first] < 0 && strings[pair.second] < 0) {
            strings[ci] = static_cast<int>(p.strings.size());
            used[pair.first] = used[pair.second] = true;
            p.strings.push_back(std::move(pair));
        }
    }
    for (const auto& d : rootLayout_.devices) {
        const int r = d.portBase, ci = d.compIndex;
        const int j = static_cast<int>(p.laws.size());
        if (d.kind == RootKind::Diode) {
            if (used[ci]) continue;
            p.laws.emplace_back();
            auto& law = p.laws.back();
            law.sense = {r, -1, 1.0};
            law.diode = ci;
            law.string = strings[ci];
            for (const auto& pair : rootLayout_.devices) {
                const int k = pair.compIndex;
                if (law.string < 0 && pair.kind == RootKind::Diode && strings[k] < 0 && k > ci && !used[k] &&
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
            if (bufferTransistors[ci]) {
                const auto a = b.evaluateTerminals(vv_[r], vv_[r + 1]);
                p.buffers.push_back({ci, r, sign * vv_[r + 1]});
                const double gds[] = {a.diC_dVce, a.diB_dVce, -a.diC_dVce - a.diB_dVce};
                const double gm[] = {a.diC_dVbe, a.diB_dVbe, -a.diC_dVbe - a.diB_dVbe};
                const double currents[] = {a.iC, a.iB, a.iE};
                for (int v = 0; v < 3; ++v) {
                    p.linear.push_back({r + v, r, gds[v]});
                    p.linear.push_back({r + v, r + 1, gm[v]});
                    p.constant[r + v] = currents[v] - gds[v] * vv_[r] - gm[v] * vv_[r+1];
                }
                continue;
            }
            for (int junction = 0; junction < 2; ++junction) {
                const bool forward = junction == 0;
                if (!forward && omitReverse && (retained.empty() || !retained[ci])) {
                    const double cutoff = reverseCutoffVoltage(b, Z_, K_, r, kFastLiveTolerance / nBjt_);
                    if (sign * (vv_[r + 1] - vv_[r]) <= cutoff) {
                        // The reverse transport current tends to -Is for both polarities.
                        const double ic = sign * b.Is / b.alphaR;
                        const double ib = -sign * b.Is / b.Br;
                        p.constant[r] += ic;
                        p.constant[r + 1] += ib;
                        p.constant[r + 2] -= ic + ib;
                        p.reverseCutoffs.push_back({ci, r, cutoff});
                        continue;
                    }
                }
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
            if (linearAmps[ci]) {
                const double source = a * amp.openLoopGain / (1.0 + a);
                const double history = 1.0 / (1.0 + a);
                p.linearPoles.push_back({ci, r, source, history, -g});
                p.linear.push_back({r, r, g * amp.outputVoltageWeight()});
                p.linear.push_back({r, r + 1, -g * source});
                p.linear.push_back({r, r + 2, g * source});
                continue;
            }
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
            p.laws.emplace_back();
            p.laws.back().sense = {r, -1, 1.0};
            p.laws.back().fet = static_cast<int>(p.fets.size());
            p.injection.push_back({r, j, .001});
            recipe.emplace_back();
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
            if (independent(e.row)) lu[i * k + e.col] += Z_[i * k + e.row] * e.value;
    }
    if (!detail::factorLUData(lu.data(), pivot.data(), k)) return false;
    p.openMap.resize(k * k);
    p.voltageMap.resize(k * m);
    p.mutual.resize(m * m);
    p.offset.assign(k, 0.0);
    for (int i = 0; i < k; ++i)
        for (int j : rootLayout_.redCol)
            p.offset[i] -= Z_[i * k + j] * p.constant[j];
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
    const int poles = static_cast<int>(p.linearPoles.size());
    p.poleMap.resize(k * poles);
    for (int j = 0; j < poles; ++j) {
        const auto& pole = p.linearPoles[j];
        for (int i = 0; i < k; ++i)
            x[i] = -Z_[i * k + pole.port] * pole.currentGain * pole.historyGain;
        detail::solveLUData(lu.data(), pivot.data(), x.data(), k);
        for (int i = 0; i < k; ++i) p.poleMap[i * poles + j] = x[i];
    }
    for (int j = 0; j < m; ++j) {
        auto& law = p.laws[j];
        law.resistance = -p.mutual[j * m + j];
        if (!(law.resistance >= 0.0)) {
            negativeReverse = !omitReverse && law.bjt >= 0 && !law.forward &&
                              std::isfinite(law.resistance);
            return false;
        }
        p.mutual[j * m + j] = 0.0;
        if (law.pole >= 0 || law.photo >= 0 || law.fet >= 0) continue;
        // Tabulate in terminal voltage to resolve the knee at every impedance.
        // Samples outside the proven monotonically invertible interval use
        // the full root, never a guessed extrapolation of the exponential.
        for (int s = law.string < 0 ? -1520 : -1500; s <= 1500; ++s) {
            const double v = s < -1500 ? -std::ldexp(1.0, -s - 1500) : .001 * s;
            double i = 0.0, g = 0.0;
            if (law.diode >= 0) {
                comp_.diode[law.diode].second.eval(v, i, g);
                if (law.string >= 0) {
                    double drop;
                    auto& pair = p.strings[law.string];
                    i -= pair.tabulate(comp_, -v, drop);
                    if (s >= -1500) pair.firstDrop.push_back(drop);
                } else if (law.opposite >= 0) {
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
        if (law.pole >= 0 || law.photo >= 0 || law.fet >= 0) continue;
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
    p.controlRevision = liveControlRevision();
    loadedJunction_ = std::move(p);
    return true;
}

bool EngineStep::tryLoadedJunctionRoot() noexcept {
    auto& p = loadedJunction_;
    const int k = K_, m = static_cast<int>(p.laws.size());
    const bool changed = p.controlRevision != liveControlRevision() &&
                         std::memcmp(p.impedance.data(), Z_.data(), Z_.size() * sizeof(double)) != 0;
    p.controlRevision = liveControlRevision();
    if (changed) {
        if (!engine::updateJunctionLoading(p, Z_, rootLayout_.redRow)) {
            p.revision = -1;
            return false;
        }
        for (auto& guard : p.reverseCutoffs)
            guard.maxVbc = reverseCutoffVoltage(bjts_[guard.device], Z_, K_, guard.port, kFastLiveTolerance / nBjt_);
        p.seed = true;
        p.cooldown = p.misses = 0;
    }
    if (p.cooldown > 0) { --p.cooldown; return false; }
    ++loadedJunctionAttempts_;
    p.misses = std::min(6, p.misses + 1);
    p.cooldown = (1 << (p.misses - 1)) - 1;
    const bool seed = p.seed;
    p.seed = true; // A declined solve must restart from the native solution next sample.
    const int poles = static_cast<int>(p.linearPoles.size());
    for (int i = 0; i < k; ++i) {
        p.open[i] = p.offset[i] + dot(&p.openMap[i * k], vOc_.data(), k);
        for (int j = 0; j < poles; ++j)
            p.open[i] += p.poleMap[i * poles + j] * opAmpVp_[p.linearPoles[j].device];
    }
    for (int i = 0; i < m; ++i) {
        const auto& law = p.laws[i];
        p.drive[i] = sense(law.sense, p.open.data());
        if (seed) {
            if (law.pole >= 0) p.q[i] = opAmpVp_[law.pole];
            else if (law.fet >= 0) {
                const auto& f = p.fets[law.fet];
                p.q[i] = 1000.0*(ii_[f.port]-f.offset-f.conductance*vv_[f.port]-f.transconductance*vv_[f.port+1]);
            } else if (law.bjt >= 0) {
                const auto& b = bjts_[law.bjt];
                const double vt = law.forward ? b.nfVt : b.nrVt;
                p.q[i] = 1000.0 * b.Is * std::expm1(std::min(kBJtMaxExpArg, sense(law.sense, vv_.data()) / vt));
            } else {
                double reverse = law.oppositePort >= 0 ? ii_[law.oppositePort] : 0.0;
                if (law.string >= 0) {
                    const auto& pair = p.strings[law.string];
                    reverse = ii_[pair.firstPort] + pair.firstShunt*vv_[pair.firstPort];
                }
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
    constexpr int maxCorrections = 8;
    for (int iteration = 0; iteration <= maxCorrections; ++iteration) {
        error = 0.0;
        for (int i = 0; i < m; ++i) {
            const auto& law = p.laws[i];
            const double* row = &p.mutual[i * m];
            const double x = p.drive[i] + dot(row, p.q.data(), m);
            if (!std::isfinite(x)) return false;
            p.raw[i] = x;
            double y, slope;
            if (law.fet >= 0) {
                const auto& f = p.fets[law.fet];
                const double* gate = &p.voltageMap[(f.port + 1) * m];
                const double vds = x - law.resistance * p.q[i];
                const double vgs = p.open[f.port + 1] + dot(gate, p.q.data(), m);
                const auto e = comp_.jfet[f.device].model.evaluate(vgs, vds);
                if (!e.valid) return false;
                y = 1000.0 * (e.id - f.offset - f.conductance*vds - f.transconductance*vgs);
                const double dds = 1000.0 * (e.did_ds - f.conductance);
                const double dgs = 1000.0 * (e.did_gs - f.transconductance);
                for (int j = 0; j < m; ++j) {
                    const double ds = j == i ? -law.resistance : row[j];
                    p.matrix[i*m+j] = (i == j ? 1.0 : 0.0) - dds*ds - dgs*gate[j];
                }
                p.correction[i] = y - p.q[i];
                error = std::max(error, std::abs(p.correction[i]));
                continue;
            }
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
        if (iteration == maxCorrections || !detail::factorLUData(p.matrix.data(), p.pivots.data(), m))
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
        const double remaining = maxCorrections - iteration;
        for (int j = 0; j < m; ++j) {
            const double step = fraction * p.correction[j];
            double limit = 8.0;
            // Diode unknowns use mA. A large physical current can lie beyond
            // the distance the remaining bounded corrections could traverse.
            if (p.laws[j].diode >= 0 && std::abs(step) > limit * remaining)
                limit = std::min(8.0 * maxCorrections, std::abs(step) / remaining);
            p.q[j] += std::clamp(step, -limit, limit);
        }
    }

    for (int i = 0; i < k; ++i) {
        vt_[i] = p.open[i] + dot(&p.voltageMap[i * m], p.q.data(), m);
        if (!std::isfinite(vt_[i])) return false;
    }
    iv_ = p.constant;
    for (const auto& e : p.linear) iv_[e.row] += e.value * vt_[e.col];
    for (const auto& pole : p.linearPoles) {
        const auto& a = opAmps_[pole.device];
        if (a.softRails) return false;
        const double previous = opAmpVp_[pole.device];
        const double demand = pole.sourceGain * (vt_[pole.port+1] - vt_[pole.port+2]) + pole.historyGain * previous;
        const double slew = opAmpMaxDv_[pole.device];
        if (demand < a.railLow() || demand > a.railHigh() || (slew > 0.0 && std::abs(demand - previous) > slew))
            return false;
        iv_[pole.port] += pole.currentGain * pole.historyGain * previous;
    }
    for (const auto& guard : p.reverseCutoffs) {
        const auto& b = bjts_[guard.device];
        const double vbc = (b.pnp ? -1.0 : 1.0) * (vt_[guard.port + 1] - vt_[guard.port]);
        if (!(vbc <= guard.maxVbc)) return false;
    }
    for (const auto& buffer : p.buffers) {
        const auto& b = bjts_[buffer.device];
        const double sign = b.pnp ? -1.0 : 1.0;
        const double vbe = sign * vt_[buffer.port + 1];
        if (std::abs(vbe - buffer.vbe) > .012 || vbe - sign * vt_[buffer.port] > .15)
            return false;
    }
    for (const auto& e : p.injection) iv_[e.row] += e.value * p.q[e.col];
    for (const auto& law : p.laws) {
        if (law.string >= 0) {
            const auto& pair = p.strings[law.string];
            const double v = vt_[law.sense.port];
            if (v < -1.5 || v > 1.5) return false;
            const double position = (v + 1.5)*1000.0;
            const auto j = std::min(static_cast<size_t>(position), pair.firstDrop.size()-2);
            const double first = pair.firstDrop[j] + (position-static_cast<double>(j))*(pair.firstDrop[j+1]-pair.firstDrop[j]);
            const double second = -v - first;
            double current, derivative;
            comp_.diode[pair.first].second.eval(first, current, derivative);
            const double total = current + pair.firstShunt*first;
            iv_[pair.firstPort] = current;
            iv_[pair.secondPort] = total - pair.secondShunt*second;
            iv_[law.sense.port] += total;
            vt_[pair.firstPort] = first;
            vt_[pair.secondPort] = second;
            continue;
        }
        if (law.opposite < 0) continue;
        double reverse, conductance;
        comp_.diode[law.opposite].second.eval(-vt_[law.sense.port], reverse, conductance);
        iv_[law.oppositePort] = reverse;
        iv_[law.sense.port] += reverse;
    }
    // The diagnostic is in port volts. The table interpolates intervals of
    // 1 mV and the FET guard bounds its loaded tangent error at 1 mV.
    if (changed || hasAdjustedLiveTransfer()) {
        for (int row = 0; row < k; ++row) {
            double residual = vt_[row] - vOc_[row];
            for (const int col : rootLayout_.redCol)
                residual += Z_[row * k + col] * iv_[col];
            if (!std::isfinite(residual) || std::abs(residual) > .001)
                return false;
        }
    }
    commitCompiledRoot(.001);
    p.seed = false;
    p.misses = p.cooldown = 0;
    ++loadedJunctionHits_;
    return true;
}

} // namespace wdf
