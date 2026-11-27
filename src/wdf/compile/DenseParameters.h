#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "circuit/Network.h"
#include "spice/Circuit.h"
#include "spice/Compiled.h"

namespace wdf::engine {

struct ParametersContext {
    Circuit& circuit;
    Compiled& comp;
    Network& net;
    const std::vector<int>& potResA;
    const std::vector<int>& potResB;
    const std::vector<int>& varResResIndex;
};

class DenseParameters {
public:
    static void setResistorValue(ParametersContext& ctx, int idx, double ohms) noexcept {
        ctx.circuit.res[static_cast<size_t>(idx)].value = ohms;
        ctx.comp.res[static_cast<size_t>(idx)].second = ohms;
        ctx.net.eG[static_cast<size_t>(idx)] = 1.0 / ohms;
    }

    template <typename RebuildFn>
    static bool setResistance(ParametersContext& ctx, int idx, double ohms, RebuildFn&& rebuild) {
        if (idx < 0 || idx >= static_cast<int>(ctx.circuit.res.size()) || !(ohms > 0.0))
            return false;
        if (ctx.circuit.res[static_cast<size_t>(idx)].value == ohms)
            return true;
        setResistorValue(ctx, idx, ohms);
        return rebuild();
    }

    template <typename Container, typename RebuildFn>
    static bool setResistancesByIndex(ParametersContext& ctx, const Container& updates, RebuildFn&& rebuild) {
        bool any = false;
        for (const auto& [idx, ohms] : updates) {
            if (idx >= 0 && idx < static_cast<int>(ctx.circuit.res.size()) && (ohms > 0.0) &&
                ctx.circuit.res[static_cast<size_t>(idx)].value != ohms) {
                setResistorValue(ctx, idx, ohms);
                any = true;
            }
        }
        return any ? rebuild() : true;
    }

    template <typename RebuildFn>
    static bool setPotPosition(ParametersContext& ctx, int potIdx, double pos, RebuildFn&& rebuild) {
        if (potIdx < 0 || potIdx >= static_cast<int>(ctx.circuit.pots.size()))
            return false;
        auto& pot = ctx.circuit.pots[static_cast<size_t>(potIdx)];
        const double clamped = std::clamp(pos, 0.0, 1.0);
        if (clamped == pot.position)
            return true;
        pot.position = clamped;
        auto [r1, r2] = Pot::resistances(pot.totalResistance, pot.position, pot.taper, pot.taperMid);
        const int idx1 = ctx.potResA[static_cast<size_t>(potIdx)];
        const int idx2 = ctx.potResB[static_cast<size_t>(potIdx)];
        if (idx1 < 0 || idx2 < 0)
            return false;

        setResistorValue(ctx, idx1, r1);
        setResistorValue(ctx, idx2, r2);
        return rebuild();
    }

    template <typename RebuildFn>
    static bool setVarResPosition(ParametersContext& ctx, int vrIdx, double pos, RebuildFn&& rebuild) {
        if (vrIdx < 0 || vrIdx >= static_cast<int>(ctx.circuit.varRes.size()))
            return false;
        auto& vr = ctx.circuit.varRes[static_cast<size_t>(vrIdx)];
        const double clamped = std::clamp(pos, 0.0, 1.0);
        if (clamped == vr.position)
            return true;
        vr.position = clamped;
        const double ohms = VarRes::resistance(vr.maxResistance, vr.position, vr.taper, vr.taperMid);
        const int idx = ctx.varResResIndex[static_cast<size_t>(vrIdx)];
        if (idx < 0)
            return false;

        setResistorValue(ctx, idx, ohms);
        return rebuild();
    }

    template <typename RebuildFn>
    static bool setParametersByIndex(ParametersContext& ctx,
                                     const std::vector<std::pair<int, double>>& pots,
                                     const std::vector<std::pair<int, double>>& varRes,
                                     const std::vector<std::pair<int, double>>& resistors,
                                     RebuildFn&& rebuild) {
        for (const auto& [idx, pos] : pots) {
            if (idx < 0 || idx >= static_cast<int>(ctx.circuit.pots.size()) ||
                !std::isfinite(pos) || ctx.potResA[static_cast<size_t>(idx)] < 0 ||
                ctx.potResB[static_cast<size_t>(idx)] < 0)
                return false;
        }
        for (const auto& [idx, pos] : varRes) {
            if (idx < 0 || idx >= static_cast<int>(ctx.circuit.varRes.size()) ||
                !std::isfinite(pos) || ctx.varResResIndex[static_cast<size_t>(idx)] < 0)
                return false;
        }
        for (const auto& [idx, ohms] : resistors) {
            if (idx < 0 || idx >= static_cast<int>(ctx.circuit.res.size()) ||
                !std::isfinite(ohms) || !(ohms > 0.0))
                return false;
        }

        bool any = false;
        for (const auto& [idx, pos] : pots) {
            auto& pot = ctx.circuit.pots[static_cast<size_t>(idx)];
            const double clamped = std::clamp(pos, 0.0, 1.0);
            if (clamped == pot.position)
                continue;
            pot.position = clamped;
            auto [r1, r2] = Pot::resistances(pot.totalResistance, pot.position, pot.taper, pot.taperMid);
            const int idx1 = ctx.potResA[static_cast<size_t>(idx)];
            const int idx2 = ctx.potResB[static_cast<size_t>(idx)];
            setResistorValue(ctx, idx1, r1);
            setResistorValue(ctx, idx2, r2);
            any = true;
        }
        for (const auto& [idx, pos] : varRes) {
            auto& vr = ctx.circuit.varRes[static_cast<size_t>(idx)];
            const double clamped = std::clamp(pos, 0.0, 1.0);
            if (clamped == vr.position)
                continue;
            vr.position = clamped;
            const double ohms = VarRes::resistance(vr.maxResistance, vr.position, vr.taper, vr.taperMid);
            const int resIdx = ctx.varResResIndex[static_cast<size_t>(idx)];
            setResistorValue(ctx, resIdx, ohms);
            any = true;
        }
        for (const auto& [idx, ohms] : resistors) {
            if (ctx.circuit.res[static_cast<size_t>(idx)].value != ohms) {
                setResistorValue(ctx, idx, ohms);
                any = true;
            }
        }

        return any ? rebuild() : true;
    }
};

} // namespace wdf::engine
