#pragma once

// The compiled WDF network: what a `Circuit` becomes once it has been laid out
// as ports.
//
// This exists to put a wall between *compiling* a netlist and *executing* it.
// The engine has to ask two different questions -- "what did the netlist become?"
// (ports, conductances, root ports, source offsets) and "what happens this
// sample?" (waves, solve, states) -- and when both answers lived in the engine's
// constructor they were impossible to tell apart: ~90 lines of layout sat inside
// the same function as the buffer allocation it has nothing to do with.
//
// So `buildNetwork()` owns the layout, and `Network` is the value it produces.
// `EngineCore` holds one as its structural state; a future tree/contraction
// backend (docs/PLAN.md step D) will compile *from* a `Network` rather than from
// engine internals, which is the main reason this type is worth having rather
// than leaving the vectors where they were.
//
// Nothing here depends on a component *value* moving: the ports and the node set
// come from topology alone, and a value reaches `eG` through the netlist. That is
// the property that makes a pot a parameter instead of a rebuild.

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "circuit/RootLayout.h"
#include "devices/CoupledInductors.h"
#include "spice/Net.h"

#if defined(_MSC_VER)
#define LIVEWDF_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define LIVEWDF_RESTRICT __restrict__
#else
#define LIVEWDF_RESTRICT
#endif

namespace wdf {

/// A port offset as an affine form: `c0 + sum(coef[k] * sourceValues[idx[k]])`.
/// Only the sources that actually reach the port are listed, which matters: in
/// the time-domain loop exactly one source varies, so a port's update is usually
/// one multiply-add.
struct AffineOffset {
    double c0 = 0.0;
    std::vector<std::pair<int, double>> coef;

    // Fast inline cache for eval(): avoids dynamic heap dereferences and vector loops in step()
    int8_t nCoef = 0;
    int8_t idx0 = -1;
    int8_t idx1 = -1;
    double c1 = 0.0;
    double c2 = 0.0;

    double eval(const std::vector<double>& sv) const noexcept {
        return eval(sv.data(), sv.size());
    }

    double eval(const double* LIVEWDF_RESTRICT sv, size_t svSize) const noexcept {
        double s = c0;
        if (nCoef == 1) {
            if (static_cast<size_t>(idx0) < svSize)
                s += c1 * sv[idx0];
            return s;
        }
        if (nCoef == 2) {
            if (static_cast<size_t>(idx0) < svSize)
                s += c1 * sv[idx0];
            if (static_cast<size_t>(idx1) < svSize)
                s += c2 * sv[idx1];
            return s;
        }
        if (nCoef > 2) {
            for (const auto& [i, c] : coef)
                if (static_cast<size_t>(i) < svSize)
                    s += c * sv[i];
        }
        return s;
    }
};

inline AffineOffset makeAffine(const std::vector<double>& off) {
    AffineOffset a;
    a.c0 = off.empty() ? 0.0 : off[0];
    for (size_t k = 1; k < off.size(); ++k)
        if (off[k] != 0.0)
            a.coef.emplace_back(static_cast<int>(k) - 1, off[k]);
    a.nCoef = static_cast<int8_t>(a.coef.size());
    if (a.nCoef >= 1) {
        a.idx0 = static_cast<int8_t>(a.coef[0].first);
        a.c1 = a.coef[0].second;
    }
    if (a.nCoef >= 2) {
        a.idx1 = static_cast<int8_t>(a.coef[1].first);
        a.c2 = a.coef[1].second;
    }
    return a;
}

/// The port layout of a compiled netlist. Index-aligned, so element port `j` is
/// `(epA[j], epB[j])` with conductance `eG[j]`.
struct Network {
    // Element ports: node a, node b, conductance, affine source offset, and the
    // index of the reactive state behind the port (-1 for a resistor). A
    // resistor is G = 1/R; a capacitor is G = 2C/T with its reflected wave kept
    // in the engine's `capState_`; an inductor is the wave-dual, G = T/(2L) with
    // reflected wave -`indState_`. Both reactive states are the wave variable at
    // the element's own port resistance (T/2C and 2L/T), so neither depends on
    // the rest of the network and both stay valid across a pot move.
    std::vector<int> epA, epB;
    std::vector<double> eG;
    std::vector<int> capOf, indOf;
    std::vector<AffineOffset> eoffAff;

    // Root ports: the triode ports (plate, external grid, internal grid per
    // tube), the ideal-current-source ports, and their concatenation -- the
    // column set the R-type transfer is built against.
    std::vector<std::array<int, 2>> rp;
    std::vector<std::array<int, 2>> isrcRoots;
    std::vector<std::array<int, 2>> allRoots;
    std::vector<AffineOffset> roffAff;

    // The `rootTransfer` view: ports plus conductances. `eG` is refreshed into
    // it on every parameter change, so it is part of the network rather than a
    // derived cache the engine has to keep straight.
    std::vector<ElemPort> elems;

