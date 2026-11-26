#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "circuit/SignalFlow.h"
#include "devices/Diode.h"
#include "spice/Circuit.h"
#include "spice/Compiled.h"
#include "wdf/DenseTopology.h"

namespace wdf::solver {

struct RootGroupsContext {
    const std::vector<RootDevice>& rootDevices;
    const Circuit& circuit;
    const Compiled& comp;
    const std::vector<double>& ctrlResValues;
    const std::vector<double>& Z;
    int K;
    int nTri;
    int nD;
    int nCtrl;
    int nPent;
    int nJfet;
    int nMosfet;
    int nBjt;
    int nOpAmp;

    bool explicitRootsAllowed;
    bool runtimeBlockEnabled;
    bool allowNonScalarSignalFlow;
    bool& explicitRootsActive;
    bool& allClosedFormBlocks;
    bool& hasAntiParallelBlock;
    int& blockCount;
    int& scalarBlockCount;
    int& groupCount;
    int& runtimeBlockCount;
    bool& runtimeBlockUsed;

    std::vector<int>& groupOrder;
    std::vector<int>& grpIndeg;
    std::vector<int>& grpQueue;
    std::vector<DiodeModel>& grpDiode;
    std::vector<NewtonBlock>& newtonBlocks;
    std::vector<int>& blockOf;
    std::vector<uint8_t>& blockReach;
    std::vector<uint8_t>& blockAdj;
    std::vector<NewtonBlock>& runtimeBlocks;
    std::vector<int>& runtimeOrder;
    std::vector<RuntimePartition>& runtimePartitions;
    const RuntimePartition*& runtimePartition;
};

class RootGroups {
public:
    static bool allowsExplicitRoot(int groupCount, int nPent, int nBjt, int nOpAmp) noexcept {
        return groupCount > 0 && nPent == 0 && nBjt == 0 && nOpAmp == 0;
    }

    /// Re-derive the root partition and whether the closed form is active. Called
    /// from `EngineCompile::rebuildLinear` -- the single choke point where `Z_`
    /// (and so the coupling) can move -- and from `setExplicitRoots`, so a pot
    /// move cannot leave the solve against a stale partition.
    static void refreshRootGroups(RootGroupsContext& ctx) {
        const int nGroups = static_cast<int>(ctx.rootDevices.size());
        if (static_cast<int>(ctx.groupOrder.size()) != nGroups) {
            // Topology is fixed for the engine's life, so this allocates once.
            ctx.groupOrder.resize(static_cast<size_t>(nGroups), 0);
            ctx.grpIndeg.resize(static_cast<size_t>(nGroups), 0);
            ctx.grpQueue.resize(static_cast<size_t>(nGroups), 0);
        }
        if (static_cast<int>(ctx.grpDiode.size()) != ctx.nD)
            ctx.grpDiode.assign(static_cast<size_t>(ctx.nD), DiodeModel{});
        if (static_cast<int>(ctx.runtimeBlocks.size()) != nGroups) {
            // Topology is fixed for the engine's life. Reserve every vector the
            // runtime partition can need so the audio loop never allocates.
            ctx.runtimeBlocks.resize(static_cast<size_t>(nGroups));
            for (auto& b : ctx.runtimeBlocks) {
                b.devices.reserve(static_cast<size_t>(nGroups));
                b.ports.reserve(static_cast<size_t>(2 * nGroups));
                b.portDevice.reserve(static_cast<size_t>(2 * nGroups));
            }
        }
        for (auto& b : ctx.runtimeBlocks) {
            b.devices.clear();
            b.ports.clear();
            b.portDevice.clear();
            b.clearLinearReduction();
        }
        ctx.runtimeOrder.resize(static_cast<size_t>(nGroups));
        ctx.runtimeBlockCount = 0;
        ctx.runtimeBlockUsed = false;
        ctx.blockCount = 0;
        ctx.allClosedFormBlocks = false;
        ctx.scalarBlockCount = 0;
        ctx.hasAntiParallelBlock = false;
        ctx.groupCount = buildRootOrder(ctx) ? ctx.blockCount : 0;
        ctx.explicitRootsActive =
            ctx.explicitRootsAllowed && ctx.groupCount > 0 && ctx.allClosedFormBlocks &&
            allowsExplicitRoot(ctx.groupCount, ctx.nPent, ctx.nBjt, ctx.nOpAmp);
        if (ctx.runtimeBlockEnabled)
            prepareRuntimePartitions(ctx);
    }

