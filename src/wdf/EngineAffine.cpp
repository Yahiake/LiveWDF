#include "wdf/Engine.h"

#include <array>
#include <map>
#include <numbers>

namespace wdf {

namespace {
constexpr size_t planLimit = 5;
}

bool Engine::canPrepareMixedAffineReduction(size_t inputSource) const noexcept {
    if (blockCount_ != 1)
        return false;
    const auto& block = newtonBlocks_.front();
    // Reconstructing the original voltages costs more than removing a
    // single row in a large coupled block. Skip small reductions.
    if (!explicitRootsAllowed_ || inputSource >= circuit_.src.size() ||
        !block.linearReduction || block.linearDevices.empty() || block.reducedPorts.size() < 3 ||
        block.linearDevices.size() * 8 < block.ports.size())
        return false;
    for (int index : block.linearDevices) {
        const auto& dev = rootDevices_[static_cast<size_t>(index)];
        if (dev.kind != DeviceKind::OpAmp || opAmps_[dev.compIndex].hasClassABOutput() ||
            opAmps_[dev.compIndex].softRails)
            return false;
    }
    for (int index : block.reducedDevices) {
        const auto kind = rootDevices_[static_cast<size_t>(index)].kind;
        if (kind != DeviceKind::Diode && kind != DeviceKind::Bjt &&
            kind != DeviceKind::Jfet && kind != DeviceKind::Mosfet)
            return false;
    }

    return true;
}

std::optional<Engine::MixedAffineSetup> Engine::captureMixedAffineSetup(size_t inputSource) const {
    if (!canPrepareMixedAffineReduction(inputSource))
        return std::nullopt;
    MixedAffineSetup setup;
    setup.owner_ = this;
    setup.circuit_ = circuit_;
    setup.rate_ = fs_;
    setup.input_ = inputSource;
    setup.gridCurrent_ = gridCurrent_;
    setup.revision_ = linearRebuilds();
    setup.transfer_ = Z_;
    return setup;
}

void Engine::MixedAffineSetup::build(std::optional<std::span<const unsigned>> cachedMasks) {
    plans_.clear();
    Engine engine(circuit_, rate_, gridCurrent_);
    if (engine.Z_ != transfer_ || !engine.canPrepareMixedAffineReduction(input_))
        return;
    if (cachedMasks)
        engine.compileMixedAffinePlans(*cachedMasks);
    else
        engine.prepareMixedAffineReduction(input_);
    plans_ = std::move(engine.newtonBlocks_.front().alternateLinearSlopes);
}

std::vector<unsigned> Engine::MixedAffineSetup::slopeMasks() const {
    std::vector<unsigned> masks;
    masks.reserve(plans_.size());
    for (const auto& plan : plans_)
        masks.push_back(plan.linearSlopeMask);
    return masks;
}

bool Engine::installMixedAffineSetup(MixedAffineSetup& setup) {
    if (setup.owner_ != this || setup.revision_ != linearRebuilds() || setup.rate_ != fs_ ||
        !canPrepareMixedAffineReduction(setup.input_) || setup.transfer_ != Z_)
        return false;
    newtonBlocks_.front().alternateLinearSlopes.swap(setup.plans_);
    return true;
}

size_t Engine::prepareMixedAffineReduction(size_t inputSource, double peakVolts) {
    if (blockCount_ != 1)
        return 0;
    auto& block = newtonBlocks_.front();
    block.alternateLinearSlopes.clear();
    if (!canPrepareMixedAffineReduction(inputSource) ||
        !(peakVolts > 0.0) || !std::isfinite(peakVolts * 8.0))
        return 0;

    Engine calibration(circuit_, fs_, gridCurrent_);
    if (calibration.blockCount_ != 1 ||
        calibration.newtonBlocks_.front().linearDevices != block.linearDevices)
        return 0;
    auto patterns = calibration.measureAffineSlopes(inputSource, peakVolts);
    std::stable_sort(patterns.begin(), patterns.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<unsigned> masks;
    for (const auto& [mask, count] : patterns) {
        if (count < 16)
            break;
        masks.push_back(mask);
    }
    compileMixedAffinePlans(masks);
    return block.alternateLinearSlopes.size();
}

void Engine::compileMixedAffinePlans(std::span<const unsigned> masks) {
    auto& block = newtonBlocks_.front();
    const size_t width = block.linearDevices.size();
    const unsigned validBits = width == 32 ? ~0u : (1u << width) - 1u;
    for (const unsigned mask : masks) {
        if (block.alternateLinearSlopes.size() == planLimit)
            break;
        if ((mask & ~validBits) != 0)
            continue;
        NewtonBlock plan;
        plan.devices = block.devices;
        if (solver::RootLinearPlan::build(plan, rootDevices_.data(), Z_.data(), K_,
                                         comp_, opAmps_, ctrlResValues_, fs_, mask))
            block.alternateLinearSlopes.emplace_back(std::move(plan));
    }
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
