#pragma once

// Compiled view: ports, offsets, and the circuit-to-port compilation.

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "spice/Circuit.h"
#include "spice/Potentials.h"

namespace wdf {

// --------------------------------------------------------------------------- //
// Compiled view
// --------------------------------------------------------------------------- //

/// A two-terminal port: node indices 0..N-1 with -1 for ground, plus the affine
/// offset of the port voltage (`off[0]` constant, `off[s+1]` for `src[s]`).
struct Port {
    int a = -1;
    int b = -1;
    std::vector<double> off;
};

struct CompiledTube {
    Port pk, gk, gik;
    int ip = -1, ik = -1, ig = -1, igi = -1;  // node indices for stamping
    TriodeModel model;
};

struct Compiled {
    int N = 0;
    int nsrc = 0;
    std::vector<std::string> nodes;              // sorted free-node names
    std::unordered_map<std::string, int> index;  // free-node name -> column
    std::vector<std::pair<Port, double>> res;
    std::vector<std::pair<Port, double>> cap;
    std::vector<std::pair<Port, double>> ind;
    std::vector<std::pair<Port, Isrc>> isrc;
    struct CompiledBucketBrigade {
        Port input, clock;
        BucketBrigadeModel model;
        std::size_t outputCurrentIndex;
    };
    std::vector<CompiledBucketBrigade> bucketBrigades;
    std::vector<CompiledTube> tri;
    /// Diodes as compiled ports: each becomes a root port, after the tubes'.
    std::vector<std::pair<Port, DiodeModel>> diode;
    /// Controlled resistors as compiled root ports, after the diodes'.
    std::vector<std::pair<Port, double>> ctrlRes;
    /// High-impedance sense ports, indexed by the controlled resistance moved.
    std::vector<std::pair<size_t, Port>> photoSensePorts;
    std::vector<std::pair<size_t, Port>> vcaSensePorts;
    std::vector<std::pair<size_t, Port>> otaSensePorts;
    std::vector<bool> otaCurrentLaw;
    /// Voltage-controlled voltage sources (SPICE `E` element).
    struct CompiledVCVS {
        Port outPort;
        Port inPort;
        double gain = 1.0;
        double ro = 1e-4;
        std::string name;
    };
    std::vector<CompiledVCVS> vcvs;
    /// Power pentodes: root ports (p, k), (s, k), (g, k).
    struct CompiledPentode {
        Port pk, sk, gk;
        int ip = -1, is = -1, ig = -1, ik = -1;
        Pentode model;
        std::string name;
    };
    std::vector<CompiledPentode> pent;
    /// JFETs: two root ports, (drain, source) and (gate, source). The gate port
    /// carries no current; its voltage still matters to the law and to the
    /// network's coupling, so it is a real port rather than a folded column.
    struct CompiledJfet {
        Port ds, gs;
        int id = -1, ig = -1, is = -1;
        JfetModel model;
        std::string name;
    };
    std::vector<CompiledJfet> jfet;
    /// N-channel MOSFETs: the same two root ports as a JFET, (drain, source)
    /// and (gate, source). The gate carries no current.
    struct CompiledMosfet {
        Port ds, gs;
        int id = -1, ig = -1, is = -1;
        MosfetModel model;
        std::string name;
    };
    std::vector<CompiledMosfet> mosfet;
    /// BJT: three root ports (c,e), (b,e), (e,ground) for Ebers-Moll evaluation.
    struct CompiledBjt {
        Port ce, be, e0;
        int ic = -1, ib = -1, ie = -1;
        BjtModel model;
        std::string name;
    };
    std::vector<CompiledBjt> bjt;
    /// Op-Amp: three root ports, all referenced to ground:
    /// (out, 0), (pos, 0), (neg, 0). The output current is the VCVS output;
    /// the two input ports carry zero current. The real feedback network stays
    /// in the linear WDF tree, so the closed-loop gain is Rf/Ri rather than a
    /// precomputed scalar. The supply pins set the output rails only.
    struct CompiledOpAmp {
        Port out, pos, neg;
        OpAmpModel model;
        std::string name;
    };
    std::vector<CompiledOpAmp> opamp;
    /// Magnetically coupled inductors (SPICE `K` directive) and explicit
    /// transformer matrices. An empty `L` uses the equal-`k` K model; a real
    /// transformer carries its full inductance matrix here.
    struct CompiledMutual {
        std::string name;
        std::vector<int> indIndices;  // indices into `ind`
        double k = 0.0;
        std::vector<std::vector<double>> L;
    };
    std::vector<CompiledMutual> mutual;
    // node name -> (representative name or "" for ground, affine potential)
    std::unordered_map<std::string, std::pair<std::string, std::vector<double>>> info;