    /// Build the runtime partition. This changes only which devices are grouped
    /// and in what order; device laws, `Z`, state, and the full-residual fallback
    /// are unchanged.
    static bool buildRuntimeBlocks(RootGroupsContext& ctx) {
        ctx.runtimeBlockCount = 0;
        if (ctx.groupCount <= 0)
            return false;

        const int nGroups = static_cast<int>(ctx.rootDevices.size());
        if (nGroups <= 0 ||
            static_cast<int>(ctx.runtimeBlocks.size()) != nGroups ||
            static_cast<int>(ctx.runtimeOrder.size()) != nGroups)
            return false;

        const auto liveCount = [&](int g) {
            return activePortCount(ctx.rootDevices[static_cast<size_t>(g)]);
        };
        const auto coupled = [&](int g, int h) {
            const RootDevice& dh = ctx.rootDevices[static_cast<size_t>(h)];
            const int hLive = liveCount(h);
            const RootDevice& dg = ctx.rootDevices[static_cast<size_t>(g)];
            const int row0 = dg.portBase;
            const int nPorts = dg.numPorts;
            for (int r = row0; r < row0 + nPorts; ++r)
                for (int k = 0; k < hLive; ++k)
                    if (std::abs(ctx.Z[static_cast<size_t>(r) * ctx.K + activePort(dh, k)]) > 0.0)
                        return true;
            return false;
        };

        const size_t n2 = static_cast<size_t>(nGroups) * nGroups;
        if (ctx.blockReach.size() < n2) {
            ctx.blockReach.resize(n2);
            ctx.blockAdj.resize(n2);
        }
        std::fill(ctx.blockReach.begin(), ctx.blockReach.begin() + n2, false);
        std::fill(ctx.blockAdj.begin(), ctx.blockAdj.begin() + n2, false);
        for (int g = 0; g < nGroups; ++g) {
            ctx.blockReach[static_cast<size_t>(g) * nGroups + g] = 1;
            for (int h = 0; h < nGroups; ++h)
                if (h != g && coupled(g, h))
                    ctx.blockReach[static_cast<size_t>(g) * nGroups + h] = 1;
        }
        for (int k = 0; k < nGroups; ++k)
            for (int g = 0; g < nGroups; ++g)
                if (ctx.blockReach[static_cast<size_t>(g) * nGroups + k])
                    for (int h = 0; h < nGroups; ++h)
                        ctx.blockReach[static_cast<size_t>(g) * nGroups + h] |=
                            ctx.blockReach[static_cast<size_t>(k) * nGroups + h];

        for (auto& b : ctx.runtimeBlocks) {
            b.devices.clear();
            b.ports.clear();
            b.portDevice.clear();
            b.clearLinearReduction();
        }
        if (static_cast<int>(ctx.blockOf.size()) != nGroups)
            ctx.blockOf.assign(static_cast<size_t>(nGroups), -1);
        std::fill(ctx.blockOf.begin(), ctx.blockOf.end(), -1);

        int nb = 0;
        for (int g = 0; g < nGroups; ++g) {
            if (ctx.blockOf[static_cast<size_t>(g)] >= 0)
                continue;
            NewtonBlock& block = ctx.runtimeBlocks[static_cast<size_t>(nb)];
            for (int h = 0; h < nGroups; ++h) {
                if (ctx.blockOf[static_cast<size_t>(h)] < 0 &&
                    ctx.blockReach[static_cast<size_t>(g) * nGroups + h] &&
                    ctx.blockReach[static_cast<size_t>(h) * nGroups + g]) {
                    ctx.blockOf[static_cast<size_t>(h)] = nb;
                    block.devices.push_back(h);
                }
            }
            ++nb;
        }

        for (int a = 0; a < nb; ++a) {
            for (int di : ctx.runtimeBlocks[static_cast<size_t>(a)].devices) {
                for (int b = 0; b < nb; ++b) {
                    if (a == b)
                        continue;
                    bool dep = false;
                    for (int h : ctx.runtimeBlocks[static_cast<size_t>(b)].devices) {
                        if (coupled(di, h)) {
                            dep = true;
                            break;
                        }
                    }
                    if (dep)
                        ctx.blockAdj[static_cast<size_t>(a) * nGroups + b] = 1;
                }
            }
        }

        if (static_cast<int>(ctx.grpIndeg.size()) != nGroups) {
            ctx.grpIndeg.resize(static_cast<size_t>(nGroups), 0);
            ctx.grpQueue.resize(static_cast<size_t>(nGroups), 0);
        }
        int* indeg = ctx.grpIndeg.data();
        int* queue = ctx.grpQueue.data();
        for (int a = 0; a < nb; ++a) {
            int d = 0;
            for (int b = 0; b < nb; ++b)
                if (a != b && ctx.blockAdj[static_cast<size_t>(a) * nGroups + b])
                    ++d;
            indeg[a] = d;
        }
        int head = 0, tail = 0;
        for (int a = 0; a < nb; ++a)
            if (indeg[a] == 0)
                queue[tail++] = a;
        int count = 0;
        while (head < tail) {
            const int h = queue[head++];
            ctx.runtimeOrder[static_cast<size_t>(count++)] = h;
            for (int a = 0; a < nb; ++a)
                if (a != h && ctx.blockAdj[static_cast<size_t>(a) * nGroups + h] &&
                    --indeg[a] == 0)
                    queue[tail++] = a;
        }
        if (count != nb)
            return false;

        for (int b = 0; b < nb; ++b) {
            NewtonBlock& block = ctx.runtimeBlocks[static_cast<size_t>(b)];
            block.isAntiParallelDiodePair = false;
            for (int di : block.devices) {
                const RootDevice& dev = ctx.rootDevices[static_cast<size_t>(di)];
                if (dev.kind == DeviceKind::Bjt || dev.kind == DeviceKind::Pentode) {
                    ctx.runtimeBlockCount = 0;
                    return false;
                }
                for (int rr = 0; rr < activePortCount(dev); ++rr) {
                    block.ports.push_back(activePort(dev, rr));
                    block.portDevice.push_back(di);
                }
            }
            if (block.devices.size() == 2 && block.ports.size() == 2) {
                const RootDevice& d0 = ctx.rootDevices[static_cast<size_t>(block.devices[0])];
                const RootDevice& d1 = ctx.rootDevices[static_cast<size_t>(block.devices[1])];
                if (d0.kind == DeviceKind::Diode && d1.kind == DeviceKind::Diode &&
                    isAntiParallelDiodePair(ctx.comp, static_cast<int>(d0.compIndex),
                                            static_cast<int>(d1.compIndex))) {
                    block.isAntiParallelDiodePair = true;
                    block.diodePair = AntiParallelDiodeModel(
                        ctx.comp.diode[static_cast<size_t>(d0.compIndex)].second,
                        ctx.comp.diode[static_cast<size_t>(d1.compIndex)].second);
                }
            }
        }

        ctx.runtimeBlockCount = nb;
        compileBlockZAndPredFold(ctx.runtimeBlocks, ctx.runtimeOrder.data(), nb, ctx.rootDevices.data(), ctx.Z.data(), ctx.K);
        return true;
    }

