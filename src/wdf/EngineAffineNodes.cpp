#include "wdf/EngineStep.h"

namespace wdf {

bool EngineStep::buildAffineNodePreview() {
    affineNodes_ = {};
    if (!nOpAmp_ || K_ != 3 * nOpAmp_ || nIsrc_ || nOpAmp_ > 32 ||
        powerSagEnabled_ || hasAdjustedLiveTransfer() || staticMode_)
        return false;
    // The clipped-root certificate excludes positive-feedback latches even if
    // their locally linear equations have a perfectly small residual.
    AffineRootPlan root;
    if (!buildAffineRoot(root))
        return false;
    const int m = nOpAmp_;
    if (int(root.rows.size()) != m)
        return false;

    const int passive = nCaps_ + nInds_;
    const int sourceBase = 1 + passive;
    const int poleBase = sourceBase + nsrc_;
    const int width = poleBase + m;
    const int out = N_ + m;
    if (size_t(width) * out > 1'000'000 ||
        size_t(width) * out * (nLive_ + m) > 30'000'000)
        return false;
    AffineNodePlan plan;
    plan.inputs.resize(width);
    plan.outputs.resize(out);
    plan.coefficients.assign(size_t(width) * out, 0.0);
    plan.residualBound.resize(width);
    for (const auto& amp : root.rows) {
        double bound = 0.0;
        for (int r = 0; r < K_; ++r)
            bound = std::max(bound, std::abs(Z_[size_t(r) * K_ + amp.port]));
        plan.amps.push_back({amp.port, amp.pole, bound});
    }
    std::vector<double> nodes(N_ + 1), open(K_), drives(m);
    const auto offset = [&](const AffineOffset& a, int column) {
        if (!column)
            return a.c0;
        double sum = 0;
        for (const auto& [src, gain] : a.coef)
            if (src + sourceBase == column)
                sum += gain;
        return sum;
    };

    // Compose the existing Xe, Xr and certified op-amp inverse on each
    // formal input column. No calibration signal enters these coefficients.
    for (int col = 0; col < width; ++col) {
        std::fill(nodes.begin(), nodes.end(), 0.0);
        for (int j = 0; j < nLive_; ++j) {
            const int p = net_.liveCols[j];
            double wave = -offset(net_.eoffAff[p], col);
            if (net_.capOf[p] >= 0 && col == 1 + net_.capOf[p]) wave += 1.0;
            if (net_.indOf[p] >= 0 && col == 1 + nCaps_ + net_.indOf[p]) wave -= 1.0;
            for (int n = 0; n < N_; ++n)
                nodes[n] += Xe_[size_t(n) * nLive_ + j] * wave;
        }
        for (int r = 0; r < K_; ++r) {
            const auto& port = rootPortInfo_[r];
            open[r] = (nodes[port.a] - nodes[port.b]) + offset(net_.roffAff[r], col);
        }
        for (int i = 0; i < m; ++i) {
            const auto& amp = root.rows[i];
            drives[i] = amp.conductance * open[amp.port] +
                        amp.differential * (open[amp.port + 1] - open[amp.port + 2]);
            if (col == poleBase + amp.pole) drives[i] += amp.poleGain;
        }
        double* output = plan.coefficients.data() + size_t(col) * out;
        for (int i = 0; i < m; ++i) {
            double current = 0;
            for (int k = 0; k < m; ++k)
                current += root.inverse[size_t(i) * m + k] * drives[k];
            output[N_ + i] = current;
        }
        for (int n = 0; n < N_; ++n) {
            double voltage = nodes[n];
            for (int i = 0; i < m; ++i)
                voltage -= Xr_[size_t(n) * K_ + root.rows[i].port] * output[N_ + i];
            output[n] = voltage;
        }
        // Bound the loaded-port residual of this column, including rounding
        // in both formulations of the node transfer.
        for (int r = 0; r < K_; ++r) {
            const auto& port = rootPortInfo_[r];
            const double pos = port.a < N_ ? output[port.a] : 0.0;
            const double neg = port.b < N_ ? output[port.b] : 0.0;
            double residual = ((pos - neg) + offset(net_.roffAff[r], col)) - open[r];
            double magnitude = std::abs(pos) + std::abs(neg) + std::abs(open[r]);
            for (int j = 0; j < m; ++j) {
                const double drop = Z_[size_t(r) * K_ + root.rows[j].port] * output[N_ + j];
                residual += drop;
                magnitude += std::abs(drop);
            }
            const double bound = std::abs(residual) +
                2.0 * (width + m + 4) * std::numeric_limits<double>::epsilon() * magnitude;
            plan.residualBound[col] = std::max(plan.residualBound[col], bound);
        }
    }
    if (!std::all_of(plan.coefficients.begin(), plan.coefficients.end(),
                     [](double a) { return std::isfinite(a); }))
        return false;
    plan.revision = linearRebuilds();
    affineNodes_ = std::move(plan);
    return true;
}

bool EngineStep::tryAffineNodes(const double* sources, size_t count, double tolerance) noexcept {
    auto& plan = affineNodes_;
    if (staticMode_ || !explicitRootsAllowed_ || powerSagEnabled_ || hasAdjustedLiveTransfer() || count < size_t(nsrc_))
        return false;
    auto next = std::copy(capState_.begin(), capState_.end(), plan.inputs.begin() + 1);
    next = std::copy(indState_.begin(), indState_.end(), next);
    next = std::copy_n(sources, nsrc_, next);
    std::copy(opAmpVp_.begin(), opAmpVp_.end(), next);
    const int nout = int(plan.outputs.size());
    double* LIVEWDF_RESTRICT out = plan.outputs.data();
    const double* LIVEWDF_RESTRICT row = plan.coefficients.data();
    std::copy_n(row, nout, out);
    for (size_t col = 1; col < plan.inputs.size(); ++col) {
        row += nout;
        const double x = plan.inputs[col];
        for (int n = 0; n < nout; ++n)
            out[n] += row[n] * x;
    }
    for (int n = 0; n < N_; ++n)
        if (!(std::abs(out[n]) <= lim_.maxNodeVolts))
            return false;
    const auto node = [&](int n) { return n == N_ ? 0.0 : out[n]; };
    for (int r = 0; r < K_; ++r) {
        const auto& rp = rootPortInfo_[r];
        vt_[r] = (node(rp.a) - node(rp.b)) + roff_[r];
        const double delta = vt_[r] - vv_[r];
        if (!(delta >= -stepDown_[r] && delta <= stepUp_[r]))
            return false;
    }
    double error = plan.residualBound[0];
    for (size_t col = 1; col < plan.inputs.size(); ++col)
        error += std::abs(plan.inputs[col]) * plan.residualBound[col];
    for (int j = 0; j < nOpAmp_; ++j) {
        const auto& amp = plan.amps[j];
        const int r = amp.port, ci = amp.model;
        const auto& model = opAmps_[ci];
        if (model.softRails || model.hasClassABOutput())
            return false;
        double slope;
        const double v = OpAmpEngineLaw::sourceVoltage(model, vt_[r+1] - vt_[r+2], opAmpVp_[ci],
                                                      opAmpAlpha_[ci], opAmpMaxDv_[ci], false, &slope);
        if (!(slope > 0.0))
            return false;
        const double current = (model.outputVoltageWeight() * vt_[r] - v) / model.outputImpedance;
        error += amp.impedanceBound * std::abs(current - out[N_ + j]);
        iv_[r] = current;
        iv_[r+1] = iv_[r+2] = 0.0;
    }
    if (!(error <= tolerance))
        return false;
    commitCompiledRoot(error);
    std::copy_n(out, N_, node_.begin());
    ++affineNodeHits_;
    return true;
}
void EngineStep::swapAffineNodePreview(EngineStep& prepared) noexcept {
    std::swap(affineNodes_, prepared.affineNodes_);
    resetAffineMotion();
    if (!affineNodes_.amps.empty())
        affineNodes_.revision = linearRebuilds();
}

}  // namespace wdf
