#include "wdf/EngineStep.h"

#include <array>

namespace wdf {
namespace {

constexpr int maxPlanPorts = 96;

// With factors stored by column the forward and backward substitutions
// update independent RHS entries in their inner loops.
void applyCircuitTangent(const double* LIVEWDF_RESTRICT lu, const int* pivots,
                         double* LIVEWDF_RESTRICT rhs, int n) noexcept {
    for (int i = 0; i < n; ++i)
        if (i != pivots[i])
            std::swap(rhs[i], rhs[pivots[i]]);
    for (int i = 0; i < n; ++i) {
        const double* column = lu + size_t(i) * n;
        const double value = rhs[i];
        for (int j = i + 1; j < n; ++j)
            rhs[j] -= value * column[j];
    }
    for (int i = n - 1; i >= 0; --i) {
        const double* column = lu + size_t(i) * n;
        const double value = rhs[i] * column[i];  // reciprocal pivot
        rhs[i] = value;
        for (int j = 0; j < i; ++j)
            rhs[j] -= value * column[j];
    }
}

// As the hard rail/slew clamps engage, each output source's incremental
// input gain is multiplied by some s in [0,1]. With output conductances
// closed first, the reduced matrix is I + diag(s) D. If I+D has a positive
// diagonal and can be scaled to strict row dominance, every one of these
// intermediate matrices is nonsingular. Subtracting any two putative
// solutions gives one such homogeneous system, hence the solution is unique.
bool uniqueClippedRoot(const std::vector<double>& full, std::vector<double> baseTranspose, int n) {
    std::vector<double> comparison(full.size());
    for (int r = 0; r < n; ++r)
        for (int c = 0; c < n; ++c)
            comparison[size_t(r) * n + c] = full[size_t(r) * n + c] - baseTranspose[size_t(c) * n + r];
    std::vector<int> pivots(n);
    if (!detail::factorLUData(baseTranspose.data(), pivots.data(), n))
        return false;
    for (int r = 0; r < n; ++r) {
        double* row = comparison.data() + size_t(r) * n;
        detail::solveLUData(baseTranspose.data(), pivots.data(), row, n);
        for (int c = 0; c < n; ++c)
            row[c] = r == c ? row[c] + 1.0 : -std::abs(row[c]);
        if (!(row[r] > 0.0))
            return false;
    }
    // Find weights and then independently verify the inequalities. Allows
    // one-way feed-forward coupling of arbitrary gain between stages.
    auto factors = comparison;
    std::vector<double> weights(n, 1.0);
    if (!detail::factorLUData(factors.data(), pivots.data(), n))
        return false;
    detail::solveLUData(factors.data(), pivots.data(), weights.data(), n);
    for (int r = 0; r < n; ++r) {
        if (!(weights[r] > 0) || !std::isfinite(weights[r]))
            return false;
        const double* row = comparison.data() + size_t(r) * n;
        double offDiagonal = 0.0;
        for (int c = 0; c < n; ++c)
            if (c != r)
                offDiagonal += std::abs(row[c] * weights[c]);
        const double diagonal = row[r] * weights[r];
        if (!(diagonal - offDiagonal > 1e-10 * std::max({diagonal, offDiagonal, 1.0})))
            return false;
    }
    return true;
}

}  // namespace

void EngineStep::prepareCircuitPlan() {
    circuitPlanRevision_ = linearRebuilds();
    circuitPlanCooldown_ = 0;
    circuitPlanRetry_ = 8;
    affineRoot_ = {};
    certifiedResistance_.clear();
    affineRootCooldown_ = 0;
    affineRootRetry_ = 8;
    driveCoefficients_.clear();
    rootFactors_.clear();
    circuitResidualColumns_.clear();
    if (hasAdjustedLiveTransfer())
        return;

    // Compose Xe * (reactive wave - source offset) once. The final input
    // column carries the constant offset. Current sources are added by step().
    const size_t width = size_t(nCaps_) + nInds_ + nsrc_ + 1;
    const size_t entries = size_t(N_) * width;
    if (!networkFactors_.active && nLive_ > 0 && entries <= xeVals_.size() + size_t(nLive_) + size_t(N_) * 2
        && nsrc_ <= 127) {
        driveInputs_.resize(width);
        driveCoefficients_.assign(entries, 0.0);
        for (int n = 0; n < N_; ++n) {
            double* row = driveCoefficients_.data() + size_t(n) * width;
            for (int j = xeRowOffsets_[n]; j < xeRowOffsets_[n + 1]; ++j) {
                const int port = net_.liveCols[xeCols_[j]];
                const double gain = xeVals_[j];
                if (net_.capOf[port] >= 0)
                    row[net_.capOf[port]] += gain;
                if (net_.indOf[port] >= 0)
                    row[nCaps_ + net_.indOf[port]] -= gain;
                const auto& offset = net_.eoffAff[port];
                for (auto [source, factor] : offset.coef)
                    row[nCaps_ + nInds_ + source] -= gain * factor;
                row[width - 1] -= gain * offset.c0;
            }
        }
        if (!std::all_of(driveCoefficients_.begin(), driveCoefficients_.end(),
                         [](double x) { return std::isfinite(x); }))
            driveCoefficients_.clear();
    }

    // Prepare the tangent of the *loaded* network. Triode domain pins require
    // the existing branch-selection solver. Keep existing exact scalar paths.
    if (K_ < 2 || K_ > maxPlanPorts || nTri_ || explicitRootsActive_ || !explicitRootsAllowed_)
        return;
    std::vector<double> currentsAtBias, jacobian;
    const auto evals = deviceEvals_;
    currents(vv_, currentsAtBias, jacobian);
    deviceEvals_ = evals;
    if (nOpAmp_) {
        prepareAffineRoot();
        if (affineRoot_.rows.empty())
            return; // A valid residual cannot identify the branch in a latch.
    }
    rootFactors_.assign(size_t(K_) * K_, 0.0);
    rootPivots_.resize(K_);
    if (!factorCircuitTangent(jacobian)) {
        rootFactors_.clear();
        return;
    }

    // Small roots already have fixed-size residual kernels. Larger roots
    // stream each current's coefficients across independent equations.
    if (nRed_ > 16) {
        circuitResidualColumns_.resize(size_t(nRed_) * K_);
        for (int j = 0; j < nRed_; ++j)
            for (int r = 0; r < K_; ++r)
                circuitResidualColumns_[size_t(j) * K_ + r] = Z_[size_t(r) * K_ + redCol_[j]];
    }
}

// An ordinary op-amp injects only one current despite its three voltage
// ports. For i = J v + c and v = vOc - Z i, solve (I + J Z) i = J vOc + c.
// The matrix is fixed whenever the op-amps remain inside their rails and
// slew limits. Keep curved devices on the nonlinear predictor.
bool EngineStep::buildAffineRoot(AffineRootPlan& result) const {
    if (!nOpAmp_ || powerSagEnabled_ || !photoresistors_.empty() ||
        !vcas_.empty() || !otas_.empty() || !dbx200Biases_.empty())
        return false;
    AffineRootPlan plan;
    for (const auto& dev : rootLayout_.devices) {
        const size_t ci = dev.compIndex;
        if (dev.kind == RootKind::ControlledRes && !comp_.otaCurrentLaw[ci] &&
            comp_.dbx200Crossover[ci] == 0.0) {
            const double r = ctrlResValues_[ci];
            plan.rows.push_back({dev.portBase, -1, r > 1e-12 ? 1.0 / r : 1e12, 0.0, 0.0});
        } else if (dev.kind == RootKind::OpAmp && !opAmps_[ci].softRails &&
                   !opAmps_[ci].hasClassABOutput()) {
            const auto& op = opAmps_[ci];
            const double alpha = std::clamp(opAmpAlpha_[ci], 0.0, 1.0);
            const double g = 1.0 / op.outputImpedance;
            const double pole = -g / (1.0 + alpha);
            plan.rows.push_back({dev.portBase, dev.compIndex, g * op.outputVoltageWeight(),
                                 pole * alpha * op.openLoopGain, pole});
        } else {
            return false;
        }
    }
    const int m = static_cast<int>(plan.rows.size());
    const size_t entries = size_t(m) * m;
    std::vector<double> lu(entries), baseTranspose(entries);
    std::vector<int> pivots(m);
    plan.impedance.resize(size_t(K_) * m);
    for (int r = 0; r < K_; ++r)
        for (int j = 0; j < m; ++j)
            plan.impedance[size_t(r) * m + j] = Z_[size_t(r) * K_ + plan.rows[j].port];
    for (int r = 0; r < m; ++r) {
        const auto& row = plan.rows[r];
        for (int j = 0; j < m; ++j) {
            double value = r == j ? 1.0 : 0.0;
            value += row.conductance * plan.impedance[size_t(row.port) * m + j];
            baseTranspose[size_t(j) * m + r] = value;
            if (row.pole >= 0)
                value += row.differential * (plan.impedance[size_t(row.port + 1) * m + j] -
                                             plan.impedance[size_t(row.port + 2) * m + j]);
            lu[size_t(r) * m + j] = value;
        }
    }
    if (!uniqueClippedRoot(lu, std::move(baseTranspose), m) ||
        !detail::factorLUData(lu.data(), pivots.data(), m))
        return false;
    plan.inverse.resize(entries);
    std::vector<double> column(m);
    for (int j = 0; j < m; ++j) {
        std::fill(column.begin(), column.end(), 0.0);
        column[j] = 1.0;
        detail::solveLUData(lu.data(), pivots.data(), column.data(), m);
        for (int r = 0; r < m; ++r) {
            if (!std::isfinite(column[r]))
                return false;
            plan.inverse[size_t(r) * m + j] = column[r];
        }
    }
    plan.drive.resize(m);
    plan.current.resize(m);
    result = std::move(plan);
    return true;
}

void EngineStep::prepareAffineRoot() {
    if (buildAffineRoot(affineRoot_))
        certifiedResistance_ = ctrlResValues_;
}

bool EngineStep::tryAffineRoot(double tolerance) noexcept {
    auto& plan = affineRoot_;
    const size_t m = plan.rows.size();
    for (size_t j = 0; j < m; ++j) {
        const auto& row = plan.rows[j];
        double value = row.conductance * vOc_[row.port];
        if (row.pole >= 0) {
            value += row.poleGain * opAmpVp_[row.pole] +
                     row.differential * (vOc_[row.port + 1] - vOc_[row.port + 2]);
        }
        plan.drive[j] = value;
    }
    for (size_t j = 0; j < m; ++j) {
        const double* row = plan.inverse.data() + j * m;
        double value = 0.0;
        for (size_t k = 0; k < m; ++k)
            value += row[k] * plan.drive[k];
        plan.current[j] = value;
    }
    for (int r = 0; r < K_; ++r) {
        const double* row = plan.impedance.data() + size_t(r) * m;
        double drop = 0.0;
        for (size_t j = 0; j < m; ++j)
            drop += row[j] * plan.current[j];
        vt_[r] = vOc_[r] - drop;
        const double delta = vt_[r] - vv_[r];
        if (!std::isfinite(vt_[r]) || !(delta >= -stepDown_[r] && delta <= stepUp_[r]))
            return false;
    }
    if (!opAmpsOnLinearBranch(vt_))
        return false;

    // Re-evaluate the original device law, including instantaneous rail/slew
    // clamps and live resistance values. Only an actual KCL solution commits.
    for (const auto& dev : rootDevices_)
        currentsForDevice(dev, vt_.data(), iv_.data(), nullptr);
    if (!std::all_of(iv_.begin(), iv_.end(), [](double value) { return std::isfinite(value); }))
        return false;
    double error = 0.0;
    for (size_t j = 0; j < m; ++j)
        plan.current[j] = iv_[plan.rows[j].port];
    for (int r = 0; r < K_; ++r) {
        const double* row = plan.impedance.data() + size_t(r) * m;
        double residual = vt_[r] - vOc_[r];
        for (size_t j = 0; j < m; ++j)
            residual += row[j] * plan.current[j];
        if (!(std::abs(residual) <= tolerance))
            return false;
        error = std::max(error, std::abs(residual));
    }
    acceptCircuitRoot(error);
    return true;
}

bool EngineStep::factorCircuitTangent(const std::vector<double>& jacobian) noexcept {
    // Factor I + Z*di/dv in existing scratch. A failed refresh leaves the
    // previous plan usable. Each Jacobian block is local to its device.
    std::fill(A_.begin(), A_.end(), 0.0);
    for (int r = 0; r < K_; ++r) {
        auto* row = A_.data() + size_t(r) * K_;
        row[r] = 1.0;
        for (const auto& dev : rootDevices_) {
            const int end = dev.portBase + dev.numPorts;
            for (int k = dev.portBase; k < end; ++k) {
                if (redRow_[k] < 0)
                    continue; // A dependent terminal current is already a port return.
                const double z = Z_[size_t(r) * K_ + k];
                for (int c = dev.portBase; c < end; ++c)
                    row[c] += z * jacobian[size_t(k) * K_ + c];
            }
        }
    }
    std::array<int, maxPlanPorts> pivots;
    if (!detail::factorLUData(A_.data(), pivots.data(), K_))
        return false;
    for (int i = 0; i < K_; ++i) {
        double& diagonal = A_[size_t(i) * K_ + i];
        diagonal = 1.0 / diagonal;
        for (int j = 0; j < i; ++j)
            std::swap(A_[size_t(i) * K_ + j], A_[size_t(j) * K_ + i]);
    }
    if (!std::all_of(A_.begin(), A_.end(), [](double x) { return std::isfinite(x); }))
        return false;
    std::copy_n(pivots.begin(), K_, rootPivots_.begin());
    rootFactors_.swap(A_);
    return true;
}

bool EngineStep::applyCompiledDrive(const double* sources, size_t count) noexcept {
    auto input = std::copy(capState_.begin(), capState_.end(), driveInputs_.begin());
    input = std::copy(indState_.begin(), indState_.end(), input);
    const size_t supplied = std::min(count, size_t(nsrc_));
    if (supplied)
        std::copy_n(sources, supplied, input);
    std::fill(input + supplied, input + nsrc_, 0.0);
    driveInputs_.back() = 1.0;
    const size_t width = driveInputs_.size();
    const double* row = driveCoefficients_.data();
    for (int n = 0; n < N_; ++n, row += width) {
        double v = row[width - 1];
        for (size_t j = 0; j + 1 < width; ++j)
            v += row[j] * driveInputs_[j];
        if (!std::isfinite(v))
            return false;
        u_[n] = v;
    }
    u_[N_] = 0.0;
    return true;
}

// A Schmitt trigger can have both a valid saturated root and a valid interior
// root at the same input. A small KCL residual cannot choose between them.
// Follow the native solver whenever the old or proposed state leaves the same
// unslewed, unsaturated piece of the device law.
bool EngineStep::opAmpsOnLinearBranch(const std::vector<double>& voltage) const noexcept {
    if (powerSagEnabled_)
        return false;
    for (const auto& dev : rootDevices_) {
        if (dev.kind != DeviceKind::OpAmp)
            continue;
        const auto ci = dev.compIndex;
        const auto& op = opAmps_[ci];
        const double pole = opAmpVp_[ci];
        if (op.softRails || op.hasClassABOutput() || !(pole > op.railLow() && pole < op.railHigh()))
            return false;
        double slope;
        OpAmpEngineLaw::sourceVoltage(op, voltage[dev.portBase + 1] - voltage[dev.portBase + 2],
                                     pole, opAmpAlpha_[ci], opAmpMaxDv_[ci], false, &slope);
        if (!(slope > 0.0))
            return false;
    }
    return true;
}

bool EngineStep::tryCompiledRoot(double tolerance) noexcept {
    if (nOpAmp_ && (certifiedResistance_ != ctrlResValues_ || !opAmpsOnLinearBranch(vv_)))
        return false;
    if (!affineRoot_.rows.empty() && !powerSagEnabled_) {
        if (affineRootCooldown_ > 0) {
            --affineRootCooldown_;
        } else {
            ++affineRootAttempts_;
            ++circuitPlanAttempts_;
            if (tryAffineRoot(tolerance)) {
                ++affineRootHits_;
                affineRootRetry_ = 8;
                return true;
            }
            affineRootCooldown_ = affineRootRetry_;
            affineRootRetry_ = std::min(affineRootRetry_ * 2, 256);
        }
    }
    if (rootFactors_.empty())
        return false;
    if (circuitPlanCooldown_ > 0) {
        --circuitPlanCooldown_;
        return false;
    }
    ++circuitPlanAttempts_;
    circuitPlanCooldown_ = circuitPlanRetry_;
    circuitPlanRetry_ = std::min(circuitPlanRetry_ * 2, 256);
    // Reuse the loaded-network tangent as long as it contracts the full
    // residual. One refresh and at most four corrections bound the trial.
    auto ctx = makeNewtonContext();
    for (int r = 0; r < K_; ++r)
        vt_[r] = vv_[r] + std::clamp(vv_[r] - vvPrev2_[r], -predDown_[r], predUp_[r]);
    double error = 0.0;
    double previous = std::numeric_limits<double>::infinity();
    bool refreshed = false;
    for (int pass = 0; ; ++pass) {
        solver::RootNewton::currentsOnlyInto(ctx, vt_, iv_);
        if (circuitResidualColumns_.empty()) {
            solver::RootNewton::residualInto(ctx, vt_, vOc_, Z_, iv_, ft_);
        } else {
            double* LIVEWDF_RESTRICT residual = ft_.data();
            for (int r = 0; r < K_; ++r)
                residual[r] = vt_[r] - vOc_[r];
            for (int j = 0; j < nRed_; ++j) {
                const double current = iv_[redCol_[j]];
                const double* LIVEWDF_RESTRICT column = circuitResidualColumns_.data() + size_t(j) * K_;
                for (int r = 0; r < K_; ++r)
                    residual[r] += column[r] * current;
            }
        }
        error = 0.0;
        for (int r = 0; r < K_; ++r) {
            if (!std::isfinite(ft_[r]) || !std::isfinite(iv_[r]) || !std::isfinite(vt_[r]))
                return false;
            error = std::max(error, std::abs(ft_[r]));
        }
        if (error <= tolerance)
            break;
        if (pass == 4)
            return false;
        if (!refreshed && pass > 0 && error > 0.15 * previous) {
            solver::RootNewton::currentsInto(ctx, vt_, iv_, Jt_);
            if (!factorCircuitTangent(Jt_))
                return false;
            refreshed = true;
        } else if (error >= previous) {
            return false;
        }
        previous = error;
        applyCircuitTangent(rootFactors_.data(), rootPivots_.data(), ft_.data(), K_);
        for (int r = 0; r < K_; ++r) {
            vt_[r] -= ft_[r];
            const double delta = vt_[r] - vv_[r];
            if (!(delta >= -stepDown_[r] && delta <= stepUp_[r]))
                return false;
        }
    }
    if (!circuitRootContinuous() || (nOpAmp_ && !opAmpsOnLinearBranch(vt_)))
        return false;
    acceptCircuitRoot(error);
    return true;
}

bool EngineStep::circuitRootContinuous() const noexcept {
    // A small residual alone cannot distinguish two stable bias points.
    // Leave large forward-junction excursions to the nonlinear step limiters.
    for (const auto& junction : junctionLimiters_) {
        const int r = junction.port;
        const double sign = junction.negate ? -1.0 : 1.0;
        if (std::max(sign * vt_[r], sign * vv_[r]) > 0.0 &&
            std::abs(vt_[r] - vv_[r]) > 2.0 * junction.model->nvt)
            return false;
    }
    for (const auto& dev : rootDevices_) {
        if (dev.kind != DeviceKind::Bjt)
            continue;
        const auto& bjt = bjts_[dev.compIndex];
        const int o = dev.portBase;
        const double sign = bjt.pnp ? -1.0 : 1.0;
        const double be = sign * vt_[o + 1], oldBe = sign * vv_[o + 1];
        const double bc = be - sign * vt_[o], oldBc = oldBe - sign * vv_[o];
        if ((std::max(be, oldBe) > 0.0 && std::abs(be - oldBe) > 2.0 * bjt.nfVt) ||
            (std::max(bc, oldBc) > 0.0 && std::abs(bc - oldBc) > 2.0 * bjt.nrVt))
            return false;
    }
    return true;
}

void EngineStep::acceptCircuitRoot(double error) noexcept {
    circuitPlanCooldown_ = 0;
    circuitPlanRetry_ = 8;
    ++circuitPlanHits_;
    commitCompiledRoot(error);
}

void EngineStep::commitCompiledRoot(double error) noexcept {
    vvPrev2_ = vvPrev_;
    vvPrev_ = vt_;
    vv_.swap(vt_);
    ii_.swap(iv_);
    jointLastIters_ = jointLastTrials_ = 0;
    lastConverged_ = true;
    lastResidual_ = error;
}

} // namespace wdf
