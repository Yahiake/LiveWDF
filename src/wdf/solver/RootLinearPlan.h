#pragma once

// General linear/nonlinear block reduction.
//
// A Newton block is a strongly-connected group of root devices.  Some of those
// devices are affine in their port voltages: an op-amp in its linear region is
// a VCVS behind its output resistance, and a controlled resistor is just a
// conductance.  Their branch currents are not independent unknowns; they are
// determined by the network voltages.  Eliminating them leaves a smaller
// nonlinear root over the remaining devices.
//
// The algebra is the usual Schur complement.  Write the block currents as
// `i = [i_L; i_N]` (linear, nonlinear) and the port voltages as
// `v = vOc - Z i`.  An affine device d contributes
//
//     i_d = g_d . v_d + h_d
//
// so, after substituting the network relation,
//
//     (I + G Z)_LL i_L + (G Z)_LN i_N = b_L.
//
// Solving the linear block gives
//
//     i_L = K0 + K i_N,
//     K  = -(I + G Z)_LL^-1 (G Z)_LN,
//
// and the nonlinear ports see the reduced Thevenin network
//
//     v_N = vOc'_N - Z' i_N,
//     vOc'_N = vOc_N - Z_NL K0,
//     Z' = Z_NN + Z_NL K.
//
// `K0` changes with the op-amp state and the open-circuit voltages, so it is
// formed per solve.  `A = (I + G Z)_LL`, `K`, and `Z'` depend only on the
// topology and the device model, so they are prepared with the partition.
//
// This is deliberately device-agnostic: any `DeviceKind` whose law is affine
// can be listed in `linearDevices`, and every other kind stays in the reduced
// nonlinear core.  Today the linear set is op-amps and controlled resistors.
// The reduced core is solved by a scalar closed form, the anti-parallel diode
// pair, or a small Newton loop.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "devices/Diode.h"
#include "devices/OpAmp.h"
#include "spice/Compiled.h"
#include "wdf/DenseSolve.h"
#include "wdf/DenseTopology.h"