    /// Port voltage of `p` at node voltages `v` (ground and the affine source
    /// offsets included).
    double across(const Port& p, const std::vector<double>& v) const {
        return (p.a >= 0 ? v[p.a] : 0.0) - (p.b >= 0 ? v[p.b] : 0.0);
    }
};

/// True when two ports span the same two nodes with opposite polarity. Their
/// port voltages are exact negatives; in a reciprocal network the cross
/// impedance is the negated driving-point impedance, so the pair's two port
/// equations collapse into one scalar equation in the shared terminal voltage.
inline bool isAntiParallel(const Port& a, const Port& b) noexcept {
    return a.a == b.b && a.b == b.a;
}

/// The same statement for two compiled diode branches. The root partitioners
/// use it to choose the anti-parallel pair law instead of a coupled Newton
/// block; the branch models may differ.
inline bool isAntiParallelDiodePair(const Compiled& c, int i0, int i1) noexcept {
    return isAntiParallel(c.diode[static_cast<size_t>(i0)].first,
                          c.diode[static_cast<size_t>(i1)].first);
}

/// Offset of an element whose port is held by ideal sources, at source values `sv`.
inline double evalOff(const std::vector<double>& off, const double* sv) {
    double s = off.empty() ? 0.0 : off[0];
    for (size_t k = 1; k < off.size(); ++k)
        s += off[k] * sv[k - 1];
    return s;
}

inline Compiled compile(const Circuit& c, const std::vector<double>& /*sourceValues*/) {
    const int nsrc = static_cast<int>(c.src.size());
    Potentials pot(nsrc);

    for (const auto& s : c.src) {
        std::string r;
        std::vector<double> p;
        pot.find(s.a, r, p);
        pot.find(s.b, r, p);
    }
    for (int s = 0; s < nsrc; ++s)
        pot.unite(c.src[s].a, c.src[s].b, pot.unit(s));

    Compiled out;
    out.nsrc = nsrc;
    for (const auto& n : c.allNodes()) {
        std::string r;
        std::vector<double> p;
        pot.find(n, r, p);
        out.info[n] = {r == kGroundName ? std::string() : r, std::move(p)};
    }

    std::vector<std::string> reps;
    for (const auto& kv : out.info)
        if (!kv.second.first.empty())
            reps.push_back(kv.second.first);
    std::sort(reps.begin(), reps.end());
    reps.erase(std::unique(reps.begin(), reps.end()), reps.end());
    for (int i = 0; i < static_cast<int>(reps.size()); ++i)
        out.index[reps[i]] = i;
    out.nodes = reps;
    out.N = static_cast<int>(reps.size());

    auto slotIndex = [&](const std::string& n) -> int {
        const std::string& rep = out.info.at(n).first;
        return rep.empty() ? -1 : out.index.at(rep);
    };
    auto port = [&](const std::string& a, const std::string& b) -> Port {
        Port p;
        p.a = slotIndex(a);
        p.b = slotIndex(b);
        const std::vector<double>& oa = out.info.at(a).second;
        const std::vector<double>& ob = out.info.at(b).second;
        p.off.resize(nsrc + 1);
        for (int k = 0; k <= nsrc; ++k)
            p.off[k] = oa[k] - ob[k];
        return p;
    };

    for (const auto& e : c.res)
        out.res.emplace_back(port(e.a, e.b), e.value);
    for (const auto& e : c.cap)
        out.cap.emplace_back(port(e.a, e.b), e.value);
    for (const auto& e : c.ind)
        out.ind.emplace_back(port(e.a, e.b), e.value);
    for (const auto& e : c.isrc)
        out.isrc.emplace_back(port(e.a, e.b), e);
    for (const auto& bbd : c.bucketBrigades)
        out.bucketBrigades.push_back({port(bbd.input, bbd.common),
                                     port(bbd.control, bbd.common),
                                     bbd.model, bbd.outputCurrentIndex});
    for (const auto& t : c.tri) {
        CompiledTube ct;
        ct.pk = port(t.p, t.k);
        ct.gk = port(t.g, t.k);
        ct.gik = port(t.gi, t.k);
        ct.ip = slotIndex(t.p);
        ct.ik = slotIndex(t.k);
        ct.ig = slotIndex(t.g);
        ct.igi = slotIndex(t.gi);
        ct.model = t.model;
        out.tri.push_back(std::move(ct));
    }
    for (const auto& d : c.diode)
        out.diode.emplace_back(port(d.a, d.b), d.model);
    for (const auto& cr : c.ctrlRes) {
        if (cr.photo)
            out.photoSensePorts.emplace_back(out.ctrlRes.size(),
                                             port(cr.photo->sensePos, cr.photo->senseNeg));
        if (cr.vca)
            out.vcaSensePorts.emplace_back(out.ctrlRes.size(),
                                            port(cr.vca->control, cr.vca->reference));
        if (cr.ota)
            out.otaSensePorts.emplace_back(out.ctrlRes.size(),
                                            port(cr.ota->bias, cr.ota->biasDiode));
        out.otaCurrentLaw.push_back(cr.ota.has_value());
        out.ctrlRes.emplace_back(port(cr.a, cr.b), cr.initialOhms);
    }
    for (const auto& v : c.vcvs) {
        if (v.ro <= 0.0)
            throw std::runtime_error("VCVS output resistance Ro must be strictly positive: " + v.name);
        Compiled::CompiledVCVS cv;
        cv.outPort = port(v.outA, v.outB);
        cv.inPort = port(v.inA, v.inB);
        cv.gain = v.gain;
        cv.ro = v.ro;
        cv.name = v.name;
        out.vcvs.push_back(std::move(cv));
    }
    for (const auto& pt : c.pent) {
        Compiled::CompiledPentode cp;
        cp.pk = port(pt.p, pt.k);
        cp.sk = port(pt.s, pt.k);
        cp.gk = port(pt.g, pt.k);
        cp.ip = slotIndex(pt.p);
        cp.is = slotIndex(pt.s);
        cp.ig = slotIndex(pt.g);
        cp.ik = slotIndex(pt.k);
        cp.model = pt.model;
        cp.name = pt.name;
        out.pent.push_back(std::move(cp));
    }
    for (const auto& j : c.jfet) {
        Compiled::CompiledJfet cj;
        cj.ds = port(j.d, j.s);
        cj.gs = port(j.g, j.s);
        cj.id = slotIndex(j.d);
        cj.ig = slotIndex(j.g);
        cj.is = slotIndex(j.s);
        cj.model = j.model;
        cj.name = j.name;
        out.jfet.push_back(std::move(cj));
    }
    for (const auto& m : c.mosfet) {
        Compiled::CompiledMosfet cm;
        cm.ds = port(m.d, m.s);
        cm.gs = port(m.g, m.s);
        cm.id = slotIndex(m.d);
        cm.ig = slotIndex(m.g);
        cm.is = slotIndex(m.s);
        cm.model = m.model;
        cm.name = m.name;
        out.mosfet.push_back(std::move(cm));
    }

    auto equalCi = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    };
    for (const auto& b : c.bjt) {
        Compiled::CompiledBjt cb;
        cb.ce = port(b.c, b.e);
        cb.be = port(b.b, b.e);
        cb.e0 = port(b.e, "0");  // emitter to ground: independent third voltage variable
        cb.ic = slotIndex(b.c);
        cb.ib = slotIndex(b.b);
        cb.ie = slotIndex(b.e);
        cb.model = b.model;
        cb.name = b.name;
        out.bjt.push_back(std::move(cb));
    }
    for (const auto& o : c.opamp) {
        Compiled::CompiledOpAmp co;
        const std::string ref = "0";
        co.out = port(o.out, ref);
        co.pos = port(o.pos, ref);
        co.neg = port(o.neg, ref);
        co.model = o.model;
        co.name = o.name;
        out.opamp.push_back(std::move(co));
    }
    for (const auto& m : c.mutual) {
        Compiled::CompiledMutual cm;
        cm.name = m.name;
        cm.k = m.k;
        cm.L = m.L;
        for (const auto& iname : m.indNames) {
            int found = -1;
            for (size_t i = 0; i < c.ind.size(); ++i) {
                if (equalCi(c.ind[i].name, iname)) {
                    found = static_cast<int>(i);
                    break;
                }
            }
            if (found < 0)
                throw std::runtime_error("coupled inductor '" + iname + "' in '" + m.name + "' not found in circuit");
            cm.indIndices.push_back(found);
        }
        out.mutual.push_back(std::move(cm));
    }

    return out;
}

} // namespace wdf
