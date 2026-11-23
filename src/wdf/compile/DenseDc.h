#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#include "circuit/Network.h"
#include "circuit/RootLayout.h"
#include "devices/Bjt.h"
#include "devices/OpAmp.h"
#include "spice/Net.h"
#include "spice/RootTransfer.h"
#include "wdf/DenseTopology.h"

namespace wdf::engine {

class DenseDc {
public:
    template <typename EngineType>
    static bool solveDcAt(const EngineType& eng,
                          std::vector<double>& node,
                          std::vector<double>& rootV,
                          double sourceScale,
                          double gminOverride,
                          double tol,
                          const std::vector<double>* warmStart) {
        const bool savedStatic = eng.staticMode_;
        eng.staticMode_ = true;
        struct StaticModeGuard {
            bool& flag;
            bool saved;
            ~StaticModeGuard() { flag = saved; }
        } staticModeGuard{eng.staticMode_, savedStatic};

        std::vector<ElemPort> elements;
        elements.reserve(static_cast<size_t>(eng.P_));
        for (int j = 0; j < eng.P_; ++j) {
            double g = eng.net_.eG[j];
            if (eng.net_.capOf[j] >= 0)
                g = 0.0;
            else if (eng.net_.indOf[j] >= 0)
                g = 1.0 / kIndDcR;
            elements.push_back({eng.net_.epA[j], eng.net_.epB[j], g});
        }
        Dense Xe, Xr;
        const bool directTransfer =
            rootTransfer(elements, eng.net_.allRoots, eng.N_, Xe, Xr, eng.net_.mutualPairs, eng.net_.vcvsList);
        if (std::getenv("LIVEWDF_DEBUG_DC"))
            std::fprintf(stderr, "solveDcAt scale %.6g gmin %.3g directTransfer %d\n",
                         sourceScale, gminOverride, directTransfer ? 1 : 0);

        // Move the op-amp's output conductance to the transfer matrix if
        // an unloaded output makes the passive system singular or the direct
        // nonlinear pass failed. Subtracting the same term in the device
        // currents leaves the circuit equations unchanged.
        std::vector<std::pair<int, double>> embeddedConductances;
        if (!directTransfer || gminOverride > 0.0) {
            for (const auto& dev : eng.rootLayout_.devices) {
                if (dev.kind != RootKind::OpAmp)
                    continue;
                const auto& model = eng.opAmps_[static_cast<size_t>(dev.compIndex)];
                if (model.outputResistanceEmbedded || !(model.outputImpedance > 0.0) ||
                    !std::isfinite(model.outputImpedance))
                    continue;
                const double g = 1.0 / model.outputImpedance;
                const auto& port = eng.net_.rp[static_cast<size_t>(dev.portBase)];
                elements.push_back({port[0], port[1], g});
                embeddedConductances.emplace_back(dev.portBase, g);
            }
        }
        const bool stabilizedTransfer =
            !directTransfer && gminOverride == 0.0 && !embeddedConductances.empty() &&
            rootTransfer(elements, eng.net_.allRoots, eng.N_, Xe, Xr, eng.net_.mutualPairs, eng.net_.vcvsList);
        if ((!directTransfer && !stabilizedTransfer) || gminOverride > 0.0) {
            std::vector<ElemPort> leaky = elements;
            leaky.reserve(elements.size() + static_cast<size_t>(eng.N_));
            if (gminOverride > 0.0) {
                // Removing capacitors can isolate a whole resistive island.
                // A nonzero diagonal only says a node has conductance to a
                // neighbor. Find the connected components and give each one
                // without a reference a single weak path to ground.
                const int ground = eng.N_;
                std::vector<int> parent(static_cast<size_t>(eng.N_ + 1));
                for (int n = 0; n <= eng.N_; ++n)
                    parent[static_cast<size_t>(n)] = n;
                const auto root = [&](int nodeIndex) {
                    while (parent[static_cast<size_t>(nodeIndex)] != nodeIndex) {
                        const int next = parent[static_cast<size_t>(nodeIndex)];
                        parent[static_cast<size_t>(nodeIndex)] =
                            parent[static_cast<size_t>(next)];
                        nodeIndex = next;
                    }
                    return nodeIndex;
                };
                for (const auto& e : elements) {
                    if (e.g <= 0.0)
                        continue;
                    const int a = root(e.a >= 0 ? e.a : ground);
                    const int b = root(e.b >= 0 ? e.b : ground);
                    parent[static_cast<size_t>(a)] = b;
                }
                const int groundRoot = root(ground);
                std::vector<int> floating;
                for (int n = 0; n < eng.N_; ++n) {
                    if (root(n) == n && n != groundRoot) {
                        floating.push_back(n);
                        leaky.push_back({n, -1, gminOverride});
                    }
                }
                if (std::getenv("LIVEWDF_DEBUG_DC")) {
                    std::fprintf(stderr, "solveDcAt scale %.6g gmin %.3g: %zu floating of %d:",
                                 sourceScale, gminOverride, floating.size(), eng.N_);
                    for (int n : floating)
                        std::fprintf(stderr, " %d", n);
                    std::fprintf(stderr, "\n");
                }
            } else {
                for (int n = 0; n < eng.N_; ++n)
                    leaky.push_back({n, -1, kGminDc});
            }

            if (!rootTransfer(leaky, eng.net_.allRoots, eng.N_, Xe, Xr,
                              eng.net_.mutualPairs, eng.net_.vcvsList)) {
                if (directTransfer || gminOverride <= 0.0)
                    return false;
                // The passive graph cannot account for an active element's
                // transconductance cancelling an otherwise valid path.
                // Retry with weak references on all nodes if still singular.
                leaky = elements;
                for (int n = 0; n < eng.N_; ++n)
                    leaky.push_back({n, -1, gminOverride});
                if (!rootTransfer(leaky, eng.net_.allRoots, eng.N_, Xe, Xr,
                                  eng.net_.mutualPairs, eng.net_.vcvsList))
                    return false;
            }
        }

        std::vector<double> eoff(eng.P_), roff(eng.K_);
        for (int j = 0; j < eng.P_; ++j)
            eoff[static_cast<size_t>(j)] =
                sourceScale * eng.net_.eoffAff[static_cast<size_t>(j)].eval(eng.dcValues_);
        for (int r = 0; r < eng.K_; ++r)
            roff[static_cast<size_t>(r)] =
                sourceScale * eng.net_.roffAff[static_cast<size_t>(r)].eval(eng.dcValues_);

        const auto dcCurrents = [&](const auto& volts, auto& currents, auto& jacobian) {
            eng.currents(volts, currents, jacobian);
            for (const auto& [port, g] : embeddedConductances) {
                const size_t index = static_cast<size_t>(port);
                // The synthetic passive port sees node voltages only, with
                // no affine contribution from an ideal source on either pin.
                currents[index] -= g * (volts[index] - roff[index]);
                jacobian[index * eng.K_ + index] -= g;
            }
        };

        std::vector<double> iIsrc(static_cast<size_t>(eng.nIsrc_));
        for (int k = 0; k < eng.nIsrc_; ++k)
            iIsrc[static_cast<size_t>(k)] =
                sourceScale * eng.comp_.isrc[static_cast<size_t>(k)].second.dc;

        std::vector<double> u(static_cast<size_t>(eng.N_), 0.0);
        for (int n = 0; n < eng.N_; ++n) {
            double s = 0.0;
            for (int j = 0; j < eng.P_; ++j)
                s += Xe[n][j] * (0.0 - eoff[static_cast<size_t>(j)]);
            for (int k = 0; k < eng.nIsrc_; ++k)
                s -= Xr[n][eng.K_ + k] * iIsrc[static_cast<size_t>(k)];
            u[static_cast<size_t>(n)] = s;
        }
        std::vector<double> vOc(static_cast<size_t>(eng.K_));
        for (int r = 0; r < eng.K_; ++r)
            vOc[static_cast<size_t>(r)] = acrossVec(eng.net_.rp[static_cast<size_t>(r)], u) + roff[static_cast<size_t>(r)];

        std::vector<double> Z(static_cast<size_t>(eng.K_) * eng.K_);
        for (int r = 0; r < eng.K_; ++r)
            for (int c = 0; c < eng.K_; ++c)
                Z[static_cast<size_t>(r) * eng.K_ + c] =
                    acrossMat(eng.net_.rp[static_cast<size_t>(r)], Xr, c);

        std::vector<double> v = vOc, i, Jm;
        for (const auto& dev : eng.rootLayout_.devices) {
            switch (dev.kind) {
            case RootKind::Triode:
                break;
            case RootKind::Diode:
                v[static_cast<size_t>(dev.portBase)] = 0.0;
                break;
            case RootKind::Pentode:
                v[static_cast<size_t>(dev.portBase + 1)] = 0.0;
                break;
            case RootKind::Bjt: {
                const BjtModel& bjt = eng.bjts_[static_cast<size_t>(dev.compIndex)];
                v[static_cast<size_t>(dev.portBase + 1)] = bjt.pnp ? -0.3 : 0.3;
                break;
            }
            case RootKind::ControlledRes:
            case RootKind::OpAmp:
            case RootKind::Jfet:
            case RootKind::Mosfet:
                break;
            }
        }
        if (warmStart != nullptr)
            v = *warmStart;

        bool bjtOnlyNonlinear = !eng.rootLayout_.devices.empty();
        for (const auto& dev : eng.rootLayout_.devices) {
            if (dev.kind != RootKind::Bjt) {
                bjtOnlyNonlinear = false;
                break;
            }
            const int bePort = dev.portBase + 1;
            double zMax = 0.0;
            for (int c = 0; c < eng.K_; ++c) {
                double zVal = std::fabs(Z[static_cast<size_t>(bePort) * eng.K_ + c]);
                if (zVal > zMax) zMax = zVal;
            }
            if (zMax > 1e-6) {
                bjtOnlyNonlinear = false;
                break;
            }
        }

        bool converged = false;
        int lastIt = 0;
        for (int it = 0; it < 200; ++it) {
            lastIt = it;
            dcCurrents(v, i, Jm);
            std::vector<double> F = eng.residual(v, vOc, Z, i);
            double base = 0.0;
            for (double e : F)
                base = std::max(base, std::fabs(e));
            if (base < tol) {
                converged = true;
                break;
            }
            std::vector<double> A = eng.jacobian(Z, Jm);
            std::vector<double> rhs(static_cast<size_t>(eng.K_));
            for (int r = 0; r < eng.K_; ++r)
                rhs[static_cast<size_t>(r)] = -F[static_cast<size_t>(r)];
            if (!detail::solveSmall(A, rhs, eng.K_))
                break;

            double maxStep = 0.0;
            double maxV = 1.0;
            for (int r = 0; r < eng.K_; ++r) {
                maxStep = std::max(maxStep, std::fabs(rhs[static_cast<size_t>(r)]));
                maxV = std::max(maxV, std::fabs(v[static_cast<size_t>(r)]));
            }
            if (eng.nOpAmp_ > 0 && maxStep <= 1e-9 * maxV &&
                base < std::max(tol, 1e-7 * maxV)) {
                converged = true;
                break;
            }

            if (eng.nOpAmp_ > 0) {
                double stepLimit = 1.0;
                for (const auto& dev : eng.rootLayout_.devices) {
                    if (dev.kind != RootKind::OpAmp)
                        continue;
                    const int o = dev.portBase;
                    const OpAmpModel& oa = eng.opAmps_[dev.compIndex];
                    const double vdOld = v[static_cast<size_t>(o + 1)] -
                                         v[static_cast<size_t>(o + 2)];
                    const double vdTrial = vdOld + rhs[static_cast<size_t>(o + 1)] -
                                           rhs[static_cast<size_t>(o + 2)];
                    const int region = OpAmpEngineLaw::staticRailRegion(oa, vdOld);
                    if (region == 0)
                        continue;
                    const double nodeScale = std::max(
                        {1.0, std::fabs(v[static_cast<size_t>(o + 1)]),
                         std::fabs(v[static_cast<size_t>(o + 2)])});
                    const double margin = 64.0 * std::numeric_limits<double>::epsilon() * nodeScale;
                    const double boundary = region > 0
                                                ? oa.railHigh() / oa.openLoopGain - margin
                                                : oa.railLow() / oa.openLoopGain + margin;
                    const bool crosses = region > 0 ? vdTrial < boundary : vdTrial > boundary;
                    if (!crosses)
                        continue;
                    const double denom = vdTrial - vdOld;
                    if (denom != 0.0)
                        stepLimit = std::min(stepLimit, (boundary - vdOld) / denom);
                }
                if (stepLimit < 1.0) {
                    stepLimit = std::clamp(stepLimit, 0.0, 1.0);
                    for (int r = 0; r < eng.K_; ++r)
                        rhs[static_cast<size_t>(r)] *= stepLimit;
                }
            }
            if (bjtOnlyNonlinear) {
                for (const auto& dev : eng.rootLayout_.devices) {
                    const int o = dev.portBase;
                    const BjtModel& m = eng.bjts_[static_cast<size_t>(dev.compIndex)];
                    const double vbe = v[static_cast<size_t>(o + 1)];
                    const double vbc = vbe - v[static_cast<size_t>(o)];
                    const double vbeRaw = vbe + rhs[static_cast<size_t>(o + 1)];
                    const double vbcRaw = vbc + rhs[static_cast<size_t>(o + 1)] -
                                          rhs[static_cast<size_t>(o)];
                    const double vbeNext = m.compensateBe(vbeRaw);
                    const double vbcNext = m.compensateBc(vbcRaw);
                    const double newBeStep = vbeNext - vbe;
                    const double newCeStep = (vbeNext - vbcNext) - (vbe - vbc);

                    const int e0 = o + 2;
                    const double rhs_e0_orig = rhs[static_cast<size_t>(e0)];
                    const double rhs_ce_orig = rhs[static_cast<size_t>(o)];
                    const double rhs_be_orig = rhs[static_cast<size_t>(o + 1)];

                    const double Z_e0_ce = Z[static_cast<size_t>(e0) * eng.K_ + o];
                    const double Z_e0_be = Z[static_cast<size_t>(e0) * eng.K_ + o + 1];
                    const double Z_e0_e0 = Z[static_cast<size_t>(e0) * eng.K_ + e0];
                    const double diC_dVce = Jm[static_cast<size_t>(o) * eng.K_ + o];
                    const double diC_dVbe = Jm[static_cast<size_t>(o) * eng.K_ + o + 1];
                    const double diB_dVce = Jm[static_cast<size_t>(o + 1) * eng.K_ + o];
                    const double diB_dVbe = Jm[static_cast<size_t>(o + 1) * eng.K_ + o + 1];
                    const double diE_dVce = -diC_dVce - diB_dVce;
                    const double diE_dVbe = -diC_dVbe - diB_dVbe;
                    const double A_e0_ce = Z_e0_ce * diC_dVce + Z_e0_be * diB_dVce + Z_e0_e0 * diE_dVce;
                    const double A_e0_be = Z_e0_ce * diC_dVbe + Z_e0_be * diB_dVbe + Z_e0_e0 * diE_dVbe;
                    rhs[static_cast<size_t>(o + 1)] = newBeStep;
                    rhs[static_cast<size_t>(o)] = newCeStep;
                    rhs[static_cast<size_t>(e0)] = rhs_e0_orig
                                                 + A_e0_ce * (rhs_ce_orig - newCeStep)
                                                 + A_e0_be * (rhs_be_orig - newBeStep);
                }
                for (int r = 0; r < eng.K_; ++r)
                    v[static_cast<size_t>(r)] += rhs[static_cast<size_t>(r)];
                continue;
            }
            double step = 1.0;
            std::vector<double> vt(static_cast<size_t>(eng.K_));
            for (int h = 0; h < 32; ++h) {
                for (int r = 0; r < eng.K_; ++r)
                    vt[static_cast<size_t>(r)] = v[static_cast<size_t>(r)] + step * rhs[static_cast<size_t>(r)];
                eng.limitDiodePorts(vt.data(), v.data());
                eng.limitBjtJunctions(vt.data(), v.data());
                std::vector<double> iv, Jt;
                dcCurrents(vt, iv, Jt);
                const std::vector<double> ft = eng.residual(vt, vOc, Z, iv);
                double m = 0.0;
                for (double e : ft)
                    m = std::max(m, std::fabs(e));
                if (m <= base)
                    break;
                step *= 0.5;
            }
            v = vt;
        }
        dcCurrents(v, i, Jm);
        node.assign(static_cast<size_t>(eng.N_), 0.0);
        for (int n = 0; n < eng.N_; ++n) {
            double s = u[static_cast<size_t>(n)];
            for (int c : eng.rootLayout_.nodeCol)
                s -= Xr[n][c] * i[static_cast<size_t>(c)];
            node[static_cast<size_t>(n)] = s;
        }
        if (!converged) {
            dcCurrents(v, i, Jm);
            const std::vector<double> F = eng.residual(v, vOc, Z, i);
            double m = 0.0;
            int worst = 0;
            for (int r = 0; r < eng.K_; ++r) {
                const double e = std::fabs(F[static_cast<size_t>(r)]);
                if (e > m) {
                    m = e;
                    worst = r;
                }
            }
            if (std::getenv("LIVEWDF_DEBUG_DC")) {
                std::fprintf(stderr,
                             "solveDcAt scale %.6g gmin %.3g fallback residual %.17g at port %d"
                             " (v=%.17g vOc=%.17g i=%.17g)\n",
                             sourceScale, gminOverride, m, worst,
                             v[static_cast<size_t>(worst)],
                             vOc[static_cast<size_t>(worst)],
                             i[static_cast<size_t>(worst)]);
                for (const auto& dev : eng.rootLayout_.devices) {
                    if (worst >= dev.portBase && worst < dev.portBase + dev.numPorts)
                        std::fprintf(stderr,
                                     "  worst port: device kind %d comp %d base %d ports %d\n",
                                     static_cast<int>(dev.kind), dev.compIndex, dev.portBase,
                                     dev.numPorts);
                }
            }
            converged = m < 1e-10;
        }
        if (std::getenv("LIVEWDF_DEBUG_DC"))
            std::fprintf(stderr, "solveDcAt scale %.6g gmin %.3g tol %.1g iters %d conv %d\n",
                         sourceScale, gminOverride, tol, lastIt + 1, converged ? 1 : 0);
        rootV = v;
        return converged;
    }

