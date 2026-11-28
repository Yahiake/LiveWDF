#include "wdf/EngineStep.h"

#include <cstdio>

namespace wdf {

bool EngineStep::tryTriodePreview() noexcept {
    // Fold the other stages into each triode's load line, using the current
    // transfer matrix even while a control is moving. Reuse the exact local
    // plate and grid laws and keep the trial separate from the accepted state.
    constexpr int sweeps = 8;
    constexpr double maxCorrectionVolts = 0.3;
    std::copy(ii_.begin(), ii_.end(), iv_.begin());
    const auto visit = [&](const RootDevice& device, double& plate, double& grid) {
        const int port = device.portBase;
        const double* zp = Z_.data() + static_cast<size_t>(port) * K_;
        const double* zg = zp + 2 * K_;
        double openPlate = vOc_[port], openGrid = vOc_[port + 2];
        for (int other = 0; other < K_; other += 3) {
            if (other == port) continue;
            openPlate -= zp[other] * iv_[other] + zp[other + 2] * iv_[other + 2];
            openGrid -= zg[other] * iv_[other] + zg[other + 2] * iv_[other + 2];
        }
        return solver::RootClosedForms::solveTriode(comp_.tri[device.compIndex].model,
            openPlate, openGrid, Z_.data(), port, K_, plate, grid);
    };
    for (int sweep = 0; sweep < sweeps; ++sweep)
        for (const auto& device : rootDevices_)
            visit(device, iv_[device.portBase], iv_[device.portBase + 2]);

    double maxCorrection = 0.;
    for (const auto& device : rootDevices_) {
        const int port = device.portBase;
        double plate = 0., grid = 0.;
        const auto domain = visit(device, plate, grid);
        const double dp = plate - iv_[port], dg = grid - iv_[port + 2];
        for (const int row : {port, port + 2}) {
            const double* z = Z_.data() + static_cast<size_t>(row) * K_;
            const double correction = std::abs(z[port] * dp + z[port + 2] * dg);
            if (!(correction <= maxCorrectionVolts)) return false;
            maxCorrection = std::max(maxCorrection, correction);
        }
        vt_[port] = static_cast<double>(domain);
    }

    for (int row = 0; row < K_; ++row) {
        double voltage = vOc_[row];
        const double* z = Z_.data() + static_cast<size_t>(row) * K_;
        for (int col = 0; col < K_; col += 3)
            voltage -= z[col] * iv_[col] + z[col + 2] * iv_[col + 2];
        vv_[row] = voltage;
    }
    for (const auto& device : rootDevices_)
        triodeDomainPin_[device.portBase] = static_cast<unsigned char>(vt_[device.portBase]);
    ii_.swap(iv_);
    vvPrev2_ = vvPrev_;
    vvPrev_ = vv_;
    lastConverged_ = true;
    lastResidual_ = maxCorrection;
    ++triodePreviewHits_;
    return true;
}

const std::vector<double>& EngineStep::step(const double* sourceValues, std::size_t sourceCount,
                                            int maxIter, double tol) {
    modeTol_ = tol;
    if (!heldRails_.empty()) {
        heldSources_.assign(sourceValues, sourceValues + sourceCount);
        for (const auto& h : heldRails_)
            if (h.source < sourceCount) heldSources_[h.source] = h.volts;
        sourceValues = heldSources_.data();
    }
    // Limit the error in the port voltages while evaluating all tube currents.
    if (triodePreviewEnabled_ && nPent_ > 0)
        tol = std::max(tol, 3e-3);

    // The runtime pass pays a small fixed cost. On cheap low-frequency
    // samples the joint Newton is already one or two iterations, so use
    // the previous sample's work as a cheap gate: only take the block path
    // when the reference solve was actually expensive. A short cooldown
    // keeps the path from oscillating on alternating samples.
    const bool previousCostly =
        jointLastIters_ + jointLastTrials_ >= 8;
    newtonIters_ = 0;
    deviceEvals_ = 0;
    lineTrials_ = 0;
    limitHits_ = 0;
    gridLimitHits_ = 0;
    keptBest_ = false;
    runtimeBlockUsed_ = false;
    lastBlockFallback_ = false;
    lastStateRejected_ = false;
    lastMaxNodeIndex_ = -1;
    lastMaxNodeVolts_ = 0.0;
    lastRejectNodeIndex_ = -1;
    lastRejectNodeVolts_ = 0.0;

    // If the same state keeps failing, restoring it has become the wrong
    // repair: every sample repeats the same doomed warm-start search. Re-
    // anchor to the DC operating point after a sustained stall and solve
    // the current sample from there. This recovery runs only after
    // consecutive rejected samples, not on normal switch changes.
    if (consecutiveStateRejects_ >= 8) {
        consecutiveStateRejects_ = 0;
        recoverOperatingPoint();
        newtonIters_ = 0;
        deviceEvals_ = 0;
        lineTrials_ = 0;
        limitHits_ = 0;
        gridLimitHits_ = 0;
        runtimeBlockUsed_ = false;
        lastStateRejected_ = false;
        lastMaxNodeIndex_ = -1;
        lastMaxNodeVolts_ = 0.0;
        lastRejectNodeIndex_ = -1;
        lastRejectNodeVolts_ = 0.0;
    }

    // Remember the last accepted state. A switch or relay is a step change
    // in the linear network; Newton can return a far, non-physical iterate
    // on that sample. The solver reports the miss through `lastConverged_`,
    // but the state was committed anyway, so one bad sample poisoned every
    // later one (the output climbed, the CPU stayed at max iterations).
    // These snapshots make rejection a restore, not a re-solve.
    if (K_ > 0) {
        std::memcpy(vvSafe_.data(), vv_.data(), static_cast<size_t>(K_) * sizeof(double));
        std::memcpy(iiSafe_.data(), ii_.data(), static_cast<size_t>(K_) * sizeof(double));
    }
    std::memcpy(nodeSafe_.data(), node_.data(), static_cast<size_t>(N_) * sizeof(double));

    using clk = std::chrono::steady_clock;
    clk::time_point tPhase{};
    if (profileOn_) {
        tPhase = clk::now();
        ++prof_.samples;
    }
    const auto tock = [&](double& acc) {
        if (profileOn_) {
            const auto now = clk::now();
            acc += std::chrono::duration<double>(now - tPhase).count();
            tPhase = now;
        }
    };

    // Only a port a source reaches has an offset that moves. The rest keep
    // what `allocateBuffers` seeded (eoff/roff = their constant part),
    // which is the value these loops used to rewrite on every sample.
    const double* LIVEWDF_RESTRICT svData = sourceValues;
    const size_t svSize = sourceCount;
    double* LIVEWDF_RESTRICT eoffData = eoff_.data();
    for (const auto& u : coupledOffsets_) {
        double s = u.c0;
        if (u.nCoef == 1) {
            if (static_cast<size_t>(u.idx0) < svSize)
                s += u.c1 * svData[u.idx0];
        } else if (u.nCoef == 2) {
            if (static_cast<size_t>(u.idx0) < svSize)
                s += u.c1 * svData[u.idx0];
            if (static_cast<size_t>(u.idx1) < svSize)
                s += u.c2 * svData[u.idx1];
        } else if (u.nCoef > 2) {
            s = net_.eoffAff[static_cast<size_t>(u.targetIdx)].eval(svData, svSize);
        }
        eoffData[u.targetIdx] = s;
    }

    double* LIVEWDF_RESTRICT roffData = roff_.data();
    for (const auto& u : coupledRoots_) {
        double s = u.c0;
        if (u.nCoef == 1) {
            if (static_cast<size_t>(u.idx0) < svSize)
                s += u.c1 * svData[u.idx0];
        } else if (u.nCoef == 2) {
            if (static_cast<size_t>(u.idx0) < svSize)
                s += u.c1 * svData[u.idx0];
            if (static_cast<size_t>(u.idx1) < svSize)
                s += u.c2 * svData[u.idx1];
        } else if (u.nCoef > 2) {
            s = net_.roffAff[static_cast<size_t>(u.targetIdx)].eval(svData, svSize);
        }
        roffData[u.targetIdx] = s;
    }

    if (nIsrc_ > 0) {
        for (int k = 0; k < nIsrc_; ++k) {
            const size_t slot = static_cast<size_t>(nsrc_ + k);
            isCur_[static_cast<size_t>(k)] =
                slot < svSize ? svData[slot]
                              : comp_.isrc[static_cast<size_t>(k)].second.dc;
        }
        for (const auto& bbd : bucketBrigades_)
            isCur_[bbd.outputCurrentIndex] = bbd.delay.outputCurrent();
        for (const auto& rms : rmsDetectors_)
            isCur_[rms.outputCurrentIndex] = rms.detector.outputCurrent();
    }

    if (loadedJunctionEnabled_ && affineNodes_.revision == linearRebuilds() &&
        tryAffineNodes(sourceValues, sourceCount, tol)) {
        tock(prof_.solve);
        return finishStep(sourceValues, sourceCount, tPhase);
    }
    const bool adjustedTransfer = hasAdjustedLiveTransfer();
    const bool compiled = circuitPlanEnabled_ && circuitPlanRevision_ == linearRebuilds() && !adjustedTransfer;
    double* LIVEWDF_RESTRICT uData = u_.data();
    if (compiled && !driveCoefficients_.empty() && applyCompiledDrive(sourceValues, sourceCount)) {
        tock(prof_.offsets);
    } else {
        // Direct assembly of live columns into w_, completely bypassing intermediate b_ vector.
        const double* LIVEWDF_RESTRICT capData = capState_.data();
        const double* LIVEWDF_RESTRICT indData = indState_.data();
        double* LIVEWDF_RESTRICT wData = w_.data();
        for (int t = 0; t < nLive_; ++t) {
            const auto& lc = liveColInfo_[static_cast<size_t>(t)];
            double b = 0.0;
            if (lc.capIdx >= 0)
                b = capData[lc.capIdx];
            else if (lc.indIdx >= 0)
                b = -indData[lc.indIdx];
            wData[t] = b - eoffData[lc.portIdx];
        }
        tock(prof_.offsets);

        // u = Xe * w via partitioned sparse rows. Degree 0 rows stay 0 from memset.
        // Degree 1 and 2 rows avoid inner-loop setup and branch checks.
        std::memset(uData, 0, (static_cast<size_t>(N_) + 1) * sizeof(double));
        if (networkFactors_.active) {
            networkFactors_.apply(wData, uData);
        } else {
            for (const auto& e : xeRows1_) {
                uData[e.node] = e.val * wData[e.col];
            }
            for (const auto& e : xeRows2_) {
                uData[e.node] = e.val0 * wData[e.col0] + e.val1 * wData[e.col1];
            }
            const bool packed = loadedJunctionEnabled_ && hasPackedDrivePlan();
            if (packed)
                detail::assignDrivePanels(packedDrive_.panels.data(), packedDrive_.panels.size(),
                    packedDrive_.nodes.data(), packedDrive_.values.data(), xeCols_.data(), wData, uData);
            const auto& remaining = packed ? packedDrive_.remaining : xeRowsN_;
            if (!remaining.empty()) {
                const int* LIVEWDF_RESTRICT xeCols = xeCols_.data();
                const double* LIVEWDF_RESTRICT xeVals = xeVals_.data();
                for (const auto& e : remaining) {
                    double s = 0.0;
                    for (int idx = e.start; idx < e.end; ++idx)
                        s += xeVals[idx] * wData[xeCols[idx]];
                    uData[e.node] = s;
                }
            }
        }
    }
    for (const auto& e : isrcRows_)
        uData[e.node] -= e.val * isCur_[static_cast<size_t>(e.src)];

    // Live knob changes correct the same base Xe transfer after evaluating it.
    if (adjustedTransfer)
        adjustableTransfer_.correctOpenCircuit(eoffData, uData);

    double* LIVEWDF_RESTRICT vOcData = vOc_.data();
    for (int r = 0; r < K_; ++r) {
        const auto& rp = rootPortInfo_[static_cast<size_t>(r)];
        vOcData[r] = (uData[rp.a] - uData[rp.b]) + roffData[r];
    }
    tock(prof_.network);
    const bool folded = lampFolded();
    if (folded) {
        auto& L = lamp_;
        if (L.foldedControls != liveControlRevision()) {
            foldLamp();
            sharedNewtonColumns_.refresh(L.Zred, K_, L.nRed);
            triodeDomainTransfer_.refresh(L.Z, K_);
        }
        // Held part solved with the circuit; the swing on top reads the other ports' currents one sample late.
        const size_t r = size_t(L.port);
        const double held = 1.0 / (Z_[r * K_ + r] + L.held);
        double open = vOcData[r];
        for (int c = 0; c < K_; ++c)
            open -= c == int(r) ? 0.0 : Z_[r * K_ + c] * ii_[size_t(c)];
        L.drive = held * vOcData[r] + (1.0 / (Z_[r * K_ + r] + ctrlResValues_[size_t(L.watch)]) - held) * open;
        for (int p = 0; p < K_; ++p)
            vOcData[p] -= p == int(r) ? 0.0 : Z_[size_t(p) * K_ + r] * L.drive;
    }

    const long long fallbacksBefore = blockFallbacks_;
    const bool pipeline = pipelineReady();
    const bool loadedRoot = !pipeline && !folded && loadedJunctionEnabled_ && hasLoadedJunctionPreview() && tryLoadedJunctionRoot();
    const bool movingAffine = !pipeline && !folded && !loadedRoot && adjustedTransfer && loadedJunctionEnabled_ && affineNodes_.motion &&
                              affineNodes_.revision == linearRebuilds() && tryAffineMotion(tol);
    const bool compiledRoot = movingAffine || (!pipeline && !folded && !loadedRoot && compiled && maxIter >= 4 &&
                              (!rootFactors_.empty() || !affineRoot_.rows.empty()) && explicitRootsAllowed_ &&
                              !explicitRootsActive_ && tryCompiledRoot(tol));
    bool triodePreview = false;
    if (!pipeline && !folded && !loadedRoot && !compiledRoot && triodePreviewEnabled_ && !explicitRootsActive_ &&
        nTri_ > 2 && static_cast<size_t>(nTri_) == rootDevices_.size()) {
        if (triodePreviewCooldown_ == 0) {
            triodePreview = tryTriodePreview();
            if (!triodePreview) triodePreviewCooldown_ = 16;
        } else {
            --triodePreviewCooldown_;
        }
    }
    if (K_ > 0 && !loadedRoot && !compiledRoot && !triodePreview) {
        // The runtime partition is an experiment on top of the reference
        // path. It is built only when neither of the existing exact paths
        // is driving the solve, and it never replaces the static partition
        // on circuits where that partition already pays.
        bool useRuntime = false;
        if (!adjustedTransfer && runtimeBlockEnabled_ && !explicitRootsActive_ && !blockRoots() &&
            (runtimeGateCooldown_ > 0 || previousCostly)) {
            selectRuntimeRootOrder();
            useRuntime = runtimeBlockRoots();
        }
        if (useRuntime) {
            // The block path's own iteration count is not comparable to a
            // joint Newton iteration. Require a fresh joint sample to
            // justify the next burst rather than letting a block solve
            // keep the gate open forever.
            jointLastIters_ = 0;
            jointLastTrials_ = 0;
            if (runtimeGateCooldown_ > 0)
                --runtimeGateCooldown_;
            else
                runtimeGateCooldown_ = 4;
        } else if (runtimeGateCooldown_ > 0) {
            --runtimeGateCooldown_;
        }

        if (folded)
            solveRootsNewton(maxIter, tol);
        else if (pipeline && blockFallbackCooldown_ == 0)
            solvePipelineStage(maxIter, tol, adjustedTransfer);
        else if (explicitRootsActive_ && !adjustedTransfer)
            solveRootGroups(maxIter, tol);
        else if (useRuntime)
            solveRootBlocksRuntime(maxIter, tol);
        else if ((explicitRootsActive_ || blockRoots() || !lamp_.alone.empty()) && blockFallbackCooldown_ == 0)
            solveRootBlocks(maxIter, tol, slowLagActive(), adjustedTransfer);
        else {
            if (blockFallbackCooldown_ > 0 && (pipeline || blockRoots() || !lamp_.alone.empty()))
                ++blockCooldownPasses_;
            solveRootsNewton(maxIter, tol);
        }
    } else if (K_ == 0) {
        lastConverged_ = true;
        lastResidual_ = 0.0;
        keptBest_ = false;
        newtonIters_ = 0;
        deviceEvals_ = 0;
    }
    if (folded) {
        auto& L = lamp_;
        const size_t r = size_t(L.port);
        double i = L.drive;
        for (int c = 0; c < K_; ++c)
            i -= c == int(r) ? 0.0 : Z_[r * K_ + c] * ii_[size_t(c)] / (Z_[r * K_ + r] + L.held);
        ii_[r] = i;
        vv_[r] = i * ctrlResValues_[size_t(L.watch)];
    }
    if (lamp_.length)
        stepLampReplay();
    tock(prof_.solve);
    ++ppdiag::sample;

    if (blockFallbacks_ > fallbacksBefore && nPent_ == 2) {
        static std::FILE* trace = [] {
            const char* path = std::getenv("LWDF_PREDLOG");
            std::FILE* f = path ? std::fopen(path, "w") : nullptr;
            if (f)
                std::fprintf(f, "tube,prev,open,answer,prev_vgk,open_vgk,answer_vgk,prev_vsk,open_vsk,answer_vsk,prev_mask,open_mask,answer_mask\n");
            return f;
        }();
        if (trace) {
            const auto regime = [](const Pentode& model, const double* v, unsigned& mask) {
                mask = 0;
                const auto* q = model.reefman.quadric;
                if (q)
                    for (int j = 0; j < 5; ++j)
                        mask |= (v[1] + q->av[j] * v[2] - q->knot[j] > 0.0) << j;
                return v[1] > 0.0 ? 2 : mask ? 1 : 0;
            };
            for (const auto& dev : rootDevices_) {
                if (dev.kind != DeviceKind::Pentode)
                    continue;
                const int o = dev.portBase;
                unsigned previousMask, openMask, answerMask;
                const int previous = regime(pentodes_[dev.compIndex], vvSafe_.data() + o, previousMask);
                const int open = regime(pentodes_[dev.compIndex], vOc_.data() + o, openMask);
                const int answer = regime(pentodes_[dev.compIndex], vv_.data() + o, answerMask);
                std::fprintf(trace, "%d,%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%u,%u\n",
                             dev.compIndex, previous, open, answer,
                             vvSafe_[o + 1], vOc_[o + 1], vv_[o + 1],
                             vvSafe_[o + 2], vOc_[o + 2], vv_[o + 2],
                             previousMask, openMask, answerMask);
            }
        }
    }

    // A block that had to fall back is not paying for itself. Spend a few
    // samples on the joint Newton reference before trying the partition
    // again; this is bounded and cannot disable the block path forever.
    if (blockFallbacks_ > fallbacksBefore)
        blockFallbackCooldown_ = blockFallbackCooldownLimit_;
    else if (blockFallbackCooldown_ > 0)
        --blockFallbackCooldown_;

    // Reconstruct the nodes before committing any state. A solved state
    // that is not a voltage this circuit can hold is a failed solve, not
    // audio: the supply rails are under 500 V, and the reactive plate load
    // stays under 2 kV even on a switch transient. A megavolt iterate used
    // to be written straight into the capacitor/inductor state, so the next
    // sample started from it and never recovered. Reject it, restore the
    // last accepted state, and let the next sample solve again from there.
    double* LIVEWDF_RESTRICT nodeData = node_.data();
    std::memcpy(nodeData, uData, static_cast<size_t>(N_) * sizeof(double));
    if (K_ > 0) {
        const double* LIVEWDF_RESTRICT iiData = ii_.data();
        if (!rootPanels_.empty())
            detail::runtimeKernels().subtractTransferPanels(rootPanels_.data(), rootPanels_.size(),
                rootPanelValues_.data(), xrCols_.data(), iiData, nodeData);
        for (const auto& e : xrRows1_) {
            nodeData[e.node] -= e.val * iiData[e.col];
        }
        for (const auto& e : xrRows2_) {
            nodeData[e.node] -= e.val0 * iiData[e.col0];
            nodeData[e.node] -= e.val1 * iiData[e.col1];
        }
        if (!xrRowsN_.empty()) {
            const int* LIVEWDF_RESTRICT xrCols = xrCols_.data();
            const double* LIVEWDF_RESTRICT xrVals = xrVals_.data();
            for (const auto& e : xrRowsN_) {
                double s = 0.0;
                for (int idx = e.start; idx < e.end; ++idx)
                    s += xrVals[idx] * iiData[xrCols[idx]];
                nodeData[e.node] -= s;
            }
        }
    }
    if (adjustedTransfer && K_ > 0)
        adjustableTransfer_.correctLoadedNodes(ii_.data(), rootLayout_.nodeCol, nodeData);

    return finishStep(sourceValues, sourceCount, tPhase);
}

const std::vector<double>& EngineStep::finishStep(const double* sourceValues, size_t sourceCount,
                                                   std::chrono::steady_clock::time_point phaseTime) {
    const auto tock = [&](double& phase) {
        if (profileOn_)
            phase += std::chrono::duration<double>(std::chrono::steady_clock::now() - phaseTime).count();
    };
    const double* nodeData = node_.data();
    const double* eoffData = eoff_.data();
    bool stateOk = stateAcceptable();
    if (stateOk) {
        double maxVolts = 0.0;
        int maxIndex = -1;
        const double maxLimit = lim_.maxNodeVolts;
        for (int n = 0; n < N_; ++n) {
            const double v = nodeData[n];
            const double av = std::abs(v);
            if (av > maxVolts) {
                maxVolts = av;
                maxIndex = n;
            }
            if (!(av <= maxLimit)) {
                lastRejectNodeIndex_ = n;
                lastRejectNodeVolts_ = v;
                stateOk = false;
                break;
            }
        }
        lastMaxNodeVolts_ = maxVolts;
        lastMaxNodeIndex_ = maxIndex;
    }
    for (int k = 0; stateOk && k < K_; ++k) {
        if (!(std::abs(vv_[static_cast<size_t>(k)]) <= 1e12) ||
            !(std::abs(ii_[static_cast<size_t>(k)]) <= 1e12))
            stateOk = false;
    }
    if (!stateOk) {
        if (K_ > 0) {
            std::memcpy(vv_.data(), vvSafe_.data(), static_cast<size_t>(K_) * sizeof(double));
            std::memcpy(ii_.data(), iiSafe_.data(), static_cast<size_t>(K_) * sizeof(double));
        }
        std::memcpy(node_.data(), nodeSafe_.data(), static_cast<size_t>(N_) * sizeof(double));
        // The failed pass also moved the warm-start history. Reset it to
        // the last accepted root; the next sample must not extrapolate from
        // a rejected iterate.
        vvPrev_ = vvSafe_;
        vvPrev2_ = vvSafe_;
        lastConverged_ = false;
        lastStateRejected_ = true;
        ++stateRejections_;
        ++consecutiveStateRejects_;
        tock(prof_.post);
        return node_;
    }

    // Commit the op-amp dominant-pole state for the next sample. This is
    // the same backward-Euler expression the device law used during the
    // solve, evaluated at the accepted root voltages; recomputing it here
    // (rather than reading it out of the law) keeps one definition of the
    // pole in `OpAmpEngineLaw`.
    for (const auto& dev : rootDevices_) {
        if (dev.kind != DeviceKind::OpAmp)
            continue;
        const size_t ci = dev.compIndex;
        const int o = dev.portBase;
        const OpAmpModel& oa = opAmps_[ci];
        const double vDiff = vv_[static_cast<size_t>(o + 1)] -
                             vv_[static_cast<size_t>(o + 2)];
        double unused = 0.0;
        opAmpVp_[ci] = OpAmpEngineLaw::sourceVoltage(
            oa, vDiff, opAmpVp_[ci], opAmpAlpha_[ci], opAmpMaxDv_[ci],
            false, &unused);
    }

    if (powerSagEnabled_) {
        for (const auto& dev : rootDevices_) {
            if (dev.kind != DeviceKind::OpAmp)
                continue;
            const size_t ci = dev.compIndex;
            if (ci >= opAmpSagActive_.size() || !opAmpSagActive_[ci])
                continue;
            const int o = dev.portBase;
            const double iOut = opAmps_[ci].physicalOutputCurrent(
                vv_[static_cast<size_t>(o)], ii_[static_cast<size_t>(o)]);
            const double iPos = std::max(0.0, iOut);
            const double iNeg = std::max(0.0, -iOut);
            const double vPos = opAmpPsuPos_[ci].tick(iPos);
            const double vNeg = opAmpPsuNeg_[ci].tick(iNeg);
            opAmps_[ci].setVRails(vPos, vNeg);
        }
    }

    // Optical response is slower than one sample. Read the accepted
    // electrical state, then update the LDRs for the next root solve.
    for (auto& photo : photoresistors_) {
        double target;
        if (compiledOptical_) {
            // Cells illuminated by the same lamp still have independent time constants.
            auto& lamp = photoresistors_[photo.lamp];
            if (&lamp == &photo) {
                const auto& control = *circuit_.ctrlRes[photo.index].photo;
                lamp.illumination = control.illumination(
                    latchedPhotoVolts(lamp, control, photoSenseVoltage(photo, node_, sourceValues, sourceCount)));
            }
            target = photo.dark + photo.span * lamp.illumination;
        } else {
            target = photoTargetConductance(photo, node_, sourceValues, sourceCount);
        }
        double& ohms = ctrlResValues_[photo.index];
        const double previousOhms = ohms;
        const double conductance = 1.0 / ohms;
        const double mix = target > conductance ? photo.attack : photo.release;
        ohms = 1.0 / (conductance + mix * (target - conductance));
        if (std::FILE* log = ppdiag::deviceWalkLog()) {
            const auto dev = std::ranges::find_if(rootDevices_, [&](const RootDevice& candidate) {
                return candidate.kind == DeviceKind::ControlledRes && candidate.compIndex == photo.index;
            });
            if (dev != rootDevices_.end() && dev->kind == DeviceKind::ControlledRes) {
                const double branch = vv_[static_cast<size_t>(dev->portBase)];
                const double sense = photoSenseVoltage(photo, node_, sourceValues, sourceCount);
                const char* run = std::getenv("LWDF_WALK_RUN");
                const char* circuit = std::getenv("LWDF_WALK_CIRCUIT");
                const char* group = std::getenv("LWDF_WALK_GROUP");
                std::fprintf(log,
                    "%s,%s,%s,%lld,%lld,-1,photo,accepted,photo,%zu,%s,%s/%s,2,,,%0.17g,%0.17g,,%0.17g,,,%0.17g,0,,,,,,,,,0,0,,%0.17g,%0.17g,%0.17g,1\n",
                    run ? run : "", circuit ? circuit : "", group ? group : "", ppdiag::sample,
                    ppdiag::sample - 256, photo.index, ctrlResNames_[photo.index].c_str(),
                    circuit ? circuit : "", ctrlResNames_[photo.index].c_str(), branch, sense,
                    branch / ohms, 1.0 / ohms, previousOhms, 1.0 / target, ohms);
                std::fflush(log);
            }
        }
    }
    updateVcas(node_, sourceValues, sourceCount);
    updateOtas(node_, sourceValues, sourceCount);
    // Commit the sampled charge only after the electrical solve has been
    // accepted. The BBD's held output then drives the next solve.
    for (auto& bbd : bucketBrigades_)
        bbd.delay.step(bbd.input.voltage(node_, sourceValues, sourceCount),
                       bbd.clock.voltage(node_, sourceValues, sourceCount), T_);

    for (auto& rms : rmsDetectors_)
        rms.detector.step(rms.input.voltage(node_, sourceValues, sourceCount));

    stepHeldRails();
    // Incident waves, then store the reactive states for the next sample.
    // Direct pre-compiled updates avoid acrossPort indirections and branch checks.
    double* LIVEWDF_RESTRICT capStateData = capState_.data();
    for (const auto& u : capBoth_) {
        const double vPort = (nodeData[u.epA] - nodeData[u.epB]) + eoffData[u.portIdx];
        capStateData[u.capIdx] = 2.0 * vPort - capStateData[u.capIdx];
    }
    for (const auto& u : capToGnd_) {
        const double vPort = nodeData[u.ep] + eoffData[u.portIdx];
        capStateData[u.capIdx] = 2.0 * vPort - capStateData[u.capIdx];
    }
    for (const auto& u : capFromGnd_) {
        const double vPort = eoffData[u.portIdx] - nodeData[u.ep];
        capStateData[u.capIdx] = 2.0 * vPort - capStateData[u.capIdx];
    }
    for (const auto& u : capZero_) {
        const double vPort = eoffData[u.portIdx];
        capStateData[u.capIdx] = 2.0 * vPort - capStateData[u.capIdx];
    }

    double* LIVEWDF_RESTRICT indStateData = indState_.data();
    for (const auto& u : indBoth_) {
        const double vPort = (nodeData[u.epA] - nodeData[u.epB]) + eoffData[u.portIdx];
        indStateData[u.indIdx] = 2.0 * vPort + indStateData[u.indIdx];
    }
    for (const auto& u : indToGnd_) {
        const double vPort = nodeData[u.ep] + eoffData[u.portIdx];
        indStateData[u.indIdx] = 2.0 * vPort + indStateData[u.indIdx];
    }
    for (const auto& u : indFromGnd_) {
        const double vPort = eoffData[u.portIdx] - nodeData[u.ep];
        indStateData[u.indIdx] = 2.0 * vPort + indStateData[u.indIdx];
    }
    for (const auto& u : indZero_) {
        const double vPort = eoffData[u.portIdx];
        indStateData[u.indIdx] = 2.0 * vPort + indStateData[u.indIdx];
    }
    tock(prof_.post);
    consecutiveStateRejects_ = 0;
    return node_;
}

}  // namespace wdf
