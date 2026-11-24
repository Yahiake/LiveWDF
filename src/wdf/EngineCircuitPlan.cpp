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

}  // namespace

void EngineStep::prepareCircuitPlan() {
    circuitPlanRevision_ = linearRebuilds();
    circuitPlanCooldown_ = 0;
    circuitPlanRetry_ = 8;
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

bool EngineStep::tryCompiledRoot(double tolerance) noexcept {
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
        error = maxAbs(ft_, K_);
        if (!std::isfinite(error))
            return false;
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
    circuitPlanCooldown_ = 0;
    circuitPlanRetry_ = 8;
    vvPrev2_ = vvPrev_;
    vvPrev_ = vt_;
    vv_.swap(vt_);
    ii_.swap(iv_);
    ++circuitPlanHits_;
    jointLastIters_ = jointLastTrials_ = 0;
    lastConverged_ = true;
    lastResidual_ = error;
    return true;
}

} // namespace wdf