    /// Reserve the per-partition storage once. `refreshRootGroups()` can run
    /// from the audio thread on a pot move, so re-preparing an already built
    /// table must reuse these vectors rather than destroy and reallocate them.
    static void ensureRuntimePartitionStorage(RuntimePartition& p, int nGroups) {
        if (static_cast<int>(p.blocks.size()) != nGroups) {
            p.blocks.clear();
            p.blocks.resize(static_cast<size_t>(nGroups));
            for (auto& b : p.blocks) {
                b.devices.reserve(static_cast<size_t>(nGroups));
                b.ports.reserve(static_cast<size_t>(2 * nGroups));
                b.portDevice.reserve(static_cast<size_t>(2 * nGroups));
            }
        }
        if (p.order.capacity() < static_cast<size_t>(nGroups))
            p.order.reserve(static_cast<size_t>(nGroups));
    }

    /// Precompute the runtime partition.
    static void prepareRuntimePartitions(RootGroupsContext& ctx) {
        ctx.runtimePartition = nullptr;
        ctx.runtimeBlockCount = 0;
        if (ctx.groupCount <= 0)
            return;

        const int nGroups = static_cast<int>(ctx.rootDevices.size());
        if (!buildRuntimeBlocks(ctx)) {
            ctx.runtimeBlockCount = 0;
            ctx.runtimePartition = nullptr;
            ctx.runtimePartitions.clear();
            return;
        }

        if (ctx.runtimePartitions.size() != 1)
            ctx.runtimePartitions.resize(1);
        RuntimePartition& p = ctx.runtimePartitions[0];
        ensureRuntimePartitionStorage(p, nGroups);
        for (int b = 0; b < ctx.runtimeBlockCount; ++b) {
            const NewtonBlock& src = ctx.runtimeBlocks[static_cast<size_t>(b)];
            NewtonBlock& dst = p.blocks[static_cast<size_t>(b)];
            dst.devices.assign(src.devices.begin(), src.devices.end());
            dst.ports.assign(src.ports.begin(), src.ports.end());
            dst.portDevice.assign(src.portDevice.begin(), src.portDevice.end());
            dst.isAntiParallelDiodePair = src.isAntiParallelDiodePair;
            dst.diodePair = src.diodePair;
            dst.blockZ.assign(src.blockZ.begin(), src.blockZ.end());
            dst.predFold.assign(src.predFold.begin(), src.predFold.end());
            dst.allFold.assign(src.allFold.begin(), src.allFold.end());
        }
        p.order.assign(ctx.runtimeOrder.begin(),
                       ctx.runtimeOrder.begin() + ctx.runtimeBlockCount);
        p.count = ctx.runtimeBlockCount;

        ctx.runtimeBlockCount = 0;
        ctx.runtimePartition = nullptr;
    }

