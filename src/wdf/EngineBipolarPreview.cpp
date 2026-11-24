#include "wdf/Engine.h"

#include <cmath>
#include <numbers>

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
    double biasInput{}, biasOutput{};
    Values biasState, biasVoltage, biasCurrent, jacobian;
    Rows voltage, transition, handoffNodes, handoffRoots;
    Values drive, next, terminals, actualCurrent, acceptedDrive;
    Values defectLimit, stateLimit;
};

bool Engine::prepareBipolarPreview(size_t inputSource, int outputNode) {
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
    const int input = nx, constant = nx + 1, defect = nx + 2, columns = defect + independent;
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
    if (!allFinite(p.biasState) || !allFinite(p.biasCurrent) || !allFinite(p.jacobian))
        return false;

    // Unknowns are deviations of physical waves, the input voltage, a constant,
    // and independent nonlinear current defects. Project the open-circuit drive
    // onto these columns, then close instantaneous feedback at the bias point.
    Rows nodes(N_, columns), roots(k, columns), factor(k, k);
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
        if (c >= defect) currents[rootLayout_.nodeCol[c - defect]] += 1.;
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
    p.voltage = Rows(k, defect);
    for (int r = 0; r < k; ++r) std::copy_n(roots[r], defect, p.voltage[r]);
    if (!allFinite(p.voltage.data) || !allFinite(p.transition.data)) return false;
    p.drive.resize(columns);
    p.drive[constant] = 1.;
    p.next.resize(nx);
    p.terminals.resize(k);
    p.actualCurrent.resize(k);
    p.acceptedDrive.resize(columns);
    p.handoffNodes = std::move(nodes);
    p.handoffRoots = std::move(roots);
    bipolarPreview_ = std::move(work);

    // Qualify this fixed control snapshot against the native nonlinear circuit
    // at two drive levels. The fitted quantities are guard ranges only.
    bias.reset();
    auto sources = bias.dcValues();
    p.qualifying = true;
    p.active = true;
    p.defectLimit.assign(independent, std::numeric_limits<double>::infinity());
    p.stateLimit.assign(nx, std::numeric_limits<double>::infinity());
    Values observedDefect(independent), observedState(nx);
    double peakError = 0., signalEnergy = 0., errorEnergy = 0.;
    constexpr int samples = 1152;
    for (int i = 0; i < samples; ++i) {
        const double phase = 2. * std::numbers::pi * i / fs_;
        const double level = i < 64 ? 0. : i < 384 ? .15 : i < 896 ? qualificationPeak : .25;
        const double x = p.biasInput + level * (.64 * std::sin(137. * phase)
                                               + .24 * std::sin(937. * phase)
                                               + .12 * std::sin(std::min(5999., .17 * fs_) * phase));
        sources[inputSource] = x;
        const double reference = bias.step(sources)[outputNode];
        double output;
        if (!stepBipolarPreview(x, output) || bias.lastStateRejected() || !std::isfinite(reference)) {
            bipolarPreview_.reset();
            return false;
        }
        const double error = output - reference;
        peakError = std::max(peakError, std::abs(error));
        errorEnergy += error * error;
        signalEnergy += (reference - p.biasOutput) * (reference - p.biasOutput);
        for (int j = 0; j < independent; ++j)
            observedDefect[j] = std::max(observedDefect[j], std::abs(p.drive[defect + j]));
        for (int j = 0; j < nx; ++j)
            observedState[j] = std::max(observedState[j], std::abs(p.drive[j]));
    }
    if (signalEnergy < 1e-12 || peakError > .002 || errorEnergy > 1e-5 * signalEnergy) {
        bipolarPreview_.reset();
        return false;
    }
    p.active = false;
    p.qualifying = false;
    for (int j = 0; j < independent; ++j)
        p.defectLimit[j] = std::max(1e-6, 4. * observedDefect[j]);
    for (int j = 0; j < nx; ++j)
        p.stateLimit[j] = std::max(.01, 8. * observedState[j]);
    return true;
}

