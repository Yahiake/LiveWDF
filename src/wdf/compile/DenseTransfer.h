#pragma once

#include <vector>
#include "circuit/Network.h"
#include "spice/Net.h"
#include "spice/RootTransfer.h"
#include "wdf/DenseTopology.h"

namespace wdf::engine {

struct TransferContext {
    Network& net;
    int N;
    int P;
    int K;
    int nIsrc;
    int& nLive;
    std::vector<double>& Xe;
    std::vector<double>& Xr;
    std::vector<double>& Xis;
    std::vector<double>& Z;
    long long& linearRebuilds;

    Dense& linY;
    Dense& linRhs;
    Dense& linXe;
    Dense& linXr;

    std::vector<int>& xeRowOffsets;
    std::vector<int>& xeCols;
    std::vector<double>& xeVals;
    std::vector<int>& xrRowOffsets;
    std::vector<int>& xrCols;
    std::vector<double>& xrVals;
    const std::vector<int>& nodeCols;
    FactoredTransfer& factors;
};

class DenseTransfer {
public:
    static bool rebuildLinear(TransferContext& ctx) {
        ++ctx.linearRebuilds;
        for (int j = 0; j < ctx.P; ++j)
            ctx.net.elems[static_cast<size_t>(j)].g = ctx.net.eG[static_cast<size_t>(j)];

        // Small transfers are faster as precomputed row dot products. On large
        // banks retain the factors, then compare actual nonzero counts below.
        const bool large = static_cast<size_t>(ctx.N) * ctx.net.liveCols.size() >= 8192;
        const auto transfer = [&](FactoredTransfer* factors) {
            return rootTransferInto(ctx.net.elems, ctx.net.allRoots, ctx.N,
                                    ctx.linY, ctx.linRhs, ctx.linXe, ctx.linXr,
                                    ctx.net.mutualPairs, ctx.net.vcvsList, nullptr,
                                    factors, ctx.net.liveCols);
        };
        ctx.factors.active = large && transfer(&ctx.factors);
        if (!ctx.factors.active && !transfer(nullptr))
            return false;

        ctx.nLive = static_cast<int>(ctx.net.liveCols.size());
        const size_t liveStride = static_cast<size_t>(ctx.nLive);
        ctx.Xe.assign(static_cast<size_t>(ctx.N) * liveStride, 0.0);
        ctx.xeRowOffsets.assign(static_cast<size_t>(ctx.N + 1), 0);
        ctx.xeCols.clear();
        ctx.xeVals.clear();
        if (ctx.nLive > 0) {
            for (int n = 0; n < ctx.N; ++n) {
                ctx.xeRowOffsets[static_cast<size_t>(n)] = static_cast<int>(ctx.xeVals.size());
                const std::vector<double>& rowIn = ctx.linXe[static_cast<size_t>(n)];
                double* rowOut = &ctx.Xe[static_cast<size_t>(n) * liveStride];
                for (int t = 0; t < ctx.nLive; ++t) {
                    const double v = rowIn[static_cast<size_t>(ctx.net.liveCols[static_cast<size_t>(t)])];
                    rowOut[t] = v;
                    if (v != 0.0) {
                        ctx.xeCols.push_back(t);
                        ctx.xeVals.push_back(v);
                    }
                }
            }
        }
        ctx.xeRowOffsets[static_cast<size_t>(ctx.N)] = static_cast<int>(ctx.xeVals.size());

        // The triangular passes gather and scatter, so reserve them for a
        // substantial reduction over the compiled row multiply.
        ctx.factors.active = ctx.factors.active && ctx.xeVals.size() > 4 * ctx.factors.operations();

        ctx.Xr.resize(static_cast<size_t>(ctx.N) * ctx.K);
        ctx.xrRowOffsets.assign(static_cast<size_t>(ctx.N + 1), 0);
        ctx.xrCols.clear();
        ctx.xrVals.clear();
        for (int n = 0; n < ctx.N; ++n) {
            ctx.xrRowOffsets[static_cast<size_t>(n)] = static_cast<int>(ctx.xrVals.size());
            for (int c = 0; c < ctx.K; ++c) {
                ctx.Xr[static_cast<size_t>(n) * ctx.K + c] =
                    ctx.linXr[static_cast<size_t>(n)][static_cast<size_t>(c)];
            }
            for (int c : ctx.nodeCols) {
                const double v = ctx.linXr[static_cast<size_t>(n)][static_cast<size_t>(c)];
                if (v != 0.0) {
                    ctx.xrCols.push_back(c);
                    ctx.xrVals.push_back(v);
                }
            }
        }
        ctx.xrRowOffsets[static_cast<size_t>(ctx.N)] = static_cast<int>(ctx.xrVals.size());

        ctx.Xis.assign(static_cast<size_t>(ctx.N) * static_cast<size_t>(ctx.nIsrc), 0.0);
        for (int n = 0; n < ctx.N; ++n)
            for (int k = 0; k < ctx.nIsrc; ++k)
                ctx.Xis[static_cast<size_t>(n) * static_cast<size_t>(ctx.nIsrc) + static_cast<size_t>(k)] =
                    ctx.linXr[static_cast<size_t>(n)][static_cast<size_t>(ctx.K + k)];

        ctx.Z.assign(static_cast<size_t>(ctx.K) * ctx.K, 0.0);
        for (int r = 0; r < ctx.K; ++r)
            for (int c = 0; c < ctx.K; ++c)
                ctx.Z[static_cast<size_t>(r) * ctx.K + c] =
                    acrossMat(ctx.net.rp[static_cast<size_t>(r)], ctx.linXr, c);

        return true;
    }
};

} // namespace wdf::engine