    template <typename EngineType>
    static bool solveDc(const EngineType& eng, std::vector<double>& node,
                                    std::vector<double>& rootV) {
        if (solveDcAt(eng, node, rootV, 1.0, 0.0, 1e-14, nullptr))
            return true;

        std::vector<double> nodeTry(static_cast<size_t>(eng.N_));
        std::vector<double> rootTry(static_cast<size_t>(eng.K_));
        const double tol = 1e-10;

        for (double gmin : {1e-12, 1e-11, 1e-10, 1e-9, 1e-8}) {
            if (solveDcAt(eng, nodeTry, rootTry, 1.0, gmin, tol, nullptr)) {
                node = nodeTry;
                rootV = rootTry;
                return true;
            }
        }

        const double gmin = 1e-10;
        if (!solveDcAt(eng, nodeTry, rootTry, 0.0, gmin, tol, nullptr))
            return false;

        std::vector<double> warm = rootTry;
        double reached = 0.0;
        double rung = 0.125;
        for (int guard = 0; guard < 64 && reached < 1.0; ++guard) {
            const double next = std::min(1.0, reached + rung);
            if (solveDcAt(eng, nodeTry, rootTry, next, gmin, tol, &warm)) {
                reached = next;
                warm = rootTry;
                if (reached == 1.0) {
                    node = nodeTry;
                    rootV = rootTry;
                    return true;
                }
                if (solveDcAt(eng, nodeTry, rootTry, 1.0, gmin, tol, &warm)) {
                    node = nodeTry;
                    rootV = rootTry;
                    return true;
                }
                rung = std::min(0.25, rung * 1.5);
            } else {
                rung *= 0.5;
                if (rung < 1.0 / 1024.0)
                    return false;
            }
        }
        return false;
    }

