#include "wdf/Engine.h"

#include <array>
#include <map>
#include <numbers>

namespace wdf {

size_t Engine::prepareMixedAffineReduction(size_t inputSource, double peakVolts) {
    if (blockCount_ != 1)
        return 0;
    auto& block = newtonBlocks_.front();
    block.alternateLinearSlopes.clear();
    // Reconstructing the original voltages costs more than removing a
    // single row in a large coupled block. Skip small reductions.
    if (!explicitRootsAllowed_ || inputSource >= circuit_.src.size() ||
        !(peakVolts > 0.0) || !std::isfinite(peakVolts * 8.0) ||
        !block.linearReduction || block.linearDevices.empty() || block.reducedPorts.size() < 3 ||
        block.linearDevices.size() * 8 < block.ports.size())
        return 0;
    for (int index : block.linearDevices) {
        const auto& dev = rootDevices_[static_cast<size_t>(index)];
        if (dev.kind != DeviceKind::OpAmp || opAmps_[dev.compIndex].hasClassABOutput() ||
            opAmps_[dev.compIndex].softRails)
            return 0;
    }
    for (int index : block.reducedDevices) {
        const auto kind = rootDevices_[static_cast<size_t>(index)].kind;
        if (kind != DeviceKind::Diode && kind != DeviceKind::Bjt &&
            kind != DeviceKind::Jfet && kind != DeviceKind::Mosfet)
            return 0;
    }

    Engine calibration(circuit_, fs_);
    if (calibration.blockCount_ != 1 ||
        calibration.newtonBlocks_.front().linearDevices != block.linearDevices)
        return 0;
    auto patterns = calibration.measureAffineSlopes(inputSource, peakVolts);
    std::stable_sort(patterns.begin(), patterns.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });
    constexpr size_t planLimit = 5;
    for (const auto& [mask, count] : patterns) {
        if (block.alternateLinearSlopes.size() == planLimit || count < 16)
            break;
        NewtonBlock plan;
        plan.devices = block.devices;
        if (solver::RootLinearPlan::build(plan, rootDevices_.data(), Z_.data(), K_,
                                         comp_, opAmps_, ctrlResValues_, fs_, mask))
            block.alternateLinearSlopes.emplace_back(std::move(plan));
    }
    return block.alternateLinearSlopes.size();
}

std::vector<std::pair<unsigned, int>> Engine::measureAffineSlopes(size_t inputSource, double peakVolts) {
    if (blockCount_ != 1 || !newtonBlocks_.front().linearReduction)
        return {};
    const auto& block = newtonBlocks_.front();
    std::vector<double> sources = dcValues_;
    for (const auto& source : circuit_.isrc)
        sources.push_back(source.dc);
    std::vector<double> previous(opAmpVp_.size());
    std::map<unsigned, int> counts;
    constexpr std::array levels{0.0, 1.0 / 24.0, 1.0 / 6.0, 1.0 / 3.0, 1.0, 2.0, 4.0, 8.0};
    constexpr int segment = 1024;
    for (int i = 0; i < segment * static_cast<int>(levels.size()); ++i) {
        const double t = static_cast<double>(i % segment) / segment;
        const double envelope = std::min({1.0, 12.0 * t, 12.0 * (1.0 - t)});
        const double phase = 2.0 * std::numbers::pi * i / fs_;
        const double signal = 0.6 * std::sin(137.0 * phase) + 0.27 * std::sin(701.0 * phase) +
                              0.13 * std::sin(1921.0 * phase);
        sources[inputSource] = dcValues_[inputSource] + peakVolts * levels[i / segment] * envelope * signal;
        std::copy(opAmpVp_.begin(), opAmpVp_.end(), previous.begin());
        step(sources);
        if (lastStateRejected() || !lastConverged())
            continue;
        unsigned mask = 0;
        for (size_t d = 0; d < block.linearDevices.size(); ++d) {
            const auto& dev = rootDevices_[static_cast<size_t>(block.linearDevices[d])];
            const auto ci = dev.compIndex;
            const int o = dev.portBase;
            double derivative = 0.0;
            OpAmpEngineLaw::sourceVoltage(opAmps_[ci], vv_[o + 1] - vv_[o + 2], previous[ci],
                opAmpAlpha_[ci], opAmpMaxDv_[ci], false, &derivative);
            if (derivative != 0.0)
                mask |= 1u << d;
        }
        ++counts[mask];
    }
    return {counts.begin(), counts.end()};
}

}  // namespace wdf
