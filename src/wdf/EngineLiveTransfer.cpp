#include "wdf/Engine.h"
#include "wdf/DenseSolve.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <tuple>

namespace wdf {

bool Engine::captureLiveTransferSetup(LiveTransferSetup& setup, bool planOnly) const noexcept {
    if (setup.prepared_ || hasAdjustedLiveTransfer() == planOnly || !explicitRootsAllowed_ || runtimeBlockEnabled_ ||
        fineBlockSolver_ || powerSagEnabled_ ||
        setup.circuit_.res.size() != circuit_.res.size() ||
        setup.circuit_.pots.size() != circuit_.pots.size() ||
        setup.circuit_.varRes.size() != circuit_.varRes.size())
        return false;

    for (size_t i = 0; i < circuit_.res.size(); ++i)
        setup.circuit_.res[i].value = circuit_.res[i].value;
    for (size_t i = 0; i < circuit_.pots.size(); ++i)
        setup.circuit_.pots[i].position = circuit_.pots[i].position;
    for (size_t i = 0; i < circuit_.varRes.size(); ++i)
        setup.circuit_.varRes[i].position = circuit_.varRes[i].position;
    setup.owner_ = this;
    setup.planOnly_ = planOnly;
    setup.maskCount_ = 0;
    setup.affineRevision_ = mixedAffineRevision();
    if (const int index = mixedAffineBlock(); planOnly && index >= 0) {
        for (const auto& plan : newtonBlocks_[static_cast<size_t>(index)].alternateLinearSlopes)
            setup.affineMasks_[setup.maskCount_++] = plan.linearSlopeMask;
    }
    setup.rate_ = fs_;
    setup.gridCurrent_ = gridCurrent_;
    setup.linearRevision_ = linearRebuilds();
    setup.controlRevision_ = liveControlRevision();
    return true;
}

Engine::LiveTransferSetup Engine::LiveTransferSetup::makePeer() {
    if (!baseCircuit_)
        baseCircuit_ = std::make_shared<BaseCircuitIdentity>();
    LiveTransferSetup peer(circuit_);
    peer.baseCircuit_ = baseCircuit_;
    return peer;
}

void Engine::LiveTransferSetup::build(size_t inputSource, bool prepareAffine) {
    prepared_ = std::make_unique<Engine>(circuit_, rate_, gridCurrent_);
    if (!planOnly_)
        prepared_->prepareLiveControls();
    if (planOnly_ || prepareAffine)
        prepared_->prepareCircuitPlan();
    if (prepareAffine)
        prepared_->prepareMixedAffineReduction(inputSource);
    else if (maskCount_ && prepared_->canPrepareMixedAffineReduction(inputSource))
        prepared_->compileMixedAffinePlans(std::span(affineMasks_).first(maskCount_));
}

void Engine::LiveTransferSetup::prepareCompiledPreview(size_t inputSource, int outputNode, bool allowOutputOnly,
                                                       double requestedPeak, bool allowPad) {
    if (!prepared_)
        return;
    compiledPreview_ = true;
    // Performance screening may leave the worker at the end of its test signal.
    // Compile each channel at its physical DC point. Live histories stay on the recipient.
    prepared_->reset();
    prepared_->prepareCompiledPreview(inputSource, outputNode, allowOutputOnly, requestedPeak, allowPad);
}

bool Engine::LiveTransferSetup::prepareCompiledPreviewPair(LiveTransferSetup& peer, size_t inputSource,
                                                           int outputNode, bool allowOutputOnly,
                                                           double requestedPeak, bool allowPad) {
    const bool fresh = !compiledPreview_ && !peer.compiledPreview_;
    prepareCompiledPreview(inputSource, outputNode, allowOutputOnly, requestedPeak, allowPad);
    const auto sameValues = [](const auto& a, const auto& b, auto field) {
        return std::ranges::equal(a, b, {}, field, field);
    };
    // Only these three sets of values can differ between snapshots of a shared
    // base circuit. Model cards, wiring and the other components came from the same copy.
    if (fresh && prepared_ && prepared_->bipolarPreview_ && peer.prepared_ &&
        baseCircuit_ && baseCircuit_ == peer.baseCircuit_ &&
        rate_ == peer.rate_ && gridCurrent_ == peer.gridCurrent_ && planOnly_ == peer.planOnly_ &&
        maskCount_ == peer.maskCount_ && affineMasks_ == peer.affineMasks_ &&
        sameValues(circuit_.res, peer.circuit_.res, &Res::value) &&
        sameValues(circuit_.pots, peer.circuit_.pots, &Pot::position) &&
        sameValues(circuit_.varRes, peer.circuit_.varRes, &VarRes::position)) {
        peer.prepared_->reset();
        peer.prepared_->copyBipolarPreviewFrom(*prepared_);
        peer.prepared_->preparePackedDrive();
        peer.compiledPreview_ = true;
        return true;
    }
    peer.prepareCompiledPreview(inputSource, outputNode, allowOutputOnly, requestedPeak, allowPad);
    return false;
}

void Engine::prepareCompiledPreview(size_t inputSource, int outputNode, bool allowOutputOnly,
                                    double requestedPeak, bool allowPad) {
    preparePackedDrive();
    if (prepareAffineNodePreview(inputSource, outputNode))
        return;
    if (allowOutputOnly && prepareBipolarPreview(inputSource, outputNode, requestedPeak, allowPad))
        return;
    prepareLoadedJunctionPreview(inputSource);
    sharedNewtonColumns_.prepare(Zred_, K_, nRed_, rootDevices_);
    triodeDomainTransfer_.prepare(Z_, K_, rootDevices_);
    prepareSlowLag();
    prepareLampReplay();
    if (lamp_.held > 0.0) {
        sharedNewtonColumns_.prepare(lamp_.Zred, K_, lamp_.nRed, lamp_.devices);
        triodeDomainTransfer_.prepare(lamp_.Z, K_, rootDevices_);
    }
}

void EngineStep::prepareLampReplay() {
    lamp_ = {};
    std::vector<int> side(static_cast<size_t>(K_));
    std::iota(side.begin(), side.end(), 0);
    const auto find = [&](int p) { while (side[p] != p) p = side[p] = side[side[p]]; return p; };
    for (int r = 0; r < K_; ++r)
        for (int c = 0; c < K_; ++c)
            if (Z_[size_t(r) * K_ + c] != 0.0) side[find(r)] = find(c);
    // A photoresistor whose light comes only from ports with no coupling to its own side watches a lamp.
    for (const auto& photo : photoresistors_) {
        const auto own = std::ranges::find_if(rootDevices_, [&](const RootDevice& d) {
            return d.kind == DeviceKind::ControlledRes && d.compIndex == photo.index; });
        const auto& light = compiledOptical_ ? photoresistors_[photo.lamp] : photo;
        int lamp = -1;
        bool alone = own != rootDevices_.end();
        for (const int node : {light.sensePos, light.senseNeg})
            for (int p = 0; node >= 0 && p < K_ && alone; ++p)
                if (Xr_[size_t(node) * K_ + p] != 0.0) {
                    alone = find(p) != find(own->portBase) && (lamp < 0 || find(p) == lamp);
                    lamp = find(p);
                }
        if (!alone || lamp < 0)
            continue;
        for (int p = 0; p < K_; ++p)
            if (find(p) == lamp) lamp_.ports.push_back(p);
        lamp_.frozen.assign(size_t(K_), 0);
        for (const int p : lamp_.ports) lamp_.frozen[size_t(p)] = 1;
        lamp_.watch = int(photo.index);
        lamp_.port = own->portBase;
        lamp_.length = int(fs_);
        lamp_.ring.assign(size_t(lamp_.length) * (lamp_.ports.size() + 1), 0.0);
        lamp_.zLamp.assign(lamp_.ports.size() * lamp_.ports.size(), 0.0);
        lamp_.order.reserve(rootDevices_.size());

        // Record one period with the lamp side solved alone from the DC point. Preview so loses the few
        // percent the rate drifts while playing, which the supply carries back to the oscillator.
        lamp_.alone.assign(size_t(K_), 1);
        for (const int p : lamp_.ports) lamp_.alone[size_t(p)] = 0;
        std::vector<double> sources;
        for (const auto& s : circuit_.src) sources.push_back(s.dc);
        for (const auto& s : circuit_.isrc) sources.push_back(s.dc);
        for (int n = 0; n < 30 * lamp_.length && !lamp_.period; ++n)
            step(sources);
        lamp_.alone.clear();

        // Fold only a cell from a node to ground whose other neighbours are tube ports.
        const int r = lamp_.port;
        bool fits = lamp_.period && (rootPortInfo_[size_t(r)].a == N_ || rootPortInfo_[size_t(r)].b == N_);
        for (const auto& d : rootDevices_)
            for (int p = d.portBase; p < d.portBase + d.numPorts && fits; ++p)
                fits = p == r || lamp_.frozen[size_t(p)] || Z_[size_t(r) * K_ + p] == 0.0 ||
                       d.kind == DeviceKind::Triode || d.kind == DeviceKind::Pentode;
        if (!fits) {
            lamp_.held = 0.0;
            return;
        }
        lamp_.frozen[size_t(r)] = 1;
        for (auto d : rootDevices_) {
            if (lamp_.frozen[size_t(d.portBase)])
                continue;
            for (int q = 0; q < d.numActiveRows; ++q)
                lamp_.redCol.push_back(redCol_[size_t(d.activeRowBase + q)]);
            d.activeRowBase = lamp_.nRed;
            lamp_.nRed += d.numActiveRows;
            lamp_.devices.push_back(d);
        }
        lamp_.Z.resize(Z_.size());
        lamp_.Zred.resize(size_t(K_) * size_t(lamp_.nRed));
        foldLamp();
        return;
    }
}

void EngineStep::foldLamp() {
    auto& L = lamp_;
    const size_t r = size_t(L.port), K = size_t(K_);
    const double k = 1.0 / (Z_[r * K + r] + L.held);
    for (size_t p = 0; p < K; ++p)
        for (size_t q = 0; q < K; ++q)
            L.Z[p * K + q] = p == r || q == r ? 0.0 : Z_[p * K + q] - Z_[p * K + r] * Z_[r * K + q] * k;
    for (size_t p = 0; p < K; ++p)
        for (int j = 0; j < L.nRed; ++j)
            L.Zred[p * size_t(L.nRed) + j] = L.Z[p * K + size_t(L.redCol[size_t(j)])];
    L.foldedControls = liveControlRevision();
}

void EngineStep::stepLampReplay() {
    auto& L = lamp_;
    if (!loadedJunctionEnabled_ && L.alone.empty())
        return;
    const size_t nS = L.ports.size(), w = nS + 1;
    bool moved = L.rebuilds != linearRebuilds();
    for (size_t i = 0; i < nS * nS && !moved && L.controls != liveControlRevision(); ++i)
        moved = L.zLamp[i] != Z_[size_t(L.ports[i / nS]) * K_ + L.ports[i % nS]];
    L.controls = liveControlRevision();
    // A recorded curve keeps playing after a control move until the curve recorded for the new setting is installed.
    if (moved && L.period) {
        L.rebuilds = linearRebuilds();
    } else if (moved) {
        L.n = L.period = L.previous = 0;
        L.crossing = -1;
        L.mid = NAN;
        L.span = 0.0;
        L.lengths.clear();
        L.spans.clear();
        L.lo = HUGE_VAL;
        L.hi = -HUGE_VAL;
        L.rebuilds = linearRebuilds();
        for (size_t i = 0; i < nS * nS; ++i) L.zLamp[i] = Z_[size_t(L.ports[i / nS]) * K_ + L.ports[i % nS]];
    }
    if (L.period) {
        const double* rec = &L.ring[size_t((L.base + L.phase) % L.length) * w];
        for (size_t i = 0; i < nS; ++i) ii_[size_t(L.ports[i])] = rec[i];
        for (const int r : L.ports) {
            double v = vOc_[size_t(r)];
            for (const int c : L.ports) v -= Z_[size_t(r) * K_ + c] * ii_[size_t(c)];
            vv_[size_t(r)] = v;
        }
        L.phase = (L.phase + 1) % L.period;
        return;
    }
    // Record until two successive periods of the watched resistance agree, then loop the last one. Alone the neon
    // relaxation never repeats exactly, so after settling it loops the first cycle of median length and swing.
    double* rec = &L.ring[size_t(L.n % L.length) * w];
    for (size_t i = 0; i < nS; ++i) rec[i] = ii_[size_t(L.ports[i])];
    const double x = rec[nS] = std::log(ctrlResValues_[size_t(L.watch)]);
    if (L.previous && L.n >= L.previous)
        L.deviation = std::max(L.deviation, std::abs(x - L.ring[size_t((L.n - L.previous) % L.length) * w + nS]));
    const double mid = std::isnan(L.mid) ? 0.5 * (L.lo + L.hi) : L.mid;
    if (L.n > 0 && L.last < mid && x >= mid && L.lo < mid - std::max(0.05, 0.25 * L.span)) {
        const long long p = L.n - L.crossing;
        const auto median = [](std::vector<double> v) {
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            return v[v.size() / 2];
        };
        bool clean = L.alone.empty() && L.previous && std::abs(p - L.previous) <= 1 && L.deviation <= 0.02;
        if (!L.alone.empty() && L.crossing >= 2 * L.length) {
            clean = L.lengths.size() >= 16 && std::abs(p - median(L.lengths)) <= 0.01 * median(L.lengths) &&
                    std::abs(L.hi - L.lo - median(L.spans)) <= 0.05 * median(L.spans);
            L.lengths.push_back(double(p));
            L.spans.push_back(L.hi - L.lo);
        }
        if (L.crossing >= 0 && clean && p < L.length && L.hi - L.lo > 0.1) {
            L.period = int(p);
            L.base = int((L.n + 1 - p) % L.length);
            L.phase = 0;
            // The held value keeps the mean current the cell draws from its own open-circuit voltage.
            const double z = Z_[size_t(L.port) * K_ + L.port];
            double k = 0.0;
            for (int s = 0; s < L.period; ++s)
                k += 1.0 / (z + std::exp(L.ring[size_t((L.base + s) % L.length) * w + nS]));
            L.held = L.period / k - z;
            L.foldedControls = -1;
        }
        L.previous = L.crossing >= 0 && p < L.length ? int(p) : 0;
        L.mid = 0.5 * (L.lo + L.hi);
        L.span = L.hi - L.lo;
        L.lo = HUGE_VAL;
        L.hi = -HUGE_VAL;
        L.deviation = 0.0;
        L.crossing = L.n;
    }
    L.lo = std::min(L.lo, x);
    L.hi = std::max(L.hi, x);
    L.last = x;
    ++L.n;
}

void EngineStep::prepareSlowLag() {
    // Steepest loaded slope |(I + J Zdd)^-1 J| of each device over a sweep of its port voltages.
    std::vector<double> slope(size_t(K_) * K_, 0.0), v, i(K_), a, x(K_);
    std::vector<int> pivots(K_);
    auto newton = makeNewtonContext();
    constexpr double sweep[] = {-50, -20, -5, -2, -1, -0.5, 0, 0.5, 1, 2, 5, 10, 20, 50, 200, 400};
    const auto at = [&](int point) {
        v = vv_;
        for (const auto& d : rootDevices_)
            for (int q = 0; q < std::min(d.numPorts, 3); ++q)
                v[d.portBase + q] += sweep[(point >> (4 * q)) & 15];
        solver::RootNewton::currentsInto(newton, v, i, J_);
    };
    for (int point = 0; point < 4096; ++point) {
        at(point);
        for (const auto& d : rootDevices_) {
            const int m = d.numPorts, b = d.portBase;
            a.assign(size_t(m) * m, 0.0);
            for (int r = 0; r < m; ++r)
                for (int c = 0; c < m; ++c) {
                    a[r * m + c] = r == c;
                    for (int k = 0; k < m; ++k)
                        a[r * m + c] += J_[size_t(b + r) * K_ + b + k] * Z_[size_t(b + k) * K_ + b + c];
                }
            const bool factored = detail::factorLUData(a.data(), pivots.data(), m);
            for (int c = 0; c < m; ++c) {
                for (int r = 0; r < m; ++r)
                    x[r] = J_[size_t(b + r) * K_ + b + c];
                if (factored)
                    detail::solveLUData(a.data(), pivots.data(), x.data(), m);
                for (int r = 0; r < m; ++r) {
                    double& s = slope[size_t(b + r) * K_ + b + c];
                    s = factored && std::isfinite(x[r]) ? std::max(s, std::abs(x[r])) : HUGE_VAL;
                }
            }
        }
    }
    std::vector<uint8_t> lag;
    std::vector<int> owner;
    bool closed = false, antiParallel = false;
    int scalars = 0;
    std::vector<double> late(size_t(K_) * K_), m(size_t(K_) * K_), t(K_);
    for (bool retry = true; retry;) {
        retry = false;
        lagOrder_.resize(rootDevices_.size());
        auto ctx = makeRootGroupsContext(lagBlocks_, lagOrder_, owner, closed, antiParallel, lagCount_, scalars);
        if (!solver::RootGroups::buildRootOrder(ctx, true, &slope, &lag) ||
            (lagCount_ == blockCount_ && std::all_of(lagBlocks_.begin(), lagBlocks_.begin() + lagCount_, [](const auto& b) { return b.lagged.empty(); })))
            lagCount_ = 0;
        for (int b = 0; b < lagCount_; ++b)
            solver::RootLinearPlan::build(lagBlocks_[size_t(b)], rootDevices_.data(), Z_.data(), K_, comp_, opAmps_,
                                          ctrlResValues_, fs_);
        // The pairwise rule misses a long loop: a late column closing around the solved
        // forward path. Refuse unless |J (I + Zs J)^-1 Zl| stays below 1/4 over the sweep.
        std::fill(late.begin(), late.end(), 0.0);
        for (int b = 0; b < lagCount_; ++b)
            for (const auto& [r, d] : lagBlocks_[size_t(b)].lagged)
                late[size_t(r) * K_ + d.col] = d.z;
        for (int point = 0; point < 4096 && lagCount_ > 1; point += 29) {
            at(point);
            std::fill(m.begin(), m.end(), 0.0);
            for (int r = 0; r < K_; ++r)
                m[size_t(r) * K_ + r] = 1.0;
            for (const auto& d : rootDevices_)
                for (int r = 0; r < K_; ++r)
                    for (int c = d.portBase; c < d.portBase + d.numPorts; ++c)
                        for (int k = d.portBase; k < d.portBase + d.numPorts; ++k)
                            m[size_t(r) * K_ + c] += (Z_[size_t(r) * K_ + k] - late[size_t(r) * K_ + k]) * J_[size_t(k) * K_ + c];
            bool ok = detail::factorLUData(m.data(), pivots.data(), K_);
            std::fill(t.begin(), t.end(), 0.0);
            int worst = -1;
            double worstGain = 0.0;
            for (int c = 0; c < K_ && ok; ++c) {
                for (int r = 0; r < K_; ++r)
                    x[r] = late[size_t(r) * K_ + c];
                if (std::none_of(x.begin(), x.end(), [](double z) { return z != 0.0; }))
                    continue;
                detail::solveLUData(m.data(), pivots.data(), x.data(), K_);
                for (const auto& d : rootDevices_)
                    for (int r = d.portBase; r < d.portBase + d.numPorts; ++r) {
                        double di = 0.0;
                        for (int k = d.portBase; k < d.portBase + d.numPorts; ++k)
                            di += J_[size_t(r) * K_ + k] * x[k];
                        t[r] += std::abs(di);
                        if (std::abs(di) > worstGain) {
                            worstGain = std::abs(di);
                            worst = c;
                        }
                    }
            }
            if (ok && *std::max_element(t.begin(), t.end()) < 0.25)
                continue;
            lagCount_ = 0;
            // Read the worst late device on time and keep the other cuts: a finite
            // maximum slope keeps it out of every lag, where infinity times a zero Z is NaN.
            for (const auto& d : rootDevices_)
                if (worst >= d.portBase && worst < d.portBase + d.numPorts)
                    for (int r = 0; r < d.numPorts; ++r)
                        for (int c = 0; c < d.numPorts; ++c)
                            slope[size_t(d.portBase + r) * K_ + d.portBase + c] = std::numeric_limits<double>::max();
            retry = worst >= 0;
        }
    }
    lagHeld_.assign(K_, 0.0);
    lagRevision_ = linearRebuilds();
}

bool Engine::canInstallLiveTransferSetup(const LiveTransferSetup& setup) const noexcept {
    const auto& prepared = setup.prepared_;
    return setup.owner_ == this && prepared && (hasAdjustedLiveTransfer() != setup.planOnly_) &&
           (!setup.planOnly_ || setup.affineRevision_ == mixedAffineRevision()) &&
           setup.linearRevision_ == linearRebuilds() &&
           setup.controlRevision_ == liveControlRevision() &&
           explicitRootsAllowed_ && !runtimeBlockEnabled_ && !fineBlockSolver_ &&
           !powerSagEnabled_ &&
           std::equal(opAmps_.begin(), opAmps_.end(), prepared->opAmps_.begin(),
                      prepared->opAmps_.end(), [](const auto& a, const auto& b) {
                          return a.softRails == b.softRails &&
                                 a.outputResistanceEmbedded == b.outputResistanceEmbedded;
                      }) &&
           prepared->net_.epA == net_.epA &&
           prepared->net_.epB == net_.epB && prepared->net_.rp == net_.rp &&
           prepared->net_.eG == net_.eG && prepared->net_.liveCols == net_.liveCols;
}

bool Engine::installLiveTransferSetup(LiveTransferSetup& setup) noexcept {
    if (!canInstallLiveTransferSetup(setup))
        return false;

    releaseBipolarPreview();
    setup.owner_ = nullptr;
    auto& preparedEngine = *setup.prepared_;
    if (preparedEngine.hasRecoveryState())
        std::swap(recovery_, preparedEngine.recovery_);
    driveCoefficients_.swap(preparedEngine.driveCoefficients_);
    driveInputs_.swap(preparedEngine.driveInputs_);
    std::swap(affineRoot_, preparedEngine.affineRoot_);
    certifiedResistance_.swap(preparedEngine.certifiedResistance_);
    affineRootCooldown_ = 0;
    affineRootRetry_ = 8;
    rootFactors_.swap(preparedEngine.rootFactors_);
    circuitResidualColumns_.swap(preparedEngine.circuitResidualColumns_);
    rootPivots_.swap(preparedEngine.rootPivots_);
    circuitPlanCooldown_ = 0;
    circuitPlanRetry_ = preparedEngine.circuitPlanRetry_;
    const auto installPreview = [&] {
        if (setup.compiledPreview_) {
            std::swap(sharedNewtonColumns_, preparedEngine.sharedNewtonColumns_);
            std::swap(triodeDomainTransfer_, preparedEngine.triodeDomainTransfer_);
            std::swap(packedDrive_, preparedEngine.packedDrive_);
            std::swap(lagBlocks_, preparedEngine.lagBlocks_);
            std::swap(lagOrder_, preparedEngine.lagOrder_);
            std::swap(lagHeld_, preparedEngine.lagHeld_);
            std::swap(lamp_, preparedEngine.lamp_);
            lagCount_ = preparedEngine.lagCount_;
            lagRevision_ = preparedEngine.lagRevision_ == preparedEngine.linearRebuilds() ? linearRebuilds() : -1;
            if (!packedDrive_.panels.empty())
                packedDrive_.revision = linearRebuilds();
            swapAffineNodePreview(preparedEngine);
            adoptBipolarPreview(preparedEngine);
            swapLoadedJunctionPreview(preparedEngine);
        }
    };
    if (setup.planOnly_) {
        circuitPlanRevision_ = linearRebuilds();
        installPreview();
        return true;
    }

    // The old transfer goes back to the control side through setup. Live
    // capacitor/inductor waves, device histories and sample scratch stay put.
    const auto transfer = [](Engine& e) {
        return std::tie(
            e.net_.elems, e.Xe_, e.Xr_, e.Z_, e.Xis_,
            e.linY_, e.linRhs_, e.linXe_, e.linXr_, e.networkFactors_,
            e.xeRowOffsets_, e.xeCols_, e.xeVals_, e.xeRows1_, e.xeRows2_, e.xeRowsN_,
            e.xrRowOffsets_, e.xrCols_, e.xrVals_, e.xrRows1_, e.xrRows2_, e.xrRowsN_,
            e.isrcRows_, e.rootPanels_, e.rootPanelValues_, e.adjustableTransfer_,
            e.Zred_, e.newtonBlocks_, e.groupOrder_, e.grpDiode_, e.blockOf_,
            e.blockCount_, e.groupCount_, e.explicitRootsActive_,
            e.allClosedFormBlocks_, e.scalarBlockCount_, e.hasAntiParallelBlock_,
            e.hasLinearReduction_);
    };
    auto live = transfer(*this);
    auto prepared = transfer(*setup.prepared_);
    live.swap(prepared);
    if (!setup.compiledPreview_) {
        sharedNewtonColumns_.refresh(Zred_, K_, nRed_);
        triodeDomainTransfer_.refresh(Z_, K_);
    }
    ++linearRebuilds_;
    circuitPlanRevision_ = preparedEngine.circuitPlanPrepared() ? linearRebuilds() : -1;
    installPreview();
    return true;
}

}  // namespace wdf