    // Structure-only index lists, so `step()` iterates the ports that can move
    // instead of all P of them. A port's `b_j` exists only for a reactive state
    // and its `eoff_j` only where an ideal source reaches it, so a resistor
    // between two free nodes is an exact 0 on every sample, forever -- and `u =
    // Xe*w` then spends a multiply on that zero. Like `elems` these follow from
    // topology alone (a pot move never changes them), which is what makes them
    // part of the network rather than a cache the engine has to invalidate.
    //
    // Ascending, so iterating one still visits the ports in index order.
    std::vector<int> reactive;      // carries a state: `capOf`/`indOf` >= 0
    std::vector<int> coupled;       // `eoff` has a source term, so it re-evaluates
    std::vector<int> coupledRoots;  // ditto `roff`
    std::vector<int> liveCols;      // element ports whose `w` entry can be nonzero

    // Magnetically coupled inductors (SPICE `K` directive):
    struct CoupledGroupInfo {
        std::vector<int> indIndices;  // indices into `indOf`
        std::vector<std::vector<double>> R;
    };
    std::vector<MutualPortPair> mutualPairs;
    std::vector<CoupledGroupInfo> coupledGroups;
    std::vector<VCVSTransconductance> vcvsList;
};

/// Lay `circuit` out as WDF ports. Runs once per engine: everything it reads is
/// topology and `T`, never a value that can move.
///
/// An ideal current source becomes an *exact* unit-current injection column
/// rather than a stiff conductance: it adds no parasitic load and never enters
/// the Newton solve (its value is prescribed each sample). Its column comes from
/// the same transfer as the device roots, so folding it into the node voltages
/// is one multiply per source per node -- which is why its ports are carried
/// alongside the device roots in `allRoots`.
inline Network buildNetwork(const Circuit& circuit, const Compiled& comp, double T,
                            bool embedOpAmpOutputResistance = false) {
    Network n;

    // --- element ports ------------------------------------------------
    const int nRes = static_cast<int>(circuit.res.size());
    const int nCap = static_cast<int>(circuit.cap.size());
    const int nInd = static_cast<int>(circuit.ind.size());
    const int nVCVS = static_cast<int>(circuit.vcvs.size());
    int P = nRes + nCap + nInd + nVCVS;

    n.epA.resize(P);
    n.epB.resize(P);
    n.eG.resize(P);
    n.eoffAff.reserve(P);
    n.capOf.assign(P, -1);
    n.indOf.assign(P, -1);

    for (int j = 0; j < nRes; ++j) {
        const Port& p = comp.res[static_cast<size_t>(j)].first;
        n.epA[j] = p.a;
        n.epB[j] = p.b;
        n.eG[j] = 1.0 / comp.res[static_cast<size_t>(j)].second;
        n.eoffAff.push_back(makeAffine(p.off));
    }
    for (int k = 0; k < nCap; ++k) {
        const Port& p = comp.cap[static_cast<size_t>(k)].first;
        const int j = nRes + k;
        n.epA[j] = p.a;
        n.epB[j] = p.b;
        n.eG[j] = 2.0 * comp.cap[static_cast<size_t>(k)].second / T;
        n.capOf[j] = k;
        n.eoffAff.push_back(makeAffine(p.off));
    }
    for (int k = 0; k < nInd; ++k) {
        const Port& p = comp.ind[static_cast<size_t>(k)].first;
        const int j = nRes + nCap + k;
        n.epA[j] = p.a;
        n.epB[j] = p.b;
        n.eG[j] = T / (2.0 * comp.ind[static_cast<size_t>(k)].second);
        n.indOf[j] = k;
        n.eoffAff.push_back(makeAffine(p.off));
    }
    for (int v = 0; v < nVCVS; ++v) {
        const auto& cv = comp.vcvs[static_cast<size_t>(v)];
        const int j = nRes + nCap + nInd + v;
        n.epA[j] = cv.outPort.a;
        n.epB[j] = cv.outPort.b;
        n.eG[j] = 1.0 / cv.ro;
        std::vector<double> vcvsOff = cv.outPort.off;
        if (vcvsOff.size() < cv.inPort.off.size())
            vcvsOff.resize(cv.inPort.off.size(), 0.0);
        for (size_t k = 0; k < cv.inPort.off.size(); ++k)
            vcvsOff[k] -= cv.gain * cv.inPort.off[k];
        n.eoffAff.push_back(makeAffine(vcvsOff));
        n.vcvsList.push_back({cv.outPort.a, cv.outPort.b, cv.inPort.a, cv.inPort.b, cv.gain / cv.ro});
    }

    // --- coupled inductors / transformers (K) -------------------------
    for (const auto& cm : comp.mutual) {
        const size_t W = cm.indIndices.size();
        if (W < 2)
            continue;
        std::vector<std::vector<double>> G;
        std::vector<std::vector<double>> R;
        if (!cm.L.empty()) {
            CoupledInductorMatrices mats(cm.L, 1.0 / T);
            G = std::move(mats.G);
            R = std::move(mats.R);
        } else {
            std::vector<double> L(W);
            for (size_t w = 0; w < W; ++w)
                L[w] = comp.ind[static_cast<size_t>(cm.indIndices[w])].second;
            CoupledInductorMatrices mats(L, cm.k, 1.0 / T);
            G = std::move(mats.G);
            R = std::move(mats.R);
        }

        for (size_t w = 0; w < W; ++w) {
            const int indK = cm.indIndices[w];
            const int j = nRes + nCap + indK;
            n.eG[static_cast<size_t>(j)] = G[w][w];
        }
        for (size_t w1 = 0; w1 < W; ++w1) {
            for (size_t w2 = 0; w2 < W; ++w2) {
                if (w1 == w2)
                    continue;
                const int ind1 = cm.indIndices[w1];
                const int ind2 = cm.indIndices[w2];
                const int p1 = nRes + nCap + ind1;
                const int p2 = nRes + nCap + ind2;
                n.mutualPairs.push_back({p1, p2, G[w1][w2]});
            }
        }
        Network::CoupledGroupInfo grp;
        grp.indIndices = cm.indIndices;
        grp.R = std::move(R);
        n.coupledGroups.push_back(std::move(grp));
    }

    // --- root ports ---------------------------------------------------
    // The order and the per-device port lists come from `RootLayout`, the one
    // place that also describes which ports are active unknowns. This function
    // only turns them into the two node indices and affine offset the network
    // needs; it does not know what a triode or a pentode is.
    const RootLayout layout = buildRootLayout(comp);
    const int K = layout.K;
    n.rp.resize(static_cast<size_t>(K));
    n.roffAff.reserve(static_cast<size_t>(K));
    for (int r = 0; r < K; ++r) {
        const Port& p = layout.ports[static_cast<size_t>(r)];
        n.rp[static_cast<size_t>(r)] = {p.a, p.b};
        n.roffAff.push_back(makeAffine(p.off));
    }

    if (embedOpAmpOutputResistance) {
        for (const auto& device : layout.devices) {
            if (device.kind != RootKind::OpAmp)
                continue;
            const auto& model = comp.opamp[static_cast<size_t>(device.compIndex)].model;
            if (model.hasClassABOutput() || !(model.outputImpedance > 0.0) ||
                !std::isfinite(model.outputImpedance))
                continue;
            const size_t port = static_cast<size_t>(device.portBase);
            n.epA.push_back(n.rp[port][0]);
            n.epB.push_back(n.rp[port][1]);
            n.eG.push_back(1.0 / model.outputImpedance);
            n.eoffAff.push_back(n.roffAff[port]);
            n.capOf.push_back(-1);
            n.indOf.push_back(-1);
            ++P;
        }
    }

    // --- current-source root ports ------------------------------------
    const int nIsrc = static_cast<int>(comp.isrc.size());
    n.isrcRoots.resize(static_cast<size_t>(nIsrc));
    for (int k = 0; k < nIsrc; ++k) {
        const Port& p = comp.isrc[static_cast<size_t>(k)].first;
        n.isrcRoots[static_cast<size_t>(k)] = {p.a, p.b};
    }
    n.allRoots = n.rp;
    n.allRoots.insert(n.allRoots.end(), n.isrcRoots.begin(), n.isrcRoots.end());

    n.elems.resize(static_cast<size_t>(P));
    for (int j = 0; j < P; ++j)
        n.elems[static_cast<size_t>(j)] = {n.epA[j], n.epB[j], n.eG[j]};

    // --- structure lists (see the struct) ------------------------------
    for (int j = 0; j < P; ++j) {
        if (n.capOf[static_cast<size_t>(j)] >= 0 || n.indOf[static_cast<size_t>(j)] >= 0)
            n.reactive.push_back(j);
        if (!n.eoffAff[static_cast<size_t>(j)].coef.empty())
            n.coupled.push_back(j);
    }
    for (size_t r = 0; r < n.roffAff.size(); ++r)
        if (!n.roffAff[r].coef.empty())
            n.coupledRoots.push_back(static_cast<int>(r));

    // Which columns of `Xe*w` can ever be nonzero? `w[j] = b[j] - eoff[j]`, and
    // `step()` writes `b` only for a reactive port and `eoff` only for a port a
    // source reaches -- exactly the two lists above. So the complement is
    // identically zero and `EngineCompile::rebuildLinear` stores only these
    // columns, which is what lets the hot loop skip the rest.
    for (int j = 0; j < P; ++j) {
        const bool r = n.capOf[static_cast<size_t>(j)] >= 0 || n.indOf[static_cast<size_t>(j)] >= 0;
        const AffineOffset& o = n.eoffAff[static_cast<size_t>(j)];
        const bool s = !o.coef.empty() || o.c0 != 0.0;
        if (r || s)
            n.liveCols.push_back(j);
    }

    return n;
}

}  // namespace wdf