    /// Pick the precomputed runtime partition.
    static bool selectRuntimeRootOrder(RootGroupsContext& ctx) {
        ctx.runtimeBlockUsed = false;
        ctx.runtimeBlockCount = 0;
        if (!ctx.runtimeBlockEnabled || ctx.groupCount <= 0)
            return false;

        if (!ctx.runtimePartitions.empty()) {
            const RuntimePartition& p = ctx.runtimePartitions[0];
            ctx.runtimePartition = &p;
            ctx.runtimeBlockCount = p.count;
            return ctx.runtimeBlockCount > 0;
        }

        ctx.runtimePartition = nullptr;
        return buildRuntimeBlocks(ctx);
    }

    /// A triode's grid current is predicted (`predictLaterGrids`) rather than
    /// ordered, so it does not merge a stage with the one driving it.
    static int partitionPortCount(const RootDevice& dev) noexcept {
        return dev.kind == DeviceKind::Triode ? 1 : activePortCount(dev);
    }

    /// Build the Newton blocks and their dependency order.
    static bool buildRootOrder(RootGroupsContext& ctx, bool allowSignalFlowPruning = true,
                               const std::vector<double>* slope = nullptr, std::vector<uint8_t>* lag = nullptr) {
        ctx.hasAntiParallelBlock = false;
        ctx.blockCount = 0;
        ctx.scalarBlockCount = 0;
        ctx.allClosedFormBlocks = false;
        // An OTA and an independently controlled dbx 200 half-cell share the
        // resistor port topology but have nonlinear signal current laws.
        if (!ctx.comp.otaSensePorts.empty() ||
            std::any_of(ctx.comp.dbx200Crossover.begin(), ctx.comp.dbx200Crossover.end(),
                        [](double crossover) { return crossover != 0.0; }))
            return false;

        const int nGroups = static_cast<int>(ctx.rootDevices.size());
        if (nGroups <= 0)
            return false;

        const double* z = ctx.Z.data();
        for (int q = 0; q < ctx.nD; ++q) {
            const RootDevice& dev = ctx.rootDevices[static_cast<size_t>(ctx.nTri + q)];
            const DiodeModel& m = ctx.comp.diode[dev.compIndex].second;
            const int e = dev.portBase;
            const double rtot = z[static_cast<size_t>(e) * ctx.K + e] + m.rs;
            if (!(rtot > 0.0))
                return false;
            ctx.grpDiode[static_cast<size_t>(q)] = m.withSeriesResistance(rtot);
        }
        for (int c = 0; c < ctx.nCtrl; ++c) {
            const RootDevice& dev = ctx.rootDevices[static_cast<size_t>(ctx.nTri + ctx.nD + c)];
            const int e = dev.portBase;
            const double rtot = z[static_cast<size_t>(e) * ctx.K + e] + ctx.ctrlResValues[dev.compIndex];
            if (!(rtot > 0.0))
                return false;
        }
        for (int j = 0; j < ctx.nJfet; ++j) {
            const RootDevice& dev = ctx.rootDevices[static_cast<size_t>(ctx.nTri + ctx.nD + ctx.nCtrl + ctx.nPent + j)];
            const int o = dev.portBase;
            const double zdd = z[static_cast<size_t>(o) * ctx.K + o];
            if (!(zdd > 0.0))
                return false;
            if (!ctx.comp.jfet[dev.compIndex].model.valid())
                return false;
        }
        for (int m = 0; m < ctx.nMosfet; ++m) {
            const RootDevice& dev = ctx.rootDevices[static_cast<size_t>(ctx.nTri + ctx.nD + ctx.nCtrl + ctx.nPent + ctx.nJfet + m)];
            const int o = dev.portBase;
            const double zdd = z[static_cast<size_t>(o) * ctx.K + o];
            if (!(zdd > 0.0))
                return false;
            if (!ctx.comp.mosfet[dev.compIndex].model.valid())
                return false;
        }

        // Directed signal flow analysis prunes false cycles across power rails (e.g. B+, VCC).
        FlowGraph fg = analyzeSignalFlow(ctx.circuit);
        const bool useSignalFlow =
            allowSignalFlowPruning && (fg.numElements == nGroups);

        std::vector<uint8_t> flowReach;
        if (useSignalFlow) {
            flowReach.assign(static_cast<size_t>(nGroups) * nGroups, 0);
            for (int u = 0; u < nGroups; ++u) {
                flowReach[static_cast<size_t>(u) * nGroups + u] = 1;
                for (int v : fg.flowAdj[static_cast<size_t>(u)])
                    flowReach[static_cast<size_t>(u) * nGroups + v] = 1;
            }
            for (int k = 0; k < nGroups; ++k)
                for (int i = 0; i < nGroups; ++i)
                    if (flowReach[static_cast<size_t>(i) * nGroups + k])
                        for (int j = 0; j < nGroups; ++j)
                            flowReach[static_cast<size_t>(i) * nGroups + j] |=
                                flowReach[static_cast<size_t>(k) * nGroups + j];
        }

        const auto zCoupled = [&](int g, int h) -> bool {
            const RootDevice& dh = ctx.rootDevices[static_cast<size_t>(h)];
            const int hAct = partitionPortCount(dh);
            const RootDevice& dg = ctx.rootDevices[static_cast<size_t>(g)];
            const int row0 = dg.portBase;
            const int nPorts = dg.numPorts;
            for (int r = row0; r < row0 + nPorts; ++r) {
                for (int c = 0; c < hAct; ++c) {
                    const int hp = activePort(dh, c);
                    if (std::abs(z[static_cast<size_t>(r) * ctx.K + hp]) > 1e-9)
                        return true;
                }
            }
            return false;
        };

        // Gain from h's partition currents to g's: |dI_g/dV_g| |Z_gh|, row-summed.
        const auto gain = [&](int g, int h) {
            const RootDevice& dg = ctx.rootDevices[static_cast<size_t>(g)];
            const RootDevice& dh = ctx.rootDevices[static_cast<size_t>(h)];
            double m = 0.0;
            for (int r = dg.portBase; r < dg.portBase + dg.numPorts; ++r) {
                double row = 0.0;
                for (int j = 0; j < partitionPortCount(dh); ++j)
                    for (int k = dg.portBase; k < dg.portBase + dg.numPorts; ++k)
                        row += (*slope)[static_cast<size_t>(r) * ctx.K + k] *
                               std::abs(z[static_cast<size_t>(k) * ctx.K + activePort(dh, j)]);
                m = std::max(m, row);
            }
            return m;
        };
        // Preview reads the weaker direction of a weak loop (supply, screen and
        // bias nodes) one sample late. Delayed, a loop rings at Nyquist unless the
        // gain around it, bounded by Z and the steepest device slopes, is below 1;
        // the margin is far lower because the late coupling also carries signal.
        const auto lagged = [&](int g, int h) {
            if (!slope || (useSignalFlow && flowReach[static_cast<size_t>(h) * nGroups + g]))
                return false;
            const double back = gain(g, h), forward = gain(h, g);
            return (back <= forward || (useSignalFlow && flowReach[static_cast<size_t>(g) * nGroups + h])) &&
                   back * forward < 1e-3;
        };

        const auto coupled = [&](int g, int h) -> bool {
            if (lagged(g, h))
                return false;
            if (useSignalFlow) {
                if (flowReach[static_cast<size_t>(h) * nGroups + g])
                    return true;
                if (flowReach[static_cast<size_t>(g) * nGroups + h])
                    return false;
                return zCoupled(g, h);
            }
            return zCoupled(g, h);
        };

        if (lag) {
            lag->assign(static_cast<size_t>(ctx.K) * ctx.K, 0);
            for (int g = 0; g < nGroups; ++g)
                for (int h = 0; h < nGroups; ++h)
                    if (g != h && lagged(g, h))
                        for (int r = 0; r < ctx.rootDevices[static_cast<size_t>(g)].numPorts; ++r)
                            for (int c = 0; c < partitionPortCount(ctx.rootDevices[static_cast<size_t>(h)]); ++c)
                                (*lag)[static_cast<size_t>(ctx.rootDevices[static_cast<size_t>(g)].portBase + r) * ctx.K +
                                       activePort(ctx.rootDevices[static_cast<size_t>(h)], c)] = 1;
        }
        const size_t n2 = static_cast<size_t>(nGroups) * nGroups;
        if (ctx.blockReach.size() < n2) {
            ctx.blockReach.resize(n2);
            ctx.blockAdj.resize(n2);
        }
        std::fill(ctx.blockReach.begin(), ctx.blockReach.begin() + n2, false);
        std::fill(ctx.blockAdj.begin(), ctx.blockAdj.begin() + n2, false);
        for (int g = 0; g < nGroups; ++g) {
            ctx.blockReach[static_cast<size_t>(g) * nGroups + g] = 1;
            for (int h = 0; h < nGroups; ++h)
                if (h != g && coupled(g, h))
                    ctx.blockReach[static_cast<size_t>(g) * nGroups + h] = 1;
        }
        for (int k = 0; k < nGroups; ++k)
            for (int g = 0; g < nGroups; ++g)
                if (ctx.blockReach[static_cast<size_t>(g) * nGroups + k])
                    for (int h = 0; h < nGroups; ++h)
                        ctx.blockReach[static_cast<size_t>(g) * nGroups + h] |=
                            ctx.blockReach[static_cast<size_t>(k) * nGroups + h];

        if (static_cast<int>(ctx.newtonBlocks.size()) != nGroups) {
            ctx.newtonBlocks.resize(static_cast<size_t>(nGroups));
            for (auto& b : ctx.newtonBlocks) {
                b.devices.reserve(static_cast<size_t>(nGroups));
                b.ports.reserve(static_cast<size_t>(2 * nGroups));
                b.portDevice.reserve(static_cast<size_t>(2 * nGroups));
            }
        }
        for (auto& b : ctx.newtonBlocks) {
            b.devices.clear();
            b.ports.clear();
            b.portDevice.clear();
        }
        if (static_cast<int>(ctx.blockOf.size()) != nGroups)
            ctx.blockOf.assign(static_cast<size_t>(nGroups), -1);
        std::fill(ctx.blockOf.begin(), ctx.blockOf.end(), -1);

        int nb = 0;
        for (int g = 0; g < nGroups; ++g) {
            if (ctx.blockOf[static_cast<size_t>(g)] >= 0)
                continue;
            NewtonBlock& block = ctx.newtonBlocks[static_cast<size_t>(nb)];
            for (int h = 0; h < nGroups; ++h) {
                if (ctx.blockOf[static_cast<size_t>(h)] < 0 &&
                    ctx.blockReach[static_cast<size_t>(g) * nGroups + h] &&
                    ctx.blockReach[static_cast<size_t>(h) * nGroups + g]) {
                    ctx.blockOf[static_cast<size_t>(h)] = nb;
                    block.devices.push_back(h);
                }
            }
            ++nb;
        }

        for (int a = 0; a < nb; ++a) {
            for (int di : ctx.newtonBlocks[static_cast<size_t>(a)].devices) {
                for (int b = 0; b < nb; ++b) {
                    if (a == b)
                        continue;
                    bool dep = false;
                    for (int h : ctx.newtonBlocks[static_cast<size_t>(b)].devices) {
                        if (coupled(di, h)) {
                            dep = true;
                            break;
                        }
                    }
                    if (dep)
                        ctx.blockAdj[static_cast<size_t>(a) * nGroups + b] = 1;
                }
            }
        }
        if (static_cast<int>(ctx.grpIndeg.size()) != nGroups) {
            ctx.grpIndeg.resize(static_cast<size_t>(nGroups), 0);
            ctx.grpQueue.resize(static_cast<size_t>(nGroups), 0);
            ctx.groupOrder.resize(static_cast<size_t>(nGroups), 0);
        }
        int* indeg = ctx.grpIndeg.data();
        int* queue = ctx.grpQueue.data();
        int* order = ctx.groupOrder.data();
        for (int a = 0; a < nb; ++a) {
            int d = 0;
            for (int b = 0; b < nb; ++b)
                if (a != b && ctx.blockAdj[static_cast<size_t>(a) * nGroups + b])
                    ++d;
            indeg[a] = d;
        }
        int head = 0, tail = 0;
        for (int a = 0; a < nb; ++a)
            if (indeg[a] == 0)
                queue[tail++] = a;
        int count = 0;
        while (head < tail) {
            const int h = queue[head++];
            order[count++] = h;
            for (int a = 0; a < nb; ++a)
                if (a != h && ctx.blockAdj[static_cast<size_t>(a) * nGroups + h] &&
                    --indeg[a] == 0)
                    queue[tail++] = a;
        }
        if (count != nb)
            return false;

        for (int b = 0; b < nb; ++b) {
            NewtonBlock& block = ctx.newtonBlocks[static_cast<size_t>(b)];
            for (int di : block.devices) {
                const RootDevice& dev = ctx.rootDevices[static_cast<size_t>(di)];
                for (int rr = 0; rr < activePortCount(dev); ++rr) {
                    block.ports.push_back(activePort(dev, rr));
                    block.portDevice.push_back(di);
                }
            }
        }

        for (int b = 0; b < nb; ++b) {
            NewtonBlock& block = ctx.newtonBlocks[static_cast<size_t>(b)];
            block.isAntiParallelDiodePair = false;
            if (block.devices.size() == 2 && block.ports.size() == 2) {
                const RootDevice& d0 = ctx.rootDevices[static_cast<size_t>(block.devices[0])];
                const RootDevice& d1 = ctx.rootDevices[static_cast<size_t>(block.devices[1])];
                if (d0.kind == DeviceKind::Diode && d1.kind == DeviceKind::Diode &&
                    isAntiParallelDiodePair(ctx.comp, static_cast<int>(d0.compIndex),
                                            static_cast<int>(d1.compIndex))) {
                    block.isAntiParallelDiodePair = true;
                    block.diodePair = AntiParallelDiodeModel(
                        ctx.comp.diode[static_cast<size_t>(d0.compIndex)].second,
                        ctx.comp.diode[static_cast<size_t>(d1.compIndex)].second);
                    ctx.hasAntiParallelBlock = true;
                }
            }
        }

        int scalarBlocks = 0;
        int closedBlocks = 0;
        for (int b = 0; b < nb; ++b) {
            const NewtonBlock& block = ctx.newtonBlocks[static_cast<size_t>(b)];
            const bool scalar = isScalarBlock(block, ctx.rootDevices.data());
            if (scalar)
                ++scalarBlocks;
            if (scalar || block.isAntiParallelDiodePair)
                ++closedBlocks;
        }
        ctx.allClosedFormBlocks = (closedBlocks == nb);
        ctx.scalarBlockCount = scalarBlocks;
        ctx.blockCount = nb;

        // A directed signal-flow order may drop a coupling edge that is
        // physically present in Z: the flow graph claims the cycle cannot
        // carry current across power rails (e.g. B+ -> VCC). Folding a scalar
        // singleton under a cut edge is harmless (its law sees only its own
        // port voltage; the full-residual check still guards the pass), but a
        // diode pair block couples both of its terminals through Z, so
        // solving it while an incoming coupling was cut solves a different
        // system. A pair with op-amps feeding back into its node is exactly
        // that shape (the Klon Centaur fixture pins it): the pruned order
        // solves the pair before the op-amps and misses the injected current.
        // When that happens, rebuild without pruning so the devices merge into
        // one block whose (reduced) solve sees the full coupling.
        bool cutCouplingIntoPair = false;
        if (useSignalFlow && ctx.hasAntiParallelBlock) {
            std::vector<int> devBlock(static_cast<size_t>(nGroups), -1);
            for (int b = 0; b < nb; ++b)
                for (int di : ctx.newtonBlocks[static_cast<size_t>(b)].devices)
                    devBlock[static_cast<size_t>(di)] = b;
            for (int b = 0; b < nb && !cutCouplingIntoPair; ++b) {
                const NewtonBlock& blk = ctx.newtonBlocks[static_cast<size_t>(b)];
                if (!blk.isAntiParallelDiodePair)
                    continue;
                for (int di : blk.devices) {
                    for (int h = 0; h < nGroups && !cutCouplingIntoPair; ++h) {
                        if (devBlock[h] == b)
                            continue;
                        if (zCoupled(di, h) && !coupled(di, h))
                            cutCouplingIntoPair = true;
                    }
                    if (cutCouplingIntoPair)
                        break;
                }
            }
        }

        // A pruned back-coupling is dropped, so only a lagged one may keep the pruned order.
        bool everyPrunedCouplingLagged = slope != nullptr;
        for (int g = 0; g < nGroups && everyPrunedCouplingLagged; ++g)
            for (int h = 0; h < nGroups && everyPrunedCouplingLagged; ++h)
                everyPrunedCouplingLagged = g == h || !zCoupled(g, h) || coupled(g, h) || lagged(g, h);
        if (allowSignalFlowPruning &&
            (!ctx.allClosedFormBlocks || cutCouplingIntoPair) &&
            !ctx.allowNonScalarSignalFlow && !(useSignalFlow && everyPrunedCouplingLagged))
            return buildRootOrder(ctx, false, slope, lag);

        compileBlockZAndPredFold(ctx.newtonBlocks, ctx.groupOrder.data(), nb, ctx.rootDevices.data(), ctx.Z.data(), ctx.K,
                                 lag ? lag->data() : nullptr);
        return true;
    }

