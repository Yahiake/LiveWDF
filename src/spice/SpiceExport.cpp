#include "spice/SpiceExport.h"

#include <algorithm>
#include <cctype>
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

std::string spiceKey(std::string name) {
    for (char& c : name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name;
}

std::string spiceUnique(const std::string& base, std::set<std::string>& used) {
    std::string name = base;
    int n = 2;
    while (!used.insert(spiceKey(name)).second)
        name = base + "_" + std::to_string(n++);
    return name;
}

}  // namespace

SpiceExportResult spiceExport(const Circuit& c,
                              double fs,
                              double durationSeconds) {
    SpiceExportResult r;
    std::ostringstream f;
    std::set<std::string> used;
    std::set<std::string> usedNodes;
    const auto circuitNodes = c.allNodes();
    for (const auto& node : circuitNodes)
        usedNodes.insert(spiceKey(node));
    std::map<std::string, std::string> internalNodes;
    for (const auto& node : circuitNodes) {
        if (node.find('$') != std::string::npos &&
            std::any_of(node.begin(), node.end(), [](unsigned char c) { return std::isspace(c); }))
            internalNodes.try_emplace(node, spiceUnique("lwdf_" + spiceSafe(node), usedNodes));
    }
    const auto nodeOrGround = [&](const std::string& name) -> std::string {
        if (const auto it = internalNodes.find(name); it != internalNodes.end())
            return it->second;
        return name.empty() ? "0" : name;
    };

    f << "* LiveWDF netlist exported by SpiceExport (core)\n";
    f << ".title LiveWDF\n";
    f << "* Engine integrates with trapezoidal at " << spiceNum(fs) << " Hz\n";
    for (const auto& bbd : c.bucketBrigades)
        f << "* " << spiceSafe(bbd.name)
          << " bucket-brigade clock and charge state omitted; output kept at nominal DC bias\n";
    f << ".options method=gear maxord=2 reltol=1e-4 abstol=1e-9 "
         "vntol=1e-5 chgtol=1e-12 trtol=7";
    // DC reference for the internal nodes of transformer-loaded tube stages.
    if (!c.pent.empty() && !c.mutual.empty())
        f << " rshunt=1e12";
    f << "\n";
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
        if (e.initialVolts)
            f << "* " << spiceSafe(base) << " LiveWDF initial voltage "
              << spiceNum(*e.initialVolts) << " V omitted; transient uses DC operating point\n";
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
        const std::string base = "K_" + spiceSafe(m.name.empty() ? "k" : m.name);
        auto winding = [&](const std::string& name) {
            const auto it = indElementNames.find(name);
            return it == indElementNames.end() ? spiceSafe(name) : it->second;
        };
        if (m.L.empty()) {
            f << spiceUnique(base, used);
            for (const auto& name : m.indNames)
                f << " " << winding(name);
            f << " " << spiceNum(m.k) << "\n";
            continue;
        }
        const size_t windings = m.indNames.size();
        if (m.L.size() != windings || std::any_of(m.L.begin(), m.L.end(),
                [windings](const auto& row) { return row.size() != windings; })) {
            r.diagnostics = "Invalid inductance matrix for " + m.name;
            return r;
        }
        for (size_t left = 0; left < windings; ++left) {
            for (size_t right = left + 1; right < windings; ++right) {
                const double coupling = m.L[left][right] / std::sqrt(m.L[left][left] * m.L[right][right]);
                if (coupling == 0.0)
                    continue;
                f << spiceUnique(base, used) << " " << winding(m.indNames[left]) << " "
                  << winding(m.indNames[right]) << " " << spiceNum(coupling) << "\n";
            }
        }
    }

    // ---- Controlled resistors ----
    // The engine seeds each controlled resistor from its initial ohms
    // (`Compiled::ctrlRes[].second`, read by `Engine::ctrlResValues_`), so a
    // plain `R` at that ohmage reproduces the same initial DC state.
    for (size_t i = 0; i < c.ctrlRes.size(); ++i) {
        const auto& e = c.ctrlRes[i];
        const std::string base = e.name.empty() ? ("CR" + std::to_string(i)) : e.name;
        if (e.vca) {
            const auto a = nodeOrGround(e.a);
            const auto b = nodeOrGround(e.b);
            const auto& vca = *e.vca;
            const auto sense = "V(" + nodeOrGround(vca.control) + "," +
                               nodeOrGround(vca.reference) + ")";
            f << spiceUnique("B_" + spiceSafe(base), used) << " " << a << " " << b
              << " I=";
            const std::string voltage = "V(" + a + "," + b + ")";
            if (e.dbx200Crossover != 0.0) {
                const auto bias = std::find_if(c.dbx200Biases.begin(), c.dbx200Biases.end(),
                    [i](const auto& pair) { return pair.pnp == i || pair.npn == i; });
                f << "(0.5*(" << voltage << (e.dbx200Crossover > 0.0 ? "+" : "-")
                  << "sqrt(" << voltage << "^2+" << spiceNum(e.dbx200Crossover * e.dbx200Crossover);
                if (bias != c.dbx200Biases.end()) {
                    f << "*exp(2*min(max(V(" << nodeOrGround(bias->ni) << "," << nodeOrGround(bias->pi)
                      << ")/" << spiceNum(Dbx200Bias::twiceThermalVolts) << ",-20),20))";
                }
                f << ")))*";
            } else {
                f << voltage << "*";
            }
            if (vca.blackmer) {
                f << "exp(" << spiceNum(std::log(10.0) / 20.0) << "*min(max("
                  << sense << "/" << spiceNum(vca.logVoltsPerDb) << ","
                  << spiceNum(VcaControl::blackmerMinimumDb) << "),"
                  << spiceNum(VcaControl::blackmerMaximumDb) << "))";
            } else {
                f << "min(max(" << sense << "/" << spiceNum(vca.fullGainVolts) << ","
                  << spiceNum(VcaControl::minimumGain) << "),"
                  << spiceNum(VcaControl::maximumGain) << ")";
            }
            f << "/" << spiceNum(e.initialOhms * vca.initialGain()) << "\n";
            continue;
        }
        if (e.ota) {
            const auto a = nodeOrGround(e.a);
            const auto b = nodeOrGround(e.b);
            f << spiceUnique("B_" + spiceSafe(base), used) << " " << a << " " << b
              << " I=" << spiceNum(OtaControl::transportGain) << "*max(V("
              << nodeOrGround(e.ota->bias) << "," << nodeOrGround(e.ota->biasDiode) << ")/"
              << spiceNum(OtaControl::senseOhms) << "," << spiceNum(OtaControl::minimumCurrent)
              << ")*tanh(V(" << a << "," << b << ")/" << spiceNum(OtaControl::pairVolts)
              << ")\n";
            continue;
        }
        if (e.photo)
            f << "* " << spiceSafe(base) << " photoresistor frozen at dark resistance\n";
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
        const auto detector = std::find_if(c.rmsDetectors.begin(), c.rmsDetectors.end(),
            [i](const auto& rms) { return rms.outputCurrentIndex == i; });
        if (detector != c.rmsDetectors.end()) {
            const auto& m = detector->model;
            const auto base = spiceSafe(detector->name);
            const std::string power = "x_" + base + "_power";
            const std::string input = "V(" + nodeOrGround(detector->input) + "," +
                                      nodeOrGround(detector->common) + ")";
            const std::string instantaneous =
                "min(abs(" + input + ")/" + spiceNum(m.referenceVolts) + "," +
                spiceNum(RmsDetectorModel::maximumNormalizedInput) + ")^2";
            f << spiceUnique("B_" + base + "_power", used) << " 0 " << power << " I=";
            if (m.attackSeconds() == m.releaseSeconds()) {
                f << instantaneous << "\n";
                f << spiceUnique("R_" + base + "_mean", used) << " " << power << " 0 1\n";
                f << spiceUnique("C_" + base + "_mean", used) << " " << power << " 0 "
                  << spiceNum(m.attackSeconds()) << "\n";
            } else {
                const std::string difference = instantaneous + "-V(" + power + ")";
                f << "(" << difference << ")/ternary_fcn((" << difference << ")>0,"
                  << spiceNum(m.attackSeconds()) << "," << spiceNum(m.releaseSeconds()) << ")\n";
                f << spiceUnique("C_" + base + "_mean", used) << " " << power << " 0 1\n";
            }
            f << spiceUnique("B_" + base + "_drive", used) << " " << nodeOrGround(s.a)
              << " " << nodeOrGround(s.b) << " I="
              << spiceNum(10.0 * m.signedVoltsPerDb() / (std::log(10.0) * m.outputOhms))
              << "*ln(min(max(V(" << power << ")," << spiceNum(RmsDetectorModel::minimumPower)
              << ")," << spiceNum(RmsDetectorModel::maximumPower) << "))\n";
            continue;
        }
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
        const std::string mid = spiceUnique("x_" + spiceSafe(base) + "_out", usedNodes);
        f << en << " " << mid << " " << v.outB << " " << v.inA << " " << v.inB
          << " " << spiceNum(v.gain) << "\n";
        f << rn << " " << mid << " " << v.outA << " " << spiceNum(v.ro) << "\n";
    }

    // ---- OpAmps (VCVS with output resistance and rail clamping) ----
    for (size_t i = 0; i < c.opamp.size(); ++i) {
        const auto& o = c.opamp[i];
        const std::string base = o.name.empty() ? ("OA" + std::to_string(i)) : o.name;
        const auto& m = o.model;
        const std::string mid = spiceUnique("x_" + spiceSafe(base) + "_out", usedNodes);
        const std::string raw = spiceNum(m.openLoopGain) + "*V(" + o.pos + "," + o.neg + ")";
        std::string railLimited = "min(max(" + raw + "," + spiceNum(m.railLow()) +
                                  ")," + spiceNum(m.railHigh()) + ")";
        if (m.softRails) {
            const auto softLimit = [&](double rail) {
                return std::abs(rail) < 1e9 && rail != 0.0
                    ? "(" + spiceNum(rail) + ")*tanh((" + raw + ")/(" + spiceNum(rail) + "))"
                    : raw;
            };
            railLimited = "ternary_fcn((" + raw + ")>=0," + softLimit(m.railHigh()) +
                          "," + softLimit(m.railLow()) + ")";
        }
        if (m.hasClassABOutput()) {
            const std::string voltageSource = spiceUnique("B_" + spiceSafe(base) + "_pole", used);
            const std::string outputStage = spiceUnique("B_" + spiceSafe(base) + "_stage", used);
            const std::string limit = spiceNum(m.outputCurrentLimit);
            const std::string is = spiceNum(m.outputJunctionIs);
            const std::string total = spiceNum(m.outputCurrentLimit + m.outputJunctionIs +
                                               OpAmpModel::kSteeringDiodeIs);
            const std::string ratio = spiceNum(m.outputJunctionIs / OpAmpModel::kSteeringDiodeIs);
            const auto branch = [&](const std::string& v) {
                const std::string expArg = "min(max((-" + v + ")/" +
                                           spiceNum(kThermalVoltage) + ",-700),700)";
                return "min(max(" + total + "/(1+exp(" + expArg + ")/" + ratio +
                       ")-" + is + ",0)," + limit + ")";
            };
            f << voltageSource << " " << mid << " 0 V = {" << railLimited << "}\n";
            f << outputStage << " " << o.out << " 0 I = {" << branch("V(" + o.out + "," + mid + ")")
              << "-" << branch("V(" + mid + "," + o.out + ")") << "}\n";
            continue;
        }
        const std::string en = spiceUnique("B_" + spiceSafe(base), used);
        const std::string rn = spiceUnique("R_" + spiceSafe(base) + "_ro", used);
        f << en << " " << mid << " 0 V = {" << railLimited << "}\n";
        f << rn << " " << mid << " " << o.out << " " << spiceNum(m.outputImpedance) << "\n";
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
        if (d.model.perveance > 0.0) {
            std::string plate = a;
            if (d.model.rs > 0.0) {
                plate = spiceUnique("plate_" + spiceSafe(base), usedNodes);
                f << spiceUnique("Rs_" + spiceSafe(base), used) << " " << a << " " << plate
                  << " " << spiceNum(d.model.rs) << "\n";
            }
            f << spiceUnique("B_" + spiceSafe(base), used) << " " << plate << " " << b
              << " I = {" << spiceNum(d.model.perveance) << "*pow(max(V(" << plate
              << "," << b << "),0),1.5)}\n";
            continue;
        }
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
        if (d.model.bv > 0.0)
            f << " BV=" << spiceNum(d.model.bv) << " IBV=" << spiceNum(d.model.ibv);
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

    // The forward square law is shared by both FET device models. Select
    // gate-to-source or gate-to-drain overdrive when the channel reverses.
    if (!c.jfet.empty() || !c.mosfet.empty()) {
        f << ".func lwdf_overdrive(ds,gs,gd,vt) "
             "max(ternary_fcn(ds>=0,gs,gd)-vt,0)\n"
             ".func lwdf_channel(ds,ov,k,l) "
             "sgn(ds)*k*min(abs(ds),ov)*(ov-min(abs(ds),ov)/2)*(1+l*abs(ds))\n";
    }
    const auto emitFet = [&](const std::string& base, const std::string& d,
                             const std::string& g, const std::string& s,
                             bool pChannel, double threshold, double k, double lambda) {
        const std::string dn = nodeOrGround(d);
        const std::string gn = nodeOrGround(g);
        const std::string sn = nodeOrGround(s);
        const auto voltage = [&](const std::string& pos, const std::string& neg) {
            return pChannel ? "V(" + neg + "," + pos + ")" : "V(" + pos + "," + neg + ")";
        };
        const std::string ds = voltage(dn, sn);
        f << spiceUnique("B_id_" + spiceSafe(base), used) << " " << dn << " " << sn
          << " I = {" << (pChannel ? "-" : "") << "lwdf_channel(" << ds
          << ",lwdf_overdrive(" << ds << "," << voltage(gn, sn) << ","
          << voltage(gn, dn) << "," << spiceNum(threshold) << "),"
          << spiceNum(k) << "," << spiceNum(lambda) << ")}\n";
    };

    for (size_t i = 0; i < c.jfet.size(); ++i) {
        const auto& j = c.jfet[i];
        const std::string base = j.name.empty() ? ("J" + std::to_string(i)) : j.name;
        emitFet(base, j.d, j.g, j.s, j.model.p.pChannel, j.model.p.Vp,
                j.model.k(), j.model.p.lambda);
    }
    for (size_t i = 0; i < c.mosfet.size(); ++i) {
        const auto& m = c.mosfet[i];
        const std::string base = m.name.empty() ? ("M" + std::to_string(i)) : m.name;
        emitFet(base, m.d, m.g, m.s, m.model.p.pChannel, m.model.p.Vth,
                m.model.p.K, m.model.p.lambda);
    }

    // Native BJTs use the same Ebers-Moll parameters and let ngspice apply
    // its junction limiting while finding the operating point.
    for (size_t i = 0; i < c.bjt.size(); ++i) {
        const auto& b = c.bjt[i];
        const std::string base = b.name.empty() ? ("Q" + std::to_string(i)) : b.name;
        const auto& m = b.model;
        const std::string modelName = spiceUnique("Qmod_" + spiceSafe(base), used);
        const std::string deviceName = spiceUnique("Q_" + spiceSafe(base), used);
        f << ".model " << modelName << (m.pnp ? " PNP" : " NPN") << "(IS=" << spiceNum(m.Is)
          << " BF=" << spiceNum(m.Bf) << " BR=" << spiceNum(m.Br)
          << " NF=" << spiceNum(m.Nf) << " NR=" << spiceNum(m.Nr);
        if (m.Vaf > 0.0)
            f << " VAF=" << spiceNum(m.Vaf);
        if (m.Var > 0.0)
            f << " VAR=" << spiceNum(m.Var);
        if (m.Ise > 0.0)
            f << " ISE=" << spiceNum(m.Ise) << " NE=" << spiceNum(m.Ne);
        if (m.Ikf > 0.0)
            f << " IKF=" << spiceNum(m.Ikf);
        f << ")\n";
        f << deviceName << " " << nodeOrGround(b.c) << " " << nodeOrGround(b.b)
          << " " << nodeOrGround(b.e) << " " << modelName << "\n";
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
      << c.opamp.size() << " OpAmp (static rail limiting), "
      << c.tri.size() << " Tri, " << c.diode.size() << " Diode, "
      << c.pent.size() << " Pent, " << c.jfet.size() << " Jfet, "
      << c.mosfet.size() << " Mosfet, " << c.bjt.size() << " Bjt; "
      << c.pots.size() << " Pot and " << c.varRes.size()
      << " VarRes are already folded into Res";
    if (shortMutual)
        d << "; NOT exported: " << shortMutual
          << " MutualInd with fewer than two inductors";
    const auto photoCount = std::count_if(c.ctrlRes.begin(), c.ctrlRes.end(),
                                         [](const auto& e) { return e.photo.has_value(); });
    if (photoCount)
        d << "; " << photoCount << " photoresistor(s) fixed at dark resistance";
    const auto initialCapCount = std::count_if(
        c.cap.begin(), c.cap.end(), [](const auto& e) { return e.initialVolts.has_value(); });
    if (initialCapCount)
        d << "; " << initialCapCount << " capacitor startup override(s) omitted (DC operating point)";
    const auto vcaCount = std::count_if(c.ctrlRes.begin(), c.ctrlRes.end(),
                                        [](const auto& e) { return e.vca.has_value(); });
    if (vcaCount)
        d << "; " << vcaCount << " VCA(s) with continuous SPICE control";
    const auto otaCount = std::count_if(c.ctrlRes.begin(), c.ctrlRes.end(),
                                        [](const auto& e) { return e.ota.has_value(); });
    if (otaCount)
        d << "; " << otaCount << " OTA(s) with continuous SPICE bias control";
    if (!c.rmsDetectors.empty())
        d << "; " << c.rmsDetectors.size() << " log RMS detector(s) with continuous SPICE averaging";
    if (!c.bucketBrigades.empty())
        d << "; " << c.bucketBrigades.size()
          << " bucket-brigade delay(s) fixed at nominal DC output bias, transient wet path omitted";
    r.diagnostics = d.str();
    r.ok = (shortMutual == 0);
    return r;
}

}  // namespace wdf
