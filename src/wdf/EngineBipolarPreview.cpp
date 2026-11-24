#include "wdf/Engine.h"
#include "wdf/BipolarPreviewQualification.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>

namespace wdf {
namespace {
using Values = std::vector<double>;
constexpr double qualificationPeak = 1.2;

bool allFinite(const Values& x) {
    return std::all_of(x.begin(), x.end(), [](double v) { return std::isfinite(v); });
}

struct Rows {
    int width{};
    Values data;

    Rows() = default;
    Rows(int rows, int cols) : width(cols), data(size_t(rows) * cols) {}
    double* operator[](int i) { return data.data() + size_t(i) * width; }
    const double* operator[](int i) const { return data.data() + size_t(i) * width; }
};

double dot(const double* row, const double* x, int count) noexcept {
    double a = 0., b = 0., c = 0., d = 0.;
    int i = 0;
    for (; i + 3 < count; i += 4) {
        a += row[i] * x[i];
        b += row[i + 1] * x[i + 1];
        c += row[i + 2] * x[i + 2];
        d += row[i + 3] * x[i + 3];
    }
    for (; i < count; ++i) a += row[i] * x[i];
    return (a + b) + (c + d);
}

double coefficient(const AffineOffset& offset, size_t input) {
    double sum = 0;
    for (auto [index, value] : offset.coef)
        if (size_t(index) == input) sum += value;
    return sum;
}
} // namespace

struct Engine::BipolarPreview {
    long long transfer{}, controls{};
    int states{};
    bool active = false, tripped = false, qualifying = false;
    double biasInput{}, biasOutput{}, scale = 1., peak = qualificationPeak;
    int rearmFrames = 0, quietFrames = 0;
    Values biasNodes;
    Values biasState, biasVoltage, biasCurrent, jacobian;
    Rows voltage, transition, handoffNodes, handoffRoots;
    Values drive, next, terminals, actualCurrent, acceptedDrive;
    Values defectLimit, stateLimit;
    std::vector<size_t> devices;

    struct ControlPort {
        int element, control;
        double reference;
        bool keepsBias;
    };
    std::vector<ControlPort> ports;
    struct Range {
        double low = 1., high = 0.;
        Values stateLimit;
    };
    static constexpr double movingPeak = .8;
    std::vector<Range> qualifiedControls;
    Rows branchVoltage;
    std::array<int, 2> moving{};
    std::array<double, 4> feedback{};
    int movingCount{};
    bool supportsMotion{};

    // Extra currents through the changed resistor legs close the small port loop
    // around the *loaded* transistor tangent. The wave histories remain physical.
    bool updateControlPorts(const Network& net, const Circuit& circuit) noexcept {
        movingCount = 0;
        std::array<double, 2> delta{};
        for (int n = 0; n < int(ports.size()); ++n) {
            const auto& p = ports[n];
            if (net.eG[p.element] == p.reference) continue;
            const auto& range = qualifiedControls[p.control];
            const double position = p.control < int(circuit.pots.size()) ? circuit.pots[p.control].position
                : circuit.varRes[p.control - int(circuit.pots.size())].position;
            if (!p.keepsBias || !(position >= range.low && position <= range.high) || movingCount == 2 ||
                (movingCount && p.control != ports[moving[0]].control)) return false;
            delta[movingCount] = (net.eG[p.element] - p.reference) / p.reference;
            moving[movingCount++] = n;
        }
        if (!movingCount) return true;
        const int start = transition.width - int(ports.size());
        const double a = 1. + delta[0] * branchVoltage[moving[0]][start + moving[0]];
        if (movingCount == 1) {
            feedback[0] = -delta[0] / a;
            return std::isfinite(feedback[0]) && a > 1e-12;
        }
        const double b = delta[0] * branchVoltage[moving[0]][start + moving[1]];
        const double c = delta[1] * branchVoltage[moving[1]][start + moving[0]];
        const double d = 1. + delta[1] * branchVoltage[moving[1]][start + moving[1]];
        const double det = a*d - b*c;
        if (!(a > 0. && d > 0. && det > 1e-12 * (1. + std::abs(a*d) + std::abs(b*c)))) return false;
        feedback = {-d*delta[0]/det, b*delta[1]/det, c*delta[0]/det, -a*delta[1]/det};
        return std::all_of(feedback.begin(), feedback.end(), [](double v) { return std::isfinite(v); });
    }