    template <typename EngineType>
    static bool solveNetworkBias(const EngineType& eng, std::vector<double>& nodeBias) {
        nodeBias.assign(static_cast<size_t>(eng.N_), 0.0);
        if (eng.N_ <= 0)
            return false;

        for (int iter = 0; iter < 50; ++iter) {
            double maxDiff = 0.0;
            for (int n = 0; n < eng.N_; ++n) {
                double sumG = 0.0;
                double sumGV = 0.0;
                for (int j = 0; j < eng.P_; ++j) {
                    if (eng.net_.capOf[j] >= 0) continue;
                    if (eng.net_.indOf[j] >= 0) continue;
                    const int a = eng.net_.epA[j];
                    const int b = eng.net_.epB[j];
                    const double g = eng.net_.eG[j];
                    if (g <= 0.0) continue;
                    const double eoff = eng.net_.eoffAff[j].eval(eng.dcValues_);
                    if (a == n) {
                        const double vOther = (b >= 0) ? nodeBias[static_cast<size_t>(b)] : 0.0;
                        sumG += g;
                        sumGV += g * (vOther - eoff);
                    } else if (b == n) {
                        const double vOther = (a >= 0) ? nodeBias[static_cast<size_t>(a)] : 0.0;
                        sumG += g;
                        sumGV += g * (vOther + eoff);
                    }
                }
                if (sumG > 1e-15) {
                    const double vNew = sumGV / sumG;
                    maxDiff = std::max(maxDiff, std::abs(vNew - nodeBias[static_cast<size_t>(n)]));
                    nodeBias[static_cast<size_t>(n)] = vNew;
                }
            }
            if (maxDiff < 1e-9)
                break;
        }
        return true;
    }
};

}  // namespace wdf::engine
