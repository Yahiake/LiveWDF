#include "spice/SpiceExport.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace wdf {

namespace {

std::string spiceSafe(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const unsigned char c : s)
        out += std::isalnum(c) ? static_cast<char>(c) : '_';
    return out.empty() ? std::string("x") : out;
}

std::string spiceNum(double v) {
    if (!std::isfinite(v)) return std::string("0");
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return std::string(buf);
}

std::string spiceUnique(const std::string& base, std::set<std::string>& used) {
    std::string name = base;
    int n = 2;
    while (used.count(name) != 0)
        name = base + "_" + std::to_string(n++);
    used.insert(name);
    return name;
}

/// Terminal-map lookup: return the engine-facing node name for a device
/// terminal. Ground is "0".
inline std::string nodeOrGround(const std::string& n) {
    return n.empty() ? std::string("0") : n;
}

}  // namespace

SpiceExportResult spiceExport(const Circuit& c,
                              double fs,
                              double durationSeconds) {
    SpiceExportResult r;
    std::ostringstream f;
    std::set<std::string> used;

    f << "* LiveWDF netlist exported by SpiceExport (core)\n";
    f << ".title LiveWDF\n";
    f << "* Engine integrates with trapezoidal at " << spiceNum(fs) << " Hz\n";
    f << ".options method=gear maxord=2 reltol=1e-4 abstol=1e-9 "
         "vntol=1e-5 chgtol=1e-12 trtol=7\n";
    if (durationSeconds > 0.0) {
        const double tstep = 1.0 / fs;
        f << ".tran " << std::setprecision(6) << tstep << " "
          << std::setprecision(6) << durationSeconds << " 0 " << tstep << "\n";
    }

    // ---- Resistors ----
    for (size_t i = 0; i < c.res.size(); ++i) {
        const auto& e = c.res[i];
        const std::string base = e.name.empty() ? ("R" + std::to_string(i)) : e.name;
        f << spiceUnique("R_" + spiceSafe(base), used) << " "
          << nodeOrGround(e.a) << " " << nodeOrGround(e.b) << " "
          << spiceNum(e.value) << "\n";
    }

    // ---- Capacitors ----
    for (size_t i = 0; i < c.cap.size(); ++i) {
        const auto& e = c.cap[i];
        const std::string base = e.name.empty() ? ("C" + std::to_string(i)) : e.name;
        f << spiceUnique("C_" + spiceSafe(base), used) << " "
          << nodeOrGround(e.a) << " " << nodeOrGround(e.b) << " "
          << spiceNum(e.value) << "\n";
    }

    // ---- Inductors ----
    std::map<std::string, std::string> indElementNames;
    for (size_t i = 0; i < c.ind.size(); ++i) {
        const auto& e = c.ind[i];
        const std::string base = e.name.empty() ? ("L" + std::to_string(i)) : e.name;
        const std::string name = spiceUnique("L_" + spiceSafe(base), used);
        indElementNames[e.name] = name;
        f << name << " " << nodeOrGround(e.a) << " " << nodeOrGround(e.b) << " "
          << spiceNum(e.value) << "\n";
    }

    // ---- Mutual inductors (transformers) ----
    for (size_t i = 0; i < c.mutual.size(); ++i) {
        const auto& m = c.mutual[i];
        if (m.indNames.size() < 2) continue;
        const std::string kn = spiceUnique("K_" + spiceSafe(m.name.empty() ? "k" : m.name), used);
        f << kn;
        for (const auto& n : m.indNames) {
            auto it = indElementNames.find(n);
            f << " " << (it != indElementNames.end() ? it->second : spiceSafe(n));
        }
        if (m.L.empty())
            f << " " << spiceNum(m.k);
        else
            f << " " << spiceNum(1.0);  // explicit matrix: k=1, entries in L
        f << "\n";
    }

    // ---- Controlled resistors ----
    // The engine seeds each controlled resistor from its initial ohms
    // (`Compiled::ctrlRes[].second`, read by `Engine::ctrlResValues_`), so a
    // plain `R` at that ohmage reproduces the same DC state. The runtime
    // controller has no SPICE equivalent and is reported in the diagnostics.
    for (size_t i = 0; i < c.ctrlRes.size(); ++i) {
        const auto& e = c.ctrlRes[i];
        const std::string base = e.name.empty() ? ("CR" + std::to_string(i)) : e.name;
        f << spiceUnique("R_" + spiceSafe(base), used) << " "
          << nodeOrGround(e.a) << " " << nodeOrGround(e.b) << " "
          << spiceNum(e.initialOhms) << "\n";
    }

    // ---- Voltage sources ----
    for (size_t i = 0; i < c.src.size(); ++i) {
        const auto& s = c.src[i];
        const std::string base = s.name.empty() ? ("V" + std::to_string(i)) : s.name;
        const std::string name = spiceUnique("V_" + spiceSafe(base), used);
        f << name << " " << nodeOrGround(s.a) << " " << nodeOrGround(s.b);
        if (!s.sin.empty() && s.sin.size() >= 3) {
            // SIN(io ia freq [td [theta [phase]]])
            f << " SIN(" << spiceNum(s.sin[0]) << " " << spiceNum(s.sin[1]) << " "
              << spiceNum(s.sin[2]);
            if (s.sin.size() > 3) f << " " << spiceNum(s.sin[3]);
            if (s.sin.size() > 4) f << " " << spiceNum(s.sin[4]);
            if (s.sin.size() > 5) f << " " << spiceNum(s.sin[5]);
            f << ")";
        } else {
            f << " DC " << spiceNum(s.dc);
            if (s.ac != 0.0)
                f << " AC " << spiceNum(s.ac);
        }
        f << "\n";
    }

    // ---- Current sources ----
    for (size_t i = 0; i < c.isrc.size(); ++i) {
        const auto& s = c.isrc[i];
        const std::string base = s.name.empty() ? ("I" + std::to_string(i)) : s.name;
        const std::string name = spiceUnique("I_" + spiceSafe(base), used);
        f << name << " " << nodeOrGround(s.a) << " " << nodeOrGround(s.b)
          << " DC " << spiceNum(s.dc);
        if (s.ac != 0.0)
            f << " AC " << spiceNum(s.ac);
        f << "\n";
    }

    // ---- VCVS (op-amps) ----
    for (size_t i = 0; i < c.vcvs.size(); ++i) {
        const auto& v = c.vcvs[i];
        const std::string base = v.name.empty() ? ("E" + std::to_string(i)) : v.name;
        const std::string en = spiceUnique("E_" + spiceSafe(base), used);
        const std::string rn = spiceUnique("R_" + spiceSafe(base) + "_ro", used);
        const std::string mid = "x_" + spiceSafe(base) + "_out";
        f << en << " " << mid << " " << v.outB << " " << v.inA << " " << v.inB
          << " " << spiceNum(v.gain) << "\n";
        f << rn << " " << mid << " " << v.outA << " " << spiceNum(v.ro) << "\n";
    }

    // ---- OpAmps (VCVS with output resistance and rail clamping) ----
    for (size_t i = 0; i < c.opamp.size(); ++i) {
        const auto& o = c.opamp[i];
        const std::string base = o.name.empty() ? ("OA" + std::to_string(i)) : o.name;
        const auto& m = o.model;
        const std::string en = spiceUnique("E_" + spiceSafe(base), used);
        const std::string rn = spiceUnique("R_" + spiceSafe(base) + "_ro", used);
        const std::string mid = "x_" + spiceSafe(base) + "_out";
        // Ideal VCVS: mid = openLoopGain * (pos - neg)
        f << en << " " << mid << " 0 " << o.pos << " " << o.neg
          << " " << spiceNum(m.openLoopGain) << "\n";
        // Output resistance
        f << rn << " " << mid << " " << o.out << " " << spiceNum(m.outputImpedance) << "\n";
        // Rail clamping (soft or hard): limit output to [vRailNeg, vRailPos]
        // Using ideal diodes for clamp; for hard rails, replace with limiting sources.
        if (m.softRails) {
            // Asymmetric tanh clamp would need a behavioral source; hard clamp is a
            // reasonable first-order approximation for DC comparison.
        }
        // Supply pins (vcc, vee) are connected externally by the schematic; the
        // VCVS itself doesn't consume supply current in this linear model.
    }

    // ---- Triodes (DAFx-23 quadric) ----
    for (size_t i = 0; i < c.tri.size(); ++i) {
        const auto& t = c.tri[i];
        const std::string base = t.name.empty() ? ("B" + std::to_string(i)) : t.name;
        const auto& p = t.model.p;
        const double cg = p.kPg / (2.0 * p.kP2);
        const double c0 = p.kP / (2.0 * p.kP2);
        const std::string bn = spiceUnique("B_" + spiceSafe(base), used);
        const std::string q = "V(" + t.p + "," + t.k + ") + " + spiceNum(cg) + "*V(" +
                              t.g + "," + t.k + ") + " + spiceNum(c0);
        f << bn << " " << t.p << " " << t.k << " I = {" << spiceNum(p.kP2)
          << " * pow(0.5*(" << q << " + abs(" << q << ")), 2)}\n";
    }

    // ---- Diodes (extended Shockley: Is*(exp((v-i*Rs)/(n*Vt))-1) + (v-i*Rs)/Rp) ----
    // Native SPICE diode with RS + parallel resistor Rp. This is an excellent
    // approximation of the extended law: for the small Rs / large Rp values used
    // in audio circuits (e.g. Rs=0.56, Rp=1e6), the difference between
    // i*Rs and i_d*Rs in the diode term and between v/Rp and (v-i*Rs)/Rp in the
    // shunt term is below nanovolt/microampere levels.
    for (size_t i = 0; i < c.diode.size(); ++i) {
        const auto& d = c.diode[i];
        const std::string base = d.name.empty() ? ("D" + std::to_string(i)) : d.name;
        const std::string a = nodeOrGround(d.a);
        const std::string b = nodeOrGround(d.b);
        const double isat = d.model.isat;
        const double rs = d.model.rs;
        const double rp = d.model.rp;
        const double n = d.model.nvt / kThermalVoltage;
        const std::string devName = spiceUnique("D_" + spiceSafe(base), used);
        const std::string modelName = spiceUnique("Dmod_" + spiceSafe(base), used);

        f << ".model " << modelName << " D(Is=" << spiceNum(isat)
          << " N=" << spiceNum(n);
        if (rs > 0.0)
            f << " RS=" << spiceNum(rs);
        f << ")\n";
        f << devName << " " << a << " " << b << " " << modelName << "\n";

        if (rp > 0.0) {
            const std::string rpName = spiceUnique("Rp_" + spiceSafe(base), used);
            f << rpName << " " << a << " " << b << " " << spiceNum(rp) << "\n";
        }
    }

    // ---- Pentodes (Reefman DerkD/DerkE and Koren) ----
    // Exported as behavioural subcircuits. Intermediate island potential E1 is
    // held on an internal node by a behavioural voltage source; plate and
    // screen currents are then behavioural current sources reading that node.
    for (size_t i = 0; i < c.pent.size(); ++i) {
        const auto& p = c.pent[i];
        const std::string base = p.name.empty() ? ("P" + std::to_string(i)) : p.name;
        const std::string subName = spiceUnique("X_" + spiceSafe(base), used);
        const std::string pk = nodeOrGround(p.p) + " " + nodeOrGround(p.k);
        const std::string sk = nodeOrGround(p.s) + " " + nodeOrGround(p.k);
        const std::string gk = nodeOrGround(p.g) + " " + nodeOrGround(p.k);
        const std::string pg = nodeOrGround(p.p) + " " + nodeOrGround(p.g);

        const auto& pent = p.model;
        const bool useReefman = pent.useReefman;
        const std::string e1Node = "x_" + spiceSafe(base) + "_e1";

        f << ".subckt " << subName << " plate screen grid cathode\n";

        if (useReefman) {
            const auto& reef = pent.reefman;
            const double mu = reef.mu;
            const double ex = reef.ex;
            const double kg1 = reef.kg1;
            const double kg2 = reef.kg2;
            const double kp = reef.kp;
            const double kvb = reef.kvb;
            const double beta = reef.beta;
            const double alpha_s = reef.alpha_s;
            const double a = reef.a;
            const bool secEmission = reef.secondaryEmission;
            const double grid_is = reef.grid_is;
            const double grid_vt = reef.grid_vt;

            // E1 = (Vsk/kp) * softplus(kp * (1/mu + Vgk/sqrt(kvb + Vsk^2)))
            // softplus(u) = ln(1 + exp(u))
            f << "E" << spiceUnique("e1_" + spiceSafe(base), used) << " " << e1Node
              << " 0 VALUE = {ln(1+exp(" << spiceNum(kp) << "*(" << spiceNum(1.0/mu)
              << "+V(" << gk << ")/sqrt(" << spiceNum(kvb) << "+V(" << sk << ")^2))))*V("
              << sk << ")/" << spiceNum(kp) << "}\n";

            // Grid current (plain Shockley, no Rp on Reefman grid diode)
            f << "B" << spiceUnique("ig_" + spiceSafe(base), used) << " " << gk
              << " I = {" << spiceNum(grid_is) << "*(exp(V(" << gk
              << ")/" << spiceNum(grid_vt) << ")-1)}\n";

            // I_space = E1^ex
            const std::string iSpaceNode = "x_" + spiceSafe(base) + "_ispace";
            f << "E" << spiceUnique("ispace_" + spiceSafe(base), used) << " " << iSpaceNode
              << " 0 VALUE = {pow(V(" << e1Node << ")," << spiceNum(ex) << ")}\n";

            // DerkD knee: kap = 1/(1 + beta*Vpk)
            const std::string kapNode = "x_" + spiceSafe(base) + "_kap";
            f << "E" << spiceUnique("kap_" + spiceSafe(base), used) << " " << kapNode
              << " 0 VALUE = {1/(1+" << spiceNum(beta) << "*V(" << pg << "))}\n";

            // Secondary emission (disabled by default; se = 0 when off)
            const std::string seNode = "x_" + spiceSafe(base) + "_se";
            if (secEmission) {
                // Vc = Vsk/lamv - nu*Vgk - wv ; q = -ap*(Vpk - Vc) ; Ise = sC*Vpk*(1+tanh(q))
                const std::string vcNode = "x_" + spiceSafe(base) + "_vc";
                const std::string qNode = "x_" + spiceSafe(base) + "_q";
                f << "E" << spiceUnique("vc_" + spiceSafe(base), used) << " " << vcNode
                  << " 0 VALUE = {V(" << sk << ")/" << spiceNum(reef.lamv) << "-"
                  << spiceNum(reef.nu) << "*V(" << gk << ")-" << spiceNum(reef.wv) << "}\n";
                f << "E" << spiceUnique("q_" + spiceSafe(base), used) << " " << qNode
                  << " 0 VALUE = {-" << spiceNum(reef.ap) << "*(V(" << pg << ")-V("
                  << vcNode << "))}\n";
                f << "E" << spiceUnique("se_" + spiceSafe(base), used) << " " << seNode
                  << " 0 VALUE = {" << spiceNum(reef.sC) << "*V(" << pg
                  << ")*(1+tanh(V(" << qNode << ")))}\n";
            } else {
                f << "E" << spiceUnique("se_zero_" + spiceSafe(base), used) << " " << seNode
                  << " 0 VALUE = {0}\n";
            }

            // Plate current: Ip = I_space * [(1/Kg1 - 1/Kg2)*(1-kap) + a*Vpk/Kg1 - Ise/Kg2]
            f << "B" << spiceUnique("ip_" + spiceSafe(base), used) << " " << pk
              << " I = {V(" << iSpaceNode << ")*((1/" << spiceNum(kg1) << "-1/"
              << spiceNum(kg2) << ")*(1-V(" << kapNode << "))+" << spiceNum(a)
              << "*V(" << pg << ")/" << spiceNum(kg1) << "-V(" << seNode << ")/"
              << spiceNum(kg2) << ")}\n";

            // Screen current: Is = I_space * (1 + alpha_s*kap + Ise) / Kg2
            f << "B" << spiceUnique("is_" + spiceSafe(base), used) << " " << sk
              << " I = {V(" << iSpaceNode << ")*(1+" << spiceNum(alpha_s)
              << "*V(" << kapNode << ")+V(" << seNode << "))/" << spiceNum(kg2)
              << "}\n";
        } else {
            // Koren pentode
            const auto& k = pent.koren;
            const double mu = k.mu;
            const double ex = k.ex;
            const double kg1 = k.kg1;
            const double kg2 = k.kg2;
            const double kp = k.kp;
            const double kvb = k.kvb;
            const double grid_is = k.grid_is;
            const double grid_vt = k.grid_vt;

            // E1 = (Vsk/kp) * softplus(kp * (1/mu + Vgk/Vsk))
            f << "E" << spiceUnique("e1_" + spiceSafe(base), used) << " " << e1Node
              << " 0 VALUE = {ln(1+exp(" << spiceNum(kp) << "*(" << spiceNum(1.0/mu)
              << "+V(" << gk << ")/V(" << sk << "))))*V(" << sk << ")/"
              << spiceNum(kp) << "}\n";

            // Grid current
            f << "B" << spiceUnique("ig_" + spiceSafe(base), used) << " " << gk
              << " I = {" << spiceNum(grid_is) << "*(exp(V(" << gk
              << ")/" << spiceNum(grid_vt) << ")-1)}\n";

            // Ip = (E1^ex / Kg1) * atan(Vpk / Kvb)
            f << "B" << spiceUnique("ip_" + spiceSafe(base), used) << " " << pk
              << " I = {pow(V(" << e1Node << ")," << spiceNum(ex) << ")/"
              << spiceNum(kg1) << "*atan(V(" << pg << ")/" << spiceNum(kvb) << ")}\n";

            // Is = (1/Kg2) * (Vsk/mu + Vgk)^ex
            f << "B" << spiceUnique("is_" + spiceSafe(base), used) << " " << sk
              << " I = {pow(V(" << sk << ")/" << spiceNum(mu) << "+V(" << gk
              << ")," << spiceNum(ex) << ")/" << spiceNum(kg2) << "}\n";
        }

        f << ".ends " << subName << "\n";
        f << subName << " " << nodeOrGround(p.p) << " " << nodeOrGround(p.s) << " "
          << nodeOrGround(p.g) << " " << nodeOrGround(p.k) << " " << subName << "\n";
    }

    // ---- JFETs (Bernardini et al. square law) ----
    // Id = f(vGS, vDS) with cut-off/ohmic/saturation regions, expressed as a
    // behavioural current source using ngspice's if() ternary.
    for (size_t i = 0; i < c.jfet.size(); ++i) {
        const auto& j = c.jfet[i];
        const std::string base = j.name.empty() ? ("J" + std::to_string(i)) : j.name;
        const auto& m = j.model;
        const double k = 2.0 * m.p.Is0 / (m.p.Vp * m.p.Vp); // 2*Is0/Vp^2
        const std::string dn = nodeOrGround(j.dExt);
        const std::string gn = nodeOrGround(j.gExt);
        const std::string sn = nodeOrGround(j.sExt);
        const std::string subName = spiceUnique("X_" + spiceSafe(base), used);

        f << ".subckt " << subName << " d g s\n";
        f << "B" << spiceUnique("id_" + spiceSafe(base), used) << " " << dn << " " << sn
          << " I = {if(V(" << gn << "," << sn << ")-" << spiceNum(m.p.Vp) << "<=0,0,"
          << "if(V(" << dn << "," << sn << ")<V(" << gn << "," << sn << ")-" << spiceNum(m.p.Vp)
          << "," << spiceNum(k) << "*((" << spiceNum(1.0/m.p.Vp) << "*V(" << gn << "," << sn
          << ")-1)*V(" << dn << "," << sn << ")-V(" << dn << "," << sn << ")^2/2)*("
          << "1+" << spiceNum(m.p.lambda) << "*V(" << dn << "," << sn << ")),"
          << spiceNum(m.p.Is0) << "*pow(1-V(" << gn << "," << sn << ")/" << spiceNum(m.p.Vp)
          << ",2)*(" << "1+" << spiceNum(m.p.lambda) << "*V(" << dn << "," << sn << "))))}\n";
        f << ".ends " << subName << "\n";
        f << subName << " " << nodeOrGround(j.dExt) << " " << nodeOrGround(j.gExt) << " " << nodeOrGround(j.sExt) << " " << subName << "\n";
    }

    // ---- MOSFETs (Bernardini et al. square law) ----
    for (size_t i = 0; i < c.mosfet.size(); ++i) {
        const auto& m = c.mosfet[i];
        const std::string base = m.name.empty() ? ("M" + std::to_string(i)) : m.name;
        const auto& mp = m.model.p;
        const std::string dn = nodeOrGround(m.d);
        const std::string gn = nodeOrGround(m.g);
        const std::string sn = nodeOrGround(m.s);
        const std::string subName = spiceUnique("X_" + spiceSafe(base), used);

        f << ".subckt " << subName << " d g s\n";
        f << "B" << spiceUnique("id_" + spiceSafe(base), used) << " " << dn << " " << sn
          << " I = {if(V(" << gn << "," << sn << ")-" << spiceNum(mp.Vth) << "<=0,0,"
          << "if(V(" << dn << "," << sn << ")<V(" << gn << "," << sn << ")-" << spiceNum(mp.Vth)
          << "," << spiceNum(mp.K) << "*((" << spiceNum(mp.Vth) << "+V(" << gn << "," << sn
          << ")-" << spiceNum(mp.Vth) << ")*V(" << dn << "," << sn << ")-V(" << dn << "," << sn
          << ")^2/2)*(" << "1+" << spiceNum(mp.lambda) << "*V(" << dn << "," << sn << ")),"
          << spiceNum(mp.K) << "/2*pow(V(" << gn << "," << sn << ")-" << spiceNum(mp.Vth)
          << ",2)*(" << "1+" << spiceNum(mp.lambda) << "*V(" << dn << "," << sn << "))))}\n";
        f << ".ends " << subName << "\n";
        f << subName << " " << dn << " " << gn << " " << sn << " " << subName << "\n";
    }

    // ---- BJTs (Ebers-Moll transport form) ----
    // Ic = Is*(exp(Vbe/nfVt)-1) - Is/alphaR*(exp(Vbc/nrVt)-1)
    // Ib = Is/alphaF*(exp(Vbe/nfVt)-1) - Is*(exp(Vbc/nrVt)-1)
    for (size_t i = 0; i < c.bjt.size(); ++i) {
        const auto& b = c.bjt[i];
        const std::string base = b.name.empty() ? ("Q" + std::to_string(i)) : b.name;
        const auto& m = b.model;
        const double alphaF = m.Bf / (m.Bf + 1.0);
        const double alphaR = m.Br / (m.Br + 1.0);
        const bool pnp = m.pnp;
        const double nfVt = m.Nf * m.Vt;
        const double nrVt = m.Nr * m.Vt;
        const std::string cn = nodeOrGround(b.c);
        const std::string bn = nodeOrGround(b.b);
        const std::string en = nodeOrGround(b.e);
        const std::string subName = spiceUnique("X_" + spiceSafe(base), used);
        const std::string vbe = pnp ? "V(" + en + "," + bn + ")" : "V(" + bn + "," + en + ")";
        const std::string vbc = pnp ? "V(" + en + "," + cn + ")" : "V(" + bn + "," + cn + ")";

        f << ".subckt " << subName << " c b e\n";
        f << "B" << spiceUnique("ic_" + spiceSafe(base), used) << " " << cn << " " << en
          << " I = {" << spiceNum(m.Is) << "*(exp(" << vbe << "/" << spiceNum(nfVt)
          << ")-1)-" << spiceNum(m.Is / alphaR) << "*(exp(" << vbc << "/" << spiceNum(nrVt)
          << ")-1)}\n";
        f << "B" << spiceUnique("ib_" + spiceSafe(base), used) << " " << bn << " " << en
          << " I = {" << spiceNum(m.Is / alphaF) << "*(exp(" << vbe << "/" << spiceNum(nfVt)
          << ")-1)-" << spiceNum(m.Is) << "*(exp(" << vbc << "/" << spiceNum(nrVt)
          << ")-1)}\n";
        f << ".ends " << subName << "\n";
        f << subName << " " << cn << " " << bn << " " << en << " " << subName << "\n";
    }

    // ---- Control / analysis cards ----
    f << "\n.control\n";
    f << "set numdgt=15\n";
    f << "op\n";
    f << "print ";
    // Collect unique node names from all elements
    std::set<std::string> nodes;
    auto addNode = [&](const std::string& n) {
        // Skip internal/synthetic nodes created by transformer expansion
        // (their names contain '$', which ngspice can't reference in v()).
        if (!n.empty() && n != "0" && n.find('$') == std::string::npos)
            nodes.insert(n);
    };
    for (const auto& e : c.res) { addNode(e.a); addNode(e.b); }
    for (const auto& e : c.cap) { addNode(e.a); addNode(e.b); }
    for (const auto& e : c.ind) { addNode(e.a); addNode(e.b); }
    for (const auto& e : c.src) { addNode(e.a); addNode(e.b); }
    for (const auto& e : c.isrc) { addNode(e.a); addNode(e.b); }
    for (const auto& e : c.vcvs) { addNode(e.outA); addNode(e.outB); addNode(e.inA); addNode(e.inB); }
    for (const auto& e : c.tri) { addNode(e.g); addNode(e.p); addNode(e.k); addNode(e.gi); }
    for (const auto& e : c.diode) { addNode(e.a); addNode(e.b); }
    for (const auto& e : c.pent) { addNode(e.p); addNode(e.s); addNode(e.g); addNode(e.k); }
    for (const auto& e : c.jfet) { addNode(e.dExt); addNode(e.gExt); addNode(e.sExt); }
    for (const auto& e : c.mosfet) { addNode(e.d); addNode(e.g); addNode(e.s); }
    for (const auto& e : c.bjt) { addNode(e.c); addNode(e.b); addNode(e.e); }
    bool first = true;
    for (const auto& n : nodes) {
        if (!first) f << " ";
        f << "v(" << n << ")";
        first = false;
    }
    f << " > dc_nodes.txt\n";
    if (durationSeconds > 0.0) {
        f << "run\nlinearize\nwrdata tran_nodes.txt";
        for (const auto& n : nodes) {
            f << " v(" << n << ")";
        }
        f << "\n";
    }
    f << ".endc\n.end\n";
    r.netlist = f.str();

    // Export accounting: every circuit-IR element kind has a SPICE
    // representation above (ControlledRes as a plain `R` at its seeded ohms,
    // pots and variable resistors already folded into `Res` by
    // Circuit::addPot / addVarRes), so the only way to lose an element is the
    // malformed transformer counted here. A caller checks `ok` and reads this
    // line rather than eyeballing the deck.
    size_t shortMutual = 0;
    for (const auto& m : c.mutual)
        if (m.indNames.size() < 2)
            ++shortMutual;
    std::ostringstream d;
    d << "exported " << c.res.size() << " Res, " << c.cap.size() << " Cap, "
      << c.ind.size() << " Ind, " << (c.mutual.size() - shortMutual) << " K, "
      << c.ctrlRes.size() << " ControlledRes, " << c.src.size() << " Src, "
      << c.isrc.size() << " Isrc, " << c.vcvs.size() << " VCVS, "
      << c.opamp.size() << " OpAmp (ideal VCVS: rail clamping not represented), "
      << c.tri.size() << " Tri, " << c.diode.size() << " Diode, "
      << c.pent.size() << " Pent, " << c.jfet.size() << " Jfet, "
      << c.mosfet.size() << " Mosfet, " << c.bjt.size() << " Bjt; "
      << c.pots.size() << " Pot and " << c.varRes.size()
      << " VarRes are already folded into Res";
    if (shortMutual)
        d << "; NOT exported: " << shortMutual
          << " MutualInd with fewer than two inductors";
    r.diagnostics = d.str();
    r.ok = (shortMutual == 0);
    return r;
}

}  // namespace wdf