    void applyControlPorts(bool includeDefects) noexcept {
        const int start = transition.width - int(ports.size());
        const int count = includeDefects ? start : states + 2;
        double drop[2]{};
        for (int i = 0; i < movingCount; ++i)
            drop[i] = dot(branchVoltage[moving[i]], drive.data(), count);
        drive[start + moving[0]] = feedback[0] * drop[0] + (movingCount == 2 ? feedback[1] * drop[1] : 0.);
        if (movingCount == 2)
            drive[start + moving[1]] = feedback[2] * drop[0] + feedback[3] * drop[1];
    }
};

bool Engine::prepareBipolarPreview(size_t inputSource, int outputNode, double requestedPeak, bool allowPad) {
    releaseBipolarPreview();
    bipolarPreview_.reset();
    const int nx = nCaps_ + nInds_;
    if (inputSource >= dcValues_.size() || outputNode < 0 || outputNode >= N_ ||
        !nBjt_ || K_ < 3 || K_ > 128 || nx < 1 || nx > 160 || nIsrc_ ||
        powerSagEnabled_ || hasAdjustedLiveTransfer() ||
        !photoresistors_.empty() || !vcas_.empty() || !otas_.empty() ||
        !bucketBrigades_.empty() || !rmsDetectors_.empty() ||
        std::any_of(rootDevices_.begin(), rootDevices_.end(), [](const auto& dev) {
            return dev.kind != DeviceKind::Bjt && dev.kind != DeviceKind::Diode;
        }) || std::any_of(circuit_.cap.begin(), circuit_.cap.end(), [](const auto& cap) {
            return cap.initialVolts.has_value();
        }))
        return false;

    const int k = K_, independent = int(rootLayout_.nodeCol.size());
    const int input = nx, constant = nx + 1, defect = nx + 2, control = defect + independent;
    auto work = std::make_shared<BipolarPreview>();
    auto& p = *work;
    p.states = nx;
    p.transfer = linearRebuilds();
    p.controls = liveControlRevision();
    p.biasInput = dcValues_[inputSource];

    // Reset a separate realization to obtain the actual circuit bias without
    // disturbing the capacitor waves of the instance currently playing.
    Engine bias(circuit_, fs_, gridCurrent_);
    Values dcNodes;
    if (!bias.solveDc(dcNodes, p.biasVoltage) || int(dcNodes.size()) < N_ ||
        int(p.biasVoltage.size()) != k || !allFinite(p.biasVoltage))
        return false;
    bias.currents(p.biasVoltage, p.biasCurrent, p.jacobian);
    p.biasState = bias.capState_;
    p.biasState.insert(p.biasState.end(), bias.indState_.begin(), bias.indState_.end());
    p.biasOutput = dcNodes[outputNode];
    p.biasNodes = dcNodes;
    if (!allFinite(p.biasState) || !allFinite(p.biasCurrent) || !allFinite(p.jacobian))
        return false;

    // Unknowns are deviations of physical waves, the input voltage, a constant,
    // and independent nonlinear current defects. Project the open-circuit drive
    // onto these columns, then close instantaneous feedback at the bias point.
    const int controlCount = int(circuit_.pots.size() + circuit_.varRes.size());
    // Two legs per potentiometer. Larger control banks keep the existing fallback.
    if (2 * circuit_.pots.size() + circuit_.varRes.size() <= 8) {
        p.qualifiedControls.resize(controlCount);
        const auto addPort = [&](int q, int owner) {
            if (q < 0) return;
            const int a = net_.epA[q], b = net_.epB[q];
            const double drop = (a < 0 ? 0. : dcNodes[a]) - (b < 0 ? 0. : dcNodes[b]) +
                                net_.eoffAff[q].eval(dcValues_);
            p.ports.push_back({q, owner, net_.eG[q], std::abs(drop) <= 1e-9 && net_.eG[q] > 0.});
        };
        for (int i = 0; i < int(circuit_.pots.size()); ++i) {
            addPort(potResA_[i], i);
            addPort(potResB_[i], i);
        }
        for (int i = 0; i < int(circuit_.varRes.size()); ++i)
            addPort(varResResIndex_[i], int(circuit_.pots.size()) + i);
    }
    const int columns = control + int(p.ports.size());
    Rows nodes(N_, columns), roots(k, columns), factor(k, k);
    for (int i = 0; i < int(p.ports.size()); ++i)
        for (int n = 0; n < N_; ++n)
            nodes[n][control + i] = linXe_[n][p.ports[i].element];
    const auto stateIndex = [&](int port) {
        return net_.capOf[port] >= 0 ? net_.capOf[port] : nCaps_ + net_.indOf[port];
    };
    for (size_t l = 0; l < net_.liveCols.size(); ++l) {
        const int port = net_.liveCols[l];
        const bool reactive = net_.capOf[port] >= 0 || net_.indOf[port] >= 0;
        const double sign = net_.capOf[port] >= 0 ? 1. : -1.;
        const double sourceDrive = coefficient(net_.eoffAff[port], inputSource);
        const double base = (reactive ? sign * p.biasState[stateIndex(port)] : 0.) -
                            net_.eoffAff[port].eval(dcValues_);
        for (int n = 0; n < N_; ++n) {
            const double gain = Xe_[size_t(n) * net_.liveCols.size() + l];
            if (reactive) nodes[n][stateIndex(port)] += sign * gain;
            nodes[n][input] -= gain * sourceDrive;
            nodes[n][constant] += gain * base;
        }
    }
    const auto difference = [&](int a, int b, int col) {
        return (a >= 0 && a < N_ ? nodes[a][col] : 0.) -
               (b >= 0 && b < N_ ? nodes[b][col] : 0.);
    };
    for (int r = 0; r < k; ++r) {
        const auto& port = rootPortInfo_[r];
        for (int c = 0; c < columns; ++c)
            roots[r][c] = difference(port.a, port.b, c);
        roots[r][input] += coefficient(net_.roffAff[r], inputSource);
        roots[r][constant] += net_.roffAff[r].eval(dcValues_) - p.biasVoltage[r];
        factor[r][r] = 1.;
        for (int j = 0; j < independent; ++j) {
            const int terminal = rootLayout_.nodeCol[j];
            const double z = Z_[size_t(r) * k + terminal];
            roots[r][defect + j] = -z;
            roots[r][constant] -= z * p.biasCurrent[terminal];
            for (int c = 0; c < k; ++c)
                factor[r][c] += z * p.jacobian[size_t(terminal) * k + c];
        }
    }
    const Rows unfactored = factor;
    std::vector<int> piv(k);
    if (!detail::factorLUData(factor.data.data(), piv.data(), k)) return false;
    Values rhs(k), currents(k);
    for (int c = 0; c < columns; ++c) {
        for (int r = 0; r < k; ++r) rhs[r] = roots[r][c];
        detail::solveLUData(factor.data.data(), piv.data(), rhs.data(), k);
        for (int r = 0; r < k; ++r) {
            // Check the floating-point solve before trusting a loaded tangent.
            double scale = std::abs(roots[r][c]);
            for (int t = 0; t < k; ++t) scale += std::abs(unfactored[r][t] * rhs[t]);
            const double residual = roots[r][c] - dot(unfactored[r], rhs.data(), k);
            if (!(std::abs(residual) <= 1e-10 * std::max(1., scale))) return false;
            roots[r][c] = rhs[r];
            currents[r] = dot(p.jacobian.data() + size_t(r) * k, rhs.data(), k);
            if (c == constant) currents[r] += p.biasCurrent[r];
        }
        if (c >= defect && c < control) currents[rootLayout_.nodeCol[c - defect]] += 1.;
        for (int n = 0; n < N_; ++n)
            for (int terminal : rootLayout_.nodeCol)
                nodes[n][c] -= Xr_[size_t(n) * k + terminal] * currents[terminal];
    }

    p.transition = Rows(nx + 1, columns);
    for (int q = 0; q < P_; ++q) {
        if (net_.capOf[q] < 0 && net_.indOf[q] < 0) continue;
        const int r = stateIndex(q);
        for (int c = 0; c < columns; ++c)
            p.transition[r][c] = 2. * difference(net_.epA[q], net_.epB[q], c);
        p.transition[r][input] += 2. * coefficient(net_.eoffAff[q], inputSource);
        p.transition[r][constant] += 2. * net_.eoffAff[q].eval(dcValues_);
        if (net_.capOf[q] >= 0) p.transition[r][constant] -= 2. * p.biasState[r];
        p.transition[r][r] += net_.capOf[q] >= 0 ? -1. : 1.;
    }
    std::copy_n(nodes[outputNode], columns, p.transition[nx]);
    p.transition[nx][constant] -= p.biasOutput;
    p.branchVoltage = Rows(int(p.ports.size()), columns);
    for (int i = 0; i < int(p.ports.size()); ++i) {
        const auto q = p.ports[i].element;
        for (int c = 0; c < columns; ++c)
            p.branchVoltage[i][c] = difference(net_.epA[q], net_.epB[q], c);
        p.branchVoltage[i][input] += coefficient(net_.eoffAff[q], inputSource);
        p.branchVoltage[i][constant] += net_.eoffAff[q].eval(dcValues_);
    }
    p.voltage = Rows(k, defect + int(p.ports.size()));
    for (int r = 0; r < k; ++r) {
        std::copy_n(roots[r], defect, p.voltage[r]);
        std::copy_n(roots[r] + control, p.ports.size(), p.voltage[r] + defect);
    }
    if (!allFinite(p.voltage.data) || !allFinite(p.transition.data) || !allFinite(p.branchVoltage.data)) return false;
    p.drive.resize(columns);
    p.drive[constant] = 1.;
    p.next.resize(nx);
    p.terminals.resize(k);
    p.actualCurrent.resize(k);
    p.acceptedDrive.resize(columns);
    p.handoffNodes = std::move(nodes);
    p.handoffRoots = std::move(roots);
    bipolarPreview_ = std::move(work);

    p.devices.resize(rootDevices_.size());
    std::iota(p.devices.begin(), p.devices.end(), size_t(0));

    // The native circuit supplies reference responses, never coefficients.
    bias.reset();
    auto sources = bias.dcValues();
    constexpr int samples = 1152;
    Values inputs(samples), reference(samples);
    for (int i = 0; i < samples; ++i) {
        const double phase = 2. * std::numbers::pi * i / fs_;
        const double level = i < 64 ? 0. : i < 384 ? .15 : i < 896 ? qualificationPeak : .25;
        inputs[i] = p.biasInput + level * (.64 * std::sin(137. * phase)
                                          + .24 * std::sin(937. * phase)
                                          + .12 * std::sin(std::min(5999., .17 * fs_) * phase));
        sources[inputSource] = inputs[i];
        reference[i] = bias.step(sources)[outputNode];
        if (bias.lastStateRejected() || !std::isfinite(reference[i])) {
            bipolarPreview_.reset();
            return false;
        }
    }
    Values measuredDefect;
    const auto qualify = [&](BipolarPreview& candidate) {
        const int terms = candidate.transition.width - int(candidate.ports.size()) - defect;
        Values observedDefect(terms), observedState(nx);
        double peakError = 0., signalEnergy = 0., errorEnergy = 0.;
        candidate.qualifying = true;
        candidate.active = true;
        candidate.tripped = false;
        std::fill_n(candidate.drive.begin(), nx, 0.);
        candidate.defectLimit.assign(terms, std::numeric_limits<double>::infinity());
        candidate.stateLimit.assign(nx, std::numeric_limits<double>::infinity());
        for (int i = 0; i < samples; ++i) {
            double output;
            if (!stepBipolarPreview(inputs[i], output)) return false;
            const double error = output - reference[i];
            peakError = std::max(peakError, std::abs(error));
            errorEnergy += error * error;
            signalEnergy += (reference[i] - p.biasOutput) * (reference[i] - p.biasOutput);
            for (int j = 0; j < terms; ++j)
                observedDefect[j] = std::max(observedDefect[j], std::abs(candidate.drive[defect + j]));
            for (int j = 0; j < nx; ++j)
                observedState[j] = std::max(observedState[j], std::abs(candidate.drive[j]));
        }
        if (signalEnergy < 1e-12 || peakError > .002 || errorEnergy > 1e-5 * signalEnergy)
            return false;
        if (&candidate == &p) measuredDefect = observedDefect;
        candidate.active = false;
        candidate.qualifying = false;
        for (int j = 0; j < terms; ++j)
            candidate.defectLimit[j] = std::max(1e-6, 4. * observedDefect[j]);
        for (int j = 0; j < nx; ++j)
            candidate.stateLimit[j] = std::max(.01, 8. * observedState[j]);
        return true;
    };
    if (!qualify(p)) {
        bipolarPreview_.reset();
        return false;
    }

    // Rank nonlinear current injections by their actual path to the selected
    // output through the reactive network. Every device keeps its DC Jacobian.
    // Reducing only the large-signal corrections needs another qualification.
    constexpr size_t keepDevices = 4;
    if (p.devices.size() > keepDevices) {
        Values projection(p.transition[nx], p.transition[nx] + nx), next(nx);
        Values weight(p.transition[nx] + defect, p.transition[nx] + control);
        for (double& w : weight) w = std::abs(w);
        for (int lag = 0; lag < 128; ++lag) {
            std::fill(next.begin(), next.end(), 0.);
            for (int r = 0; r < nx; ++r)
                for (int c = 0; c < nx; ++c) next[c] += projection[r] * p.transition[r][c];
            for (int c = 0; c < independent; ++c) {
                double value = 0.;
                for (int r = 0; r < nx; ++r) value += projection[r] * p.transition[r][defect + c];
                weight[c] += std::abs(value);
            }
            projection.swap(next);
        }
        Values rank(rootDevices_.size());
        int term = 0;
        for (size_t d = 0; d < rank.size(); ++d)
            for (int r = 0; r < rootDevices_[d].numActiveRows; ++r, ++term)
                rank[d] += weight[term] * measuredDefect[term];

        auto full = bipolarPreview_;
        auto reduced = std::make_shared<BipolarPreview>(p);
        auto& selected = reduced->devices;
        std::stable_sort(selected.begin(), selected.end(), [&](size_t a, size_t b) { return rank[a] > rank[b]; });
        selected.resize(keepDevices);
        std::sort(selected.begin(), selected.end());
        std::vector<int> use;
        term = 0;
        for (size_t d = 0; d < rootDevices_.size(); ++d) {
            const bool chosen = std::binary_search(selected.begin(), selected.end(), d);
            for (int r = 0; r < rootDevices_[d].numActiveRows; ++r, ++term)
                if (chosen) use.push_back(defect + term);
        }
        for (int c = control; c < columns; ++c) use.push_back(c);
        const auto compress = [&](const Rows& from) {
            const int n = int(from.data.size()) / from.width;
            Rows result(n, defect + int(use.size()));
            for (int r = 0; r < n; ++r) {
                std::copy_n(from[r], defect, result[r]);
                for (size_t i = 0; i < use.size(); ++i)
                    result[r][defect + i] = from[r][use[i]];
            }
            return result;
        };
        reduced->transition = compress(p.transition);
        reduced->branchVoltage = compress(p.branchVoltage);
        reduced->handoffRoots = compress(p.handoffRoots);
        reduced->handoffNodes = compress(p.handoffNodes);
        reduced->drive.resize(reduced->transition.width);
        reduced->acceptedDrive.resize(reduced->transition.width);
        bipolarPreview_ = std::move(reduced);
        if (!qualify(*bipolarPreview_)) bipolarPreview_ = std::move(full);
    }
    bipolarPreview_->rearmFrames = std::clamp(int(std::round(.002 * fs_)), 32, 1024);
    if (std::isfinite(requestedPeak) && requestedPeak > qualificationPeak && requestedPeak <= lim_.maxNodeVolts)
        qualifyBipolarPreviewRange(inputSource, outputNode, requestedPeak, allowPad);
    qualifyBipolarControlMotion(inputSource, outputNode);
    return true;
}

void Engine::qualifyBipolarControlMotion(size_t source, int output) {
    auto& plan = *bipolarPreview_;
    if (plan.scale != 1. || std::none_of(plan.ports.begin(), plan.ports.end(),
                                            [](const auto& p) { return p.keepsBias; })) return;
    // A zero DC drop certifies the unchanged bias. At that bias a knob may
    // change the AC feedback and which transistor corrections reach the output.
    // Screen each control using its actual two endpoints and an interior jump.
    Engine oracle(circuit_, fs_, gridCurrent_), trial(circuit_, fs_, gridCurrent_);
    trial.prepareLiveControls();
    trial.bipolarPreview_ = std::make_shared<BipolarPreview>(plan);
    auto& candidate = *trial.bipolarPreview_;
    auto sources = oracle.dcValues();
    std::vector<std::pair<int, double>> pos(1), empty;
    constexpr int segment = 256, frames = 4 * segment;
    Values expected(frames), rendered(frames);
    for (int k = 0; k < int(plan.qualifiedControls.size()); ++k) {
        if (std::none_of(plan.ports.begin(), plan.ports.end(), [k](const auto& p) { return p.control == k; }) ||
            std::any_of(plan.ports.begin(), plan.ports.end(),
                        [k](const auto& p) { return p.control == k && !p.keepsBias; })) continue;
        const bool isPot = k < int(circuit_.pots.size());
        const int index = isPot ? k : k - int(circuit_.pots.size());
        const double initial = isPot ? circuit_.pots[index].position : circuit_.varRes[index].position;
        const auto setPosition = [&](double value) {
            pos[0] = {index, value};
            return oracle.setParametersByIndex(isPot ? pos : empty, isPot ? empty : pos, empty) &&
                   trial.setLiveControls(isPot ? pos : empty, isPot ? empty : pos);
        };
        const auto qualifies = [&](BipolarPreview::Range& range) {
            // Changing a filter can raise stored energy at the same external level.
            // Measure capacitor/inductor excursions with the original nonlinear
            // circuit, then check a separate drive against those limits and waves.
            range.stateLimit = plan.stateLimit;
            const double sequence[] = {initial, range.low, range.high, range.low + .37 * (range.high - range.low)};
            const auto wave = [&](int r) {
                return r < nCaps_ ? oracle.capState_[r] : oracle.indState_[r - nCaps_];
            };
            for (bool verify : {false, true}) {
                if (!setPosition(initial)) return false;
                trial.reset();
                oracle.reset();
                if (verify) candidate.qualifiedControls[k] = range;
                for (int i = 0; i < frames; ++i) {
                    if (i % segment == 0 && !setPosition(sequence[i / segment])) return false;
                    const double t = 2. * std::numbers::pi * i / fs_;
                    const double drive = verify
                        ? .41 * std::sin(257. * t + .3) + .34 * std::sin(1249. * t) +
                          .25 * std::sin(std::min(5483., .17 * fs_) * t + .2)
                        : .43 * std::sin(311. * t) + .32 * std::sin(1387. * t) +
                          .25 * std::sin(std::min(6421., .18 * fs_) * t);
                    sources[source] = plan.biasInput + BipolarPreview::movingPeak * (i % segment == 127 ? 1. : drive);
                    expected[i] = oracle.step(sources)[output] - plan.biasOutput;
                    if (!oracle.lastConverged() || oracle.lastStateRejected()) return false;
                    if (verify) {
                        if (!trial.stepBipolarPreview(sources[source], rendered[i])) return false;
                        rendered[i] -= plan.biasOutput;
                    }
                    for (int r = 0; r < plan.states; ++r) {
                        const double physical = wave(r) - plan.biasState[r];
                        if (!std::isfinite(physical)) return false;
                        if (!verify)
                            range.stateLimit[r] = std::max(range.stateLimit[r], 1.25 * std::abs(physical) + 50e-6);
                        else if (std::abs(candidate.drive[r] - physical) > .001 + .015 * std::abs(physical))
                            return false;
                    }
                }
            }
            return engine::bipolarPreviewWaveformMatches(expected, rendered);
        };
        // An EQ can remain nearly linear around noon and overload at an end stop.
        for (double radius : {1., .125}) {
            BipolarPreview::Range range{std::max(0., initial - radius), std::min(1., initial + radius), {}};
            const bool passed = qualifies(range);
            candidate.qualifiedControls[k] = {};
            if (!setPosition(initial)) return;
            if (passed) {
                plan.qualifiedControls[k] = std::move(range);
                break;
            }
        }
    }
}

void Engine::qualifyBipolarPreviewRange(size_t source, int output, double peak, bool allowPad) {
    using Clock = std::chrono::steady_clock;
    const engine::BipolarPreviewStimulus stimulus(fs_, peak);
    const int w = stimulus.period, frames = int(stimulus.input.size());
    const auto& plan = *bipolarPreview_;
    struct Handoff {
        int frame;
        Values nodes, caps, inds;
    };
    std::array<Handoff, 3> handoffs{{{5*w + w/4, {}, {}, {}},
                                     {8*w + w/3, {}, {}, {}},
                                     {10*w + w/8 - 1, {}, {}, {}}}};
    Engine trial(circuit_, fs_, gridCurrent_);
    trial.bipolarPreview_ = std::make_shared<BipolarPreview>(plan);
    auto& candidate = *trial.bipolarPreview_;
    candidate.transfer = trial.linearRebuilds();
    candidate.controls = trial.liveControlRevision();
    candidate.peak = peak;
    candidate.rearmFrames = plan.rearmFrames;

    // An in-range guard trip ahead of the first required handoff disqualifies
    // a scale independently of the reference waveform or the speed screen.
    constexpr std::array allScales{1., .5, .25};
    auto scales = std::span(allScales).first(allowPad ? allScales.size() : 1);
    const auto prefix = std::span(stimulus.input).first(handoffs.front().frame);
    while (!scales.empty()) {
        trial.reset();
        candidate.scale = scales.front();
        if (std::ranges::all_of(prefix, [&](double input) {
            double outputVolts;
            return trial.stepBipolarPreview(plan.biasInput + input, outputVolts);
        })) break;
        scales = scales.subspan(1);
    }
    if (scales.empty()) return;

    Engine oracle(circuit_, fs_, gridCurrent_);
    if (oracle.Z_ != Z_) return;
    auto sources = oracle.dcValues();
    Values expected(frames), rendered(frames);
    for (int i = 0; i < frames; ++i) {
        sources[source] = plan.biasInput + stimulus.input[i];
        const auto& nodes = oracle.step(sources);
        if (!oracle.lastConverged() || oracle.lastStateRejected() || !std::isfinite(nodes[output])) return;
        expected[i] = nodes[output] - plan.biasOutput;
        for (auto& state : handoffs) {
            if (state.frame != i) continue;
            state.nodes = nodes;
            state.caps = oracle.capState_;
            state.inds = oracle.indState_;
        }
    }

    // Time the existing Fast Live root at the same hot two-tone points. The
    // complete Accurate trajectory above remains the independent sound reference.
    Engine fast(circuit_, fs_, gridCurrent_);
    fast.prepareCircuitPlan();
    fast.useCircuitPlan(true);
    fast.useMixedAffineReduction(true);
    constexpr int block = 64;
    const int timedStart = 6*w, timedEnd = 9*w;
    Values physicalTimes;
    for (int begin = timedStart; begin + block <= timedEnd; begin += block) {
        const auto start = Clock::now();
        for (int i = begin; i < begin + block; ++i) {
            sources[source] = plan.biasInput + stimulus.input[i];
            fast.step(sources, kDefaultMaxIterations, kFastLiveTolerance);
        }
        physicalTimes.push_back(std::chrono::duration<double>(Clock::now() - start).count());
    }
    std::sort(physicalTimes.begin(), physicalTimes.end());

    const auto closeState = [](double actual, double reference, double dc) {
        return std::isfinite(actual) && std::abs(actual - reference) <= .015 + .025 * std::abs(reference - dc);
    };
    const auto checkHandoff = [&](const Handoff& state) {
        trial.releaseBipolarPreview();
        for (int n = 0; n < N_; ++n)
            if (!closeState(trial.node_[n], state.nodes[n], plan.biasNodes[n])) return false;
        for (int n = 0; n < nCaps_; ++n)
            if (!closeState(trial.capState_[n], state.caps[n], plan.biasState[n])) return false;
        for (int n = 0; n < nInds_; ++n)
            if (!closeState(trial.indState_[n], state.inds[n], plan.biasState[nCaps_ + n])) return false;
        return true;
    };

    const auto passes = [&](double scale) {
        trial.reset();
        candidate.scale = scale;
        bool physicalSample = false;
        Values times;
        auto start = Clock::now();
        for (int i = 0; i < frames; ++i) {
            const int offset = i - timedStart;
            if (offset >= 0 && i < timedEnd && offset % block == 0) start = Clock::now();
            sources[source] = plan.biasInput + stimulus.input[i];
            double y;
            const bool active = !physicalSample && trial.stepBipolarPreview(sources[source], y);
            if (!active) {
                // Force a real full-drive step after a measured state handoff.
                // A deliberate over-range impulse must fall back and later rearm.
                const bool inRecovery = i >= stimulus.overloadSection*w && i <= stimulus.overloadSection*w + candidate.rearmFrames;
                if (!physicalSample && !inRecovery) return false;
                y = trial.step(sources, kDefaultMaxIterations, kFastLiveTolerance)[output];
                if (!trial.lastConverged() || trial.lastStateRejected()) return false;
            } else if (i == stimulus.overloadSection*w) {
                return false;
            }
            physicalSample = false;
            rendered[i] = y - plan.biasOutput;
            if (offset >= 0 && i < timedEnd && offset % block == block - 1)
                times.push_back(std::chrono::duration<double>(Clock::now() - start).count());
            for (const auto& state : handoffs) {
                if (state.frame != i) continue;
                if (!checkHandoff(state)) return false;
                physicalSample = true;
            }
        }
        std::sort(times.begin(), times.end());
        const size_t mid = times.size()/2, q3 = 3*times.size()/4;
        return times[mid] < .8 * physicalTimes[mid] && times[q3] < .95 * physicalTimes[q3] &&
               engine::bipolarPreviewWaveformMatches(expected, rendered) &&
               stimulus.matchesColor(expected, rendered);
    };
    for (double scale : scales) {
        if (!passes(scale)) continue;
        bipolarPreview_->scale = scale;
        bipolarPreview_->peak = peak;
        bipolarPreview_->rearmFrames = candidate.rearmFrames;
        return;
    }
}

bool Engine::stepBipolarPreview(double inputVolts, double& outputVolts) noexcept {
    if (!bipolarPreview_) return false;
    auto& p = *bipolarPreview_;
    if (p.transfer != linearRebuilds()) {
        releaseBipolarPreview();
        return false;
    }
    if (p.controls != liveControlRevision()) {
        std::fill(p.drive.end() - p.ports.size(), p.drive.end(), 0.);
        p.supportsMotion = !p.ports.empty() && p.updateControlPorts(net_, circuit_);
        if (!p.supportsMotion) p.movingCount = 0;
        p.controls = liveControlRevision();
    }
    if (hasAdjustedLiveTransfer() && !p.supportsMotion) {
        releaseBipolarPreview();
        return false;
    }
    const double input = inputVolts - p.biasInput;
    if (p.tripped) {
        if (!p.rearmFrames) return false;
        if (!lastConverged() || lastStateRejected_ || !(std::abs(input) <= p.peak / 8.)) p.quietFrames = 0;
        else if (++p.quietFrames >= p.rearmFrames) p.tripped = false;
        if (p.tripped) return false;
    }
    const auto reject = [&]() {
        releaseBipolarPreview();
        p.tripped = true;
        p.quietFrames = 0;
        return false;
    };
    // The worker uses double test voltages and the host supplies rounded float samples.
    const double peak = p.movingCount ? std::min(p.peak, BipolarPreview::movingPeak) : p.peak;
    if (!(std::abs(input) <= std::max(peak, double(static_cast<float>(peak))))) return reject();

    const int nx = p.states, defect = nx + 2;
    const int control = p.transition.width - int(p.ports.size());
    const int sampleColumns = p.movingCount ? p.transition.width : control;
    if (!p.active) {
        for (int i = 0; i < nCaps_; ++i) p.drive[i] = p.scale * (capState_[i] - p.biasState[i]);
        for (int i = 0; i < nInds_; ++i)
            p.drive[nCaps_ + i] = p.scale * (indState_[i] - p.biasState[nCaps_ + i]);
    }
    p.drive[nx] = input * p.scale;
    if (p.movingCount) p.applyControlPorts(false);
    int column = 0;
    for (size_t index : p.devices) {
        const auto& dev = rootDevices_[index];
        const int base = dev.portBase;
        // The BJT law uses Vce and Vbe. Its third current is their return path.
        for (int r = base; r < base + dev.numActiveRows; ++r) {
            double voltage = dot(p.voltage[r], p.drive.data(), defect);
            for (int i = 0; i < p.movingCount; ++i)
                voltage += p.voltage[r][defect + p.moving[i]] * p.drive[control + p.moving[i]];
            p.terminals[r] = p.biasVoltage[r] + voltage;
        }
        currentsForDevice(dev, p.terminals.data(), p.actualCurrent.data(), nullptr);
        for (int r = base; r < base + dev.numActiveRows; ++r, ++column) {
            double correction = p.actualCurrent[r] - p.biasCurrent[r];
            for (int c = base; c < base + dev.numActiveRows; ++c)
                correction -= p.jacobian[size_t(r) * K_ + c] * (p.terminals[c] - p.biasVoltage[c]);
            if (!(std::abs(correction) <= p.defectLimit[column])) return reject();
            p.drive[defect + column] = correction;
        }
    }
    if (p.movingCount) p.applyControlPorts(true);
    const auto& stateLimit = p.movingCount ? p.qualifiedControls[p.ports[p.moving[0]].control].stateLimit : p.stateLimit;
    for (int r = 0; r < nx; ++r) {
        p.next[r] = dot(p.transition[r], p.drive.data(), sampleColumns);
        if (!(std::abs(p.next[r]) <= stateLimit[r])) return reject();
    }
    const double y = p.biasOutput + dot(p.transition[nx], p.drive.data(), sampleColumns) / p.scale;
    if (!(std::abs(y) <= lim_.maxNodeVolts)) return reject();

    std::copy(p.drive.begin(), p.drive.end(), p.acceptedDrive.begin());
    std::copy(p.next.begin(), p.next.end(), p.drive.begin());
    p.active = true;
    outputVolts = y;
    return true;
}

double Engine::bipolarPreviewInputScale() const noexcept {
    return bipolarPreview_ ? bipolarPreview_->scale : 1.;
}

double Engine::bipolarPreviewPeakInput() const noexcept {
    return bipolarPreview_ ? bipolarPreview_->peak : 0.;
}

bool Engine::bipolarPreviewActive() const noexcept {
    return bipolarPreview_ && bipolarPreview_->active;
}

void Engine::releaseBipolarPreview() noexcept {
    if (!bipolarPreviewActive() || bipolarPreview_->qualifying) return;
    auto& p = *bipolarPreview_;
    for (int j = 0; j < nCaps_; ++j) capState_[j] = p.biasState[j] + p.drive[j] / p.scale;
    for (int j = 0; j < nInds_; ++j)
        indState_[j] = p.biasState[j + nCaps_] + p.drive[j + nCaps_] / p.scale;

    // Reconstruct the last accepted sample, including its current correction.
    // A rejected input still belongs to the normal solver when this returns.
    for (int r = 0; r < K_; ++r)
        vv_[r] = p.biasVoltage[r] + dot(p.handoffRoots[r], p.acceptedDrive.data(), p.handoffRoots.width) / p.scale;
    for (const auto& dev : rootDevices_)
        currentsForDevice(dev, vv_.data(), ii_.data(), nullptr);
    vvPrev_ = vv_;
    vvPrev2_ = vv_;
    for (int n = 0; n < N_; ++n) {
        const double node = dot(p.handoffNodes[n], p.acceptedDrive.data(), p.handoffNodes.width);
        node_[n] = p.biasNodes[n] + (node - p.biasNodes[n]) / p.scale;
    }
    lastStateRejected_ = false;
    consecutiveStateRejects_ = 0;
    p.active = false;
}

void Engine::resetBipolarPreviewHistory() noexcept {
    bipolarPreview_->active = false;
    bipolarPreview_->tripped = false;
    bipolarPreview_->quietFrames = 0;
}

void Engine::adoptBipolarPreview(Engine& prepared) noexcept {
    releaseBipolarPreview();
    bipolarPreview_.swap(prepared.bipolarPreview_);
    if (bipolarPreview_) {
        bipolarPreview_->transfer = linearRebuilds();
        bipolarPreview_->controls = liveControlRevision();
    }
}
} // namespace wdf
