#include "wdf/LoadedDiodeString.h"
#include "circuit/RootLayout.h"

#include <algorithm>

namespace wdf {
double LoadedDiodeString::tabulate(const Compiled& circuit, double voltage, double& drop) const {
    double lo = std::min(0.0, voltage), hi = std::max(0.0, voltage);
    const auto& a = circuit.diode[first].second;
    const auto& b = circuit.diode[second].second;
    double ia, ib, unused;
    for (int n = 0; n < 46; ++n) {
        drop = .5 * (lo + hi);
        a.eval(drop, ia, unused);
        b.eval(voltage - drop, ib, unused);
        if (ia + firstShunt*drop < ib + secondShunt*(voltage - drop))
            lo = drop;
        else hi = drop;
    }
    drop = .5 * (lo + hi);
    a.eval(drop, ia, unused);
    return ia + firstShunt*drop;
}

bool findLoadedDiodeString(const Compiled& c, const RootLayout& layout,
                                  int d, const std::vector<bool>& used, LoadedDiodeString& result) {
    const auto& direct = c.diode[d].first;
    for (const auto& a : layout.devices) {
        if (a.kind != RootKind::Diode || a.compIndex == d || used[a.compIndex]) continue;
        const auto& ap = c.diode[a.compIndex].first;
        const int mid = ap.b;
        if (ap.a != direct.b || mid < 0 || mid == direct.a || mid == direct.b) continue;
        for (const auto& b : layout.devices) {
            if (b.kind != RootKind::Diode || b.compIndex == d || b.compIndex == a.compIndex || used[b.compIndex]) continue;
            const auto& bp = c.diode[b.compIndex].first;
            if (bp.a != mid || bp.b != direct.a) continue;
            bool isolated = true;
            for (size_t n = 0; n < direct.off.size(); ++n)
                isolated &= direct.off[n] + ap.off[n] + bp.off[n] == 0.0;
            auto touches = [mid](const Port& port) { return port.a == mid || port.b == mid; };
            for (int j = 0; j < layout.K; ++j)
                if (j != a.portBase && j != b.portBase) isolated &= !touches(layout.ports[j]);
            for (const auto& e : c.cap) isolated &= !touches(e.first);
            for (const auto& e : c.ind) isolated &= !touches(e.first);
            for (const auto& e : c.isrc) isolated &= !touches(e.first);
            for (const auto& e : c.vcvs) isolated &= !touches(e.inPort) && !touches(e.outPort);
            if (!isolated) continue;
            double ga = 0, gb = 0;
            auto parallel = [](const Port& p, const Port& q) {
                const bool same = p.a == q.a && p.b == q.b;
                if (!same && !isAntiParallel(p,q)) return false;
                for (size_t j = 0; j < p.off.size(); ++j)
                    if (p.off[j] != (same ? q.off[j] : -q.off[j])) return false;
                return true;
            };
            for (const auto& [port, resistance] : c.res) {
                if (!touches(port)) continue;
                if (!(resistance > 0.0)) { isolated = false; break; }
                if (parallel(port, ap)) ga += 1.0/resistance;
                else if (parallel(port, bp)) gb += 1.0/resistance;
                else { isolated = false; break; }
            }
            if (isolated && ((ga > 0.0) != (gb > 0.0))) {
                result = {a.compIndex, b.compIndex, a.portBase, b.portBase, ga, gb, {}};
                return true;
            }
        }
    }
    return false;
}
} // namespace wdf