namespace wdf::solver {

class RootLinearPlan {
public:
    /// Build the reduction for one block.  Returns false when the block has no
    /// affine devices, or when an affine device is outside the model the
    /// elimination assumes (soft rails, invalid gain/resistance, or too many
    /// unknowns for the fixed-size fast path).
    static bool build(
        NewtonBlock& block,
        const RootDevice* rootDevices,
        const double* zData,
        int K,
        const Compiled& comp,
        const std::vector<OpAmpModel>& opAmps,
        const std::vector<double>& ctrlResValues,
        double sampleRate)
    {
        block.clearLinearReduction();
        if (!rootDevices || !zData || K <= 0 || block.devices.empty())
            return false;

        // Classify.  The order of `linearDevices` and `reducedDevices` follows
        // `block.devices`, which is the order `buildRootOrder` used for the
        // active ports.
        for (int di : block.devices) {
            const RootDevice& dev = rootDevices[di];
            if (dev.kind == DeviceKind::OpAmp || dev.kind == DeviceKind::ControlledRes)
                block.linearDevices.push_back(di);
            else
                block.reducedDevices.push_back(di);
        }
        if (block.linearDevices.empty())
            return false;

        const int mL = static_cast<int>(block.linearDevices.size());
        if (mL > 32)
            return false;

        // Active ports of the linear devices.  These are exactly the ports
        // `buildRootOrder` would have put in the block for the same devices.
        block.linearG.assign(static_cast<size_t>(mL), 0.0);
        for (int d = 0; d < mL; ++d) {
            const RootDevice& dev = rootDevices[block.linearDevices[static_cast<size_t>(d)]];
            if (dev.numPorts <= 0 || dev.numActiveRows <= 0)
                return false;
            const int active = activePort(dev, 0);
            if (active < 0 || active >= K)
                return false;
            block.linearPorts.push_back(active);

            if (dev.kind == DeviceKind::OpAmp) {
                if (dev.compIndex >= opAmps.size())
                    return false;
                const OpAmpModel& oa = opAmps[dev.compIndex];
                if (oa.softRails)
                    return false;
                if (!(oa.openLoopGain > 0.0) || !(oa.outputImpedance > 0.0))
                    return false;
                const double a = std::clamp(OpAmpEngineLaw::alpha(oa, sampleRate), 0.0, 1.0);
                if (!(a > 0.0))
                    return false;
                block.linearG[static_cast<size_t>(d)] =
                    a * oa.openLoopGain / (1.0 + a);
            } else if (dev.kind == DeviceKind::ControlledRes) {
                if (dev.compIndex >= ctrlResValues.size())
                    return false;
                const double r = ctrlResValues[dev.compIndex];
                if (!(r > 1e-12) || !std::isfinite(r))
                    return false;
                block.linearG[static_cast<size_t>(d)] = 1.0 / r;
            } else {
                return false;
            }
        }

        // Every port of every nonlinear device is a row of the reduced network;
        // the active ports among them are the reduced unknowns.
        for (int di : block.reducedDevices) {
            const RootDevice& dev = rootDevices[di];
            const int nAct = activePortCount(dev);
            if (nAct <= 0 || dev.numPorts <= 0)
                return false;
            for (int rr = 0; rr < nAct; ++rr)
                block.reducedPorts.push_back(activePort(dev, rr));
            for (int q = 0; q < dev.numPorts; ++q)
                block.reducedRowPorts.push_back(dev.portBase + q);
        }

        const int nN = static_cast<int>(block.reducedPorts.size());
        if (nN > 32)
            return false;

        block.reducedRowOfPort.assign(static_cast<size_t>(K), -1);
        for (int r = 0; r < static_cast<int>(block.reducedRowPorts.size()); ++r)
            block.reducedRowOfPort[static_cast<size_t>(block.reducedRowPorts[static_cast<size_t>(r)])] = r;

        // A = I + G Z on the linear active columns, and G Z on the nonlinear
        // columns.  `G` is one row per affine device.
        block.linearA.assign(static_cast<size_t>(mL) * static_cast<size_t>(mL), 0.0);
        block.linearALN.assign(static_cast<size_t>(mL) * static_cast<size_t>(nN), 0.0);

        const auto gz = [&](int d, int col) {
            const RootDevice& dev = rootDevices[block.linearDevices[static_cast<size_t>(d)]];
            const int o = dev.portBase;
            if (dev.kind == DeviceKind::OpAmp) {
                const double ro = opAmps[dev.compIndex].outputImpedance;
                const double g = block.linearG[static_cast<size_t>(d)];
                return (1.0 / ro) * zData[static_cast<size_t>(o) * K + col] -
                       (g / ro) * zData[static_cast<size_t>(o + 1) * K + col] +
                       (g / ro) * zData[static_cast<size_t>(o + 2) * K + col];
            }
            const double g = block.linearG[static_cast<size_t>(d)];
            return g * zData[static_cast<size_t>(o) * K + col];
        };

        for (int d = 0; d < mL; ++d) {
            for (int l = 0; l < mL; ++l) {
                double v = gz(d, block.linearPorts[static_cast<size_t>(l)]);
                if (block.linearPorts[static_cast<size_t>(l)] ==
                    block.linearPorts[static_cast<size_t>(d)])
                    v += 1.0;
                block.linearA[static_cast<size_t>(d) * mL + l] = v;
            }
            for (int j = 0; j < nN; ++j)
                block.linearALN[static_cast<size_t>(d) * nN + j] =
                    gz(d, block.reducedPorts[static_cast<size_t>(j)]);
        }

        // K = -A^-1 ALN.  `solveData` is in-place, so each column gets its own
        // copy of A.
        block.linearK.assign(static_cast<size_t>(mL) * static_cast<size_t>(nN), 0.0);
        std::vector<double> aCopy(block.linearA);
        std::vector<double> rhs(static_cast<size_t>(mL));
        for (int j = 0; j < nN; ++j) {
            std::copy(block.linearA.begin(), block.linearA.end(), aCopy.begin());
            for (int d = 0; d < mL; ++d)
                rhs[static_cast<size_t>(d)] = -block.linearALN[static_cast<size_t>(d) * nN + j];
            if (!detail::solveData(aCopy.data(), rhs.data(), mL))
                return false;
            for (int d = 0; d < mL; ++d)
                block.linearK[static_cast<size_t>(d) * nN + j] = rhs[static_cast<size_t>(d)];
        }

        // Reduced port impedance for every row of every nonlinear device.
        block.reducedZ.assign(block.reducedRowPorts.size() * static_cast<size_t>(nN), 0.0);
        for (int r = 0; r < static_cast<int>(block.reducedRowPorts.size()); ++r) {
            const int q = block.reducedRowPorts[static_cast<size_t>(r)];
            for (int j = 0; j < nN; ++j) {
                double v = zData[static_cast<size_t>(q) * K + block.reducedPorts[static_cast<size_t>(j)]];
                for (int l = 0; l < mL; ++l)
                    v += zData[static_cast<size_t>(q) * K + block.linearPorts[static_cast<size_t>(l)]] *
                         block.linearK[static_cast<size_t>(l) * nN + j];
                block.reducedZ[static_cast<size_t>(r) * nN + j] = v;
            }
        }

        // Closed-form opportunities in the reduced core.
        if (nN == 2 && block.reducedDevices.size() == 2) {
            const RootDevice& d0 = rootDevices[block.reducedDevices[0]];
            const RootDevice& d1 = rootDevices[block.reducedDevices[1]];
            if (d0.kind == DeviceKind::Diode && d1.kind == DeviceKind::Diode) {
                const int p0 = block.reducedPorts[0];
                const int p1 = block.reducedPorts[1];
                const int r0 = block.reducedRowOfPort[static_cast<size_t>(p0)];
                const int r1 = block.reducedRowOfPort[static_cast<size_t>(p1)];
                if (r0 >= 0 && r1 >= 0) {
                    const double z00 = block.reducedZ[static_cast<size_t>(r0) * nN + 0];
                    const double z01 = block.reducedZ[static_cast<size_t>(r0) * nN + 1];
                    const double z10 = block.reducedZ[static_cast<size_t>(r1) * nN + 0];
                    const double z11 = block.reducedZ[static_cast<size_t>(r1) * nN + 1];
                    if (z00 > 0.0 && z11 > 0.0 &&
                        std::abs(z01 + z00) < 1e-4 * z00 &&
                        std::abs(z10 + z11) < 1e-4 * z11) {
                        const auto& m0 = comp.diode[d0.compIndex].second;
                        const auto& m1 = comp.diode[d1.compIndex].second;
                        if (std::abs(m0.isat - m1.isat) <= 1e-6 * m0.isat &&
                            std::abs(m0.n - m1.n) <= 1e-4 &&
                            std::abs(m0.rs - m1.rs) <= 1e-3) {
                            block.reducedAntiParallel = true;
                            block.reducedPair = AntiParallelDiodeModel(m0, m1);
                        }
                    }
                }
            }
        } else if (nN == 1 && block.reducedDevices.size() == 1) {
            const RootDevice& dev = rootDevices[block.reducedDevices[0]];
            if (dev.kind == DeviceKind::Diode) {
                const int p = block.reducedPorts[0];
                const int r = block.reducedRowOfPort[static_cast<size_t>(p)];
                if (r >= 0) {
                    const double z = block.reducedZ[static_cast<size_t>(r)];
                    const auto& m = comp.diode[dev.compIndex].second;
                    if (z > 0.0 && std::isfinite(z)) {
                        block.reducedScalarDiode = true;
                        block.reducedScalarPort = p;
                        block.reducedScalarModel = DiodeModel(m.isat, m.n, z + m.rs);
                    }
                }
            }
        }

        block.linearReduction = true;
        return true;
    }
};

}  // namespace wdf::solver