bool Engine::stepBipolarPreview(double inputVolts, double& outputVolts) noexcept {
    if (!bipolarPreview_) return false;
    auto& p = *bipolarPreview_;
    if (p.tripped || p.transfer != linearRebuilds() || p.controls != liveControlRevision() ||
        hasAdjustedLiveTransfer()) {
        releaseBipolarPreview();
        return false;
    }
    const auto reject = [&]() {
        releaseBipolarPreview();
        p.tripped = true;
        return false;
    };
    if (!(std::abs(inputVolts - p.biasInput) <= static_cast<float>(qualificationPeak)))
        return reject();

    const int nx = p.states, defect = nx + 2;
    if (!p.active) {
        for (int i = 0; i < nCaps_; ++i) p.drive[i] = capState_[i] - p.biasState[i];
        for (int i = 0; i < nInds_; ++i)
            p.drive[nCaps_ + i] = indState_[i] - p.biasState[nCaps_ + i];
    }
    p.drive[nx] = inputVolts - p.biasInput;
    int column = 0;
    for (const auto& dev : rootDevices_) {
        const int base = dev.portBase;
        // The BJT law uses Vce and Vbe. Its third current is their return path.
        for (int r = base; r < base + dev.numActiveRows; ++r)
            p.terminals[r] = p.biasVoltage[r] + dot(p.voltage[r], p.drive.data(), defect);
        currentsForDevice(dev, p.terminals.data(), p.actualCurrent.data(), nullptr);
        for (int r = base; r < base + dev.numActiveRows; ++r, ++column) {
            double correction = p.actualCurrent[r] - p.biasCurrent[r];
            for (int c = base; c < base + dev.numActiveRows; ++c)
                correction -= p.jacobian[size_t(r) * K_ + c] * (p.terminals[c] - p.biasVoltage[c]);
            if (!(std::abs(correction) <= p.defectLimit[column])) return reject();
            p.drive[defect + column] = correction;
        }
    }
    for (int r = 0; r < nx; ++r) {
        p.next[r] = dot(p.transition[r], p.drive.data(), p.transition.width);
        if (!(std::abs(p.next[r]) <= p.stateLimit[r])) return reject();
    }
    const double y = p.biasOutput + dot(p.transition[nx], p.drive.data(), p.transition.width);
    if (!(std::abs(y) <= lim_.maxNodeVolts)) return reject();

    std::copy(p.drive.begin(), p.drive.end(), p.acceptedDrive.begin());
    std::copy(p.next.begin(), p.next.end(), p.drive.begin());
    p.active = true;
    outputVolts = y;
    return true;
}

bool Engine::bipolarPreviewActive() const noexcept {
    return bipolarPreview_ && bipolarPreview_->active;
}

void Engine::releaseBipolarPreview() noexcept {
    if (!bipolarPreviewActive() || bipolarPreview_->qualifying) return;
    auto& p = *bipolarPreview_;
    for (int j = 0; j < nCaps_; ++j) capState_[j] = p.biasState[j] + p.drive[j];
    for (int j = 0; j < nInds_; ++j)
        indState_[j] = p.biasState[j + nCaps_] + p.drive[j + nCaps_];

    // Reconstruct the last accepted sample, including its current correction.
    // A rejected input still belongs to the normal solver when this returns.
    for (int r = 0; r < K_; ++r)
        vv_[r] = p.biasVoltage[r] + dot(p.handoffRoots[r], p.acceptedDrive.data(), p.handoffRoots.width);
    for (const auto& dev : rootDevices_)
        currentsForDevice(dev, vv_.data(), ii_.data(), nullptr);
    vvPrev_ = vv_;
    vvPrev2_ = vv_;
    for (int n = 0; n < N_; ++n)
        node_[n] = dot(p.handoffNodes[n], p.acceptedDrive.data(), p.handoffNodes.width);
    lastStateRejected_ = false;
    consecutiveStateRejects_ = 0;
    p.active = false;
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