    /// Pre-gather contiguous block submatrix Z and predecessor non-zero port couplings.
    static inline void compileBlockZAndPredFold(
        std::vector<NewtonBlock>& blocks,
        const int* order,
        int nb,
        const RootDevice* rootDevices,
        const double* zData,
        int K,
        const uint8_t* lag = nullptr) noexcept
    {
        for (int b = 0; b < nb; ++b) {
            NewtonBlock& block = blocks[static_cast<size_t>(b)];
            int totalDevPorts = 0;
            for (int di : block.devices)
                totalDevPorts += rootDevices[di].numPorts;
            const int n = static_cast<int>(block.ports.size());
            block.blockZ.resize(static_cast<size_t>(totalDevPorts * n));
            int devPortOffset = 0;
            for (int di : block.devices) {
                const RootDevice& dev = rootDevices[di];
                for (int q = 0; q < dev.numPorts; ++q) {
                    const int globalPort = dev.portBase + q;
                    const double* zRow = zData + static_cast<size_t>(globalPort) * K;
                    double* destRow = block.blockZ.data() + static_cast<size_t>(devPortOffset + q) * n;
                    for (int j = 0; j < n; ++j) {
                        destRow[j] = zRow[block.ports[static_cast<size_t>(j)]];
                    }
                }
                devPortOffset += dev.numPorts;
            }
        }

        for (int s = 0; s < nb; ++s) {
            const int b = order[s];
            NewtonBlock& block = blocks[static_cast<size_t>(b)];
            int totalDevPorts = 0;
            for (int di : block.devices)
                totalDevPorts += rootDevices[di].numPorts;
            block.predFold.resize(static_cast<size_t>(totalDevPorts));
            block.allFold.resize(static_cast<size_t>(totalDevPorts));
            block.laterGrid.clear();
            block.lagged.clear();

            int devPortOffset = 0;
            for (int di : block.devices) {
                const RootDevice& dev = rootDevices[di];
                for (int q = 0; q < dev.numPorts; ++q) {
                    const int globalPort = dev.portBase + q;
                    const double* zRow = zData + static_cast<size_t>(globalPort) * K;
                    auto& deps = block.predFold[static_cast<size_t>(devPortOffset + q)];
                    deps.clear();
                    for (int p = 0; p < nb; ++p) {
                        if (p == s)
                            continue;
                        const NewtonBlock& otherBlock = blocks[static_cast<size_t>(order[p])];
                        for (size_t k = 0; k < otherBlock.ports.size(); ++k) {
                            const int c = otherBlock.ports[k];
                            const RootDevice& od = rootDevices[otherBlock.portDevice[k]];
                            // A later block's grid current is predicted, so its
                            // back-coupling lags instead of iterating.
                            const bool grid = od.kind == DeviceKind::Triode && c == od.portBase + 2;
                            if (p > s && !grid && lag && lag[static_cast<size_t>(globalPort) * K + c] && zRow[c] != 0.0)
                                block.lagged.push_back({globalPort, PredecessorPort{c, zRow[c]}});
                            if (p > s && !grid)
                                continue;
                            const double z = zRow[c];
                            if (z == 0.0)
                                continue;
                            deps.push_back(PredecessorPort{c, z});
                            if (p > s && std::find(block.laterGrid.begin(), block.laterGrid.end(), c) ==
                                             block.laterGrid.end())
                                block.laterGrid.push_back(c);
                        }
                    }

                    auto& allDeps = block.allFold[static_cast<size_t>(devPortOffset + q)];
                    allDeps.clear();
                    for (int p = 0; p < nb; ++p) {
                        const int bp = order[p];
                        if (bp == b)
                            continue;
                        const NewtonBlock& otherBlock = blocks[static_cast<size_t>(bp)];
                        for (int c : otherBlock.ports) {
                            const double z = zRow[c];
                            if (z != 0.0)
                                allDeps.push_back(PredecessorPort{c, z});
                        }
                    }
                }
                devPortOffset += dev.numPorts;
            }
        }
    }
};

} // namespace wdf::solver
