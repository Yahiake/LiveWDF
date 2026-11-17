#include <catch2.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "devices/Bjt.h"
#include "devices/Jfet.h"
#include "devices/Mosfet.h"
#include "oracle/transistor_cards.h"
#include "spice/Circuit.h"
#include "spice/SpiceNet.h"
#include "support/NgspiceOracle.h"
#include "wdf/Engine.h"

// Transistor oracle matrix, reusing the in-process ngspice harness in
// support/NgspiceOracle.h.
//
// Two kinds of checks are deliberately kept separate:
//
//   * equation-matched: the ngspice card is built from exactly the parameters
//     the LiveWDF law reads, with every unimplemented SPICE effect absent. A
//     failure here is a model/mapping defect and is asserted.
//
//   * preset realism: the reference is the full LTspice library card, which
//     contains Early effect, high injection, series resistances, junctions,
//     temperature and (for MOSFETs) Level-3/VDMOS behavior. LiveWDF implements
//     a reduced law, so these are characterization measurements: both engines
//     must converge and produce a physical operating point, but the numeric
//     difference is reported, not tuned into agreement.
//
// The reference cards are embedded in tests/oracle/transistor_cards.h.

using namespace lwdf_test;

namespace {

constexpr double kFs = 48000.0;
constexpr double kVoltageTol = 50.0;  // verify.py DC threshold
constexpr double kCurrentTol = 0.1;  // A, same scale as the JFET oracle
constexpr double kCurrentFloor = 1e-9;

std::string number(double v) {
    std::ostringstream s;
    s << std::setprecision(17) << v;
    return s.str();
}

std::string regionName(wdf::BjtRegion r) {
    switch (r) {
        case wdf::BjtRegion::Cutoff: return "cutoff";
        case wdf::BjtRegion::ForwardActive: return "forward-active";
        case wdf::BjtRegion::Saturation: return "saturation";
        case wdf::BjtRegion::ReverseActive: return "reverse-active";
        default: return "invalid";
    }
}

std::string regionName(wdf::MosfetRegion r) {
    switch (r) {
        case wdf::MosfetRegion::Cutoff: return "cutoff";
        case wdf::MosfetRegion::Ohmic: return "ohmic";
        case wdf::MosfetRegion::Saturation: return "saturation";
        default: return "invalid";
    }
}

std::string regionName(wdf::JfetRegion r) {
    switch (r) {
        case wdf::JfetRegion::Cutoff: return "cutoff";
        case wdf::JfetRegion::Ohmic: return "ohmic";
        case wdf::JfetRegion::Saturation: return "saturation";
        default: return "invalid";
    }
}

const std::vector<std::string> kCsvHeader = {"family", "model",    "topology", "region",
                                             "quantity", "ngspice", "livewdf",  "abs_error",
                                             "rel_error"};

std::vector<std::string> row(const std::string& family, const std::string& model,
                             const std::string& topology, const std::string& region,
                             const std::string& quantity, double ng, double lw) {
    return {family, model, topology, region, quantity, number(ng), number(lw),
            number(std::abs(lw - ng)), number(relativeError(ng, lw, kCurrentFloor))};
}

// ---------------------------------------------------------------------------
// BJT fixtures
// ---------------------------------------------------------------------------

enum class BjtElemKind { Source, Resistor, Transistor };

struct BjtElem {
    BjtElemKind kind = BjtElemKind::Resistor;
    std::string a, b, c;
    double value = 0.0;
};

BjtElem bjtSrc(std::string a, std::string b, double value) {
    return {BjtElemKind::Source, std::move(a), std::move(b), {}, value};
}
BjtElem bjtRes(std::string a, std::string b, double value) {
    return {BjtElemKind::Resistor, std::move(a), std::move(b), {}, value};
}
BjtElem bjtNpn(std::string c, std::string b, std::string e) {
    return {BjtElemKind::Transistor, std::move(c), std::move(b), std::move(e), 0.0};
}

struct BjtFixture {
    std::vector<BjtElem> elems;
    std::vector<std::string> compareNodes;
};

wdf::Circuit bjtCircuit(const BjtFixture& f, const wdf::BjtModel& model) {
    wdf::Circuit c;
    for (const auto& e : f.elems) {
        if (e.kind == BjtElemKind::Source)
            c.addSrc(e.a, e.b, e.value);
        else if (e.kind == BjtElemKind::Resistor)
            c.addRes(e.a, e.b, e.value);
        else
            c.addBjt(e.a, e.b, e.c, model, "Q1");
    }
    return c;
}

std::string bjtNet(const BjtFixture& f, const std::string& card,
                   const std::string& modelName = "QMOD") {
    std::ostringstream d;
    d << "* livewdf bjt oracle\n";
    int source = 0, resistor = 0;
    for (const auto& e : f.elems) {
        if (e.kind == BjtElemKind::Source)
            d << "V" << ++source << " " << e.a << " " << e.b << " DC " << number(e.value)
              << "\n";
        else if (e.kind == BjtElemKind::Resistor)
            d << "R" << ++resistor << " " << e.a << " " << e.b << " " << number(e.value)
              << "\n";
        else
            d << "Q1 " << e.a << " " << e.b << " " << e.c << " " << modelName << "\n";
    }
    d << card << "\n";
    return d.str();
}

std::string bjtCoreCard(const wdf::BjtModel& m) {
    std::ostringstream s;
    s << ".model QMOD NPN(IS=" << number(m.Is) << " BF=" << number(m.Bf)
      << " BR=" << number(m.Br) << " NF=" << number(m.Nf) << " NR=" << number(m.Nr)
      << ")";
    return s.str();
}

struct BjtPoint {
    std::vector<std::string> nodes;
    std::vector<double> ngNode;
    std::vector<double> engNode;
    double ngIc = 0.0, ngIb = 0.0, ngIe = 0.0;
    double engIc = 0.0, engIb = 0.0, engIe = 0.0;
    double vce = 0.0, vbe = 0.0, vbc = 0.0;
    wdf::BjtRegion region = wdf::BjtRegion::Invalid;
    double maxNodeErr = 0.0;
    double maxCurrentErr = 0.0;
};

BjtPoint runBjt(const BjtFixture& f, const wdf::BjtModel& model, const std::string& card,
                const std::string& tag, const std::string& modelName = "QMOD") {
    std::vector<std::string> vecs;
    for (const auto& n : f.compareNodes) vecs.push_back("v(" + n + ")");
    vecs.push_back("@Q1[ic]");
    vecs.push_back("@Q1[ib]");
    vecs.push_back("@Q1[ie]");

    const std::string net = bjtNet(f, card, modelName);
    const auto run = runDeck(net + controlBlock({"op"}, vecs, tag) + ".end\n", tag);
    REQUIRE(run.ok);

    BjtPoint p;
    p.nodes = f.compareNodes;
    for (size_t i = 0; i < f.compareNodes.size(); ++i) {
        const auto v = parseOpFile(vecFile(tag, i));
        REQUIRE(v.size() == 1);
        p.ngNode.push_back(v[0]);
    }
    const size_t base = f.compareNodes.size();
    const auto ngIc = parseOpFile(vecFile(tag, base));
    const auto ngIb = parseOpFile(vecFile(tag, base + 1));
    const auto ngIe = parseOpFile(vecFile(tag, base + 2));
    REQUIRE(ngIc.size() == 1);
    REQUIRE(ngIb.size() == 1);
    REQUIRE(ngIe.size() == 1);
    p.ngIc = ngIc[0];
    p.ngIb = ngIb[0];
    p.ngIe = ngIe[0];

    const wdf::Circuit c = bjtCircuit(f, model);
    wdf::Engine e(c, kFs);
    std::vector<double> node, rootV;
    REQUIRE(e.solveDc(node, rootV));
    for (const auto& n : f.compareNodes) {
        const int idx = e.nodeIndex(n);
        REQUIRE(idx >= 0);
        p.engNode.push_back(node[static_cast<size_t>(idx)]);
    }
    std::vector<double> current, jac;
    e.currents(rootV, current, jac);
    REQUIRE(current.size() == 3);
    p.engIc = current[0];
    p.engIb = current[1];
    p.engIe = current[2];
    p.vce = rootV[0];
    p.vbe = rootV[1];
    p.vbc = p.vbe - p.vce;
    p.region = wdf::classifyRegion(p.vbe, p.vbc);

    for (size_t i = 0; i < p.nodes.size(); ++i)
        p.maxNodeErr = std::max(p.maxNodeErr, std::abs(p.ngNode[i] - p.engNode[i]));
    p.maxCurrentErr = std::max({std::abs(p.ngIc - p.engIc), std::abs(p.ngIb - p.engIb),
                                std::abs(p.ngIe - p.engIe)});
    INFO("tag=" << tag << " nodes ng/lw"
                << [&] {
                       std::ostringstream s;
                       for (size_t i = 0; i < p.nodes.size(); ++i)
                           s << " " << p.nodes[i] << "=" << p.ngNode[i] << "/" << p.engNode[i];
                       return s.str();
                   }()
                << " Ic/Ib/Ie ng=" << p.ngIc << "/" << p.ngIb << "/" << p.ngIe
                << " lw=" << p.engIc << "/" << p.engIb << "/" << p.engIe << " region="
                << regionName(p.region));
    return p;
}

void requireBjtMatches(const BjtPoint& p, const std::string& what) {
    std::ostringstream detail;
    detail << what << " nodes";
    for (size_t i = 0; i < p.nodes.size(); ++i)
        detail << " " << p.nodes[i] << "=" << p.ngNode[i] << "/" << p.engNode[i];
    detail << " Ic/Ib/Ie ng=" << p.ngIc << "/" << p.ngIb << "/" << p.ngIe
           << " lw=" << p.engIc << "/" << p.engIb << "/" << p.engIe
           << " region=" << regionName(p.region);
    INFO(detail.str());
    REQUIRE(p.maxNodeErr <= kVoltageTol);
    REQUIRE(p.maxCurrentErr <= kCurrentTol);
    // ngspice's printed terminal currents satisfy KCL only to its own solve
    // tolerance; 1.9e-9 A on a 1.3 mA operating point is round-off from the
    // simulator, not a model difference. LiveWDF's returned currents satisfy
    // the identity algebraically, so its bound stays tighter.
    REQUIRE(std::abs(p.ngIc + p.ngIb + p.ngIe) <= 1e-8);
    REQUIRE(std::abs(p.engIc + p.engIb + p.engIe) <= 1e-12);
}

void appendBjtRows(std::vector<std::vector<std::string>>& rows, const std::string& model,
                   const std::string& topology, const BjtPoint& p) {
    const std::string reg = regionName(p.region);
    for (size_t i = 0; i < p.nodes.size(); ++i)
        rows.push_back(row("BJT", model, topology, reg, "v(" + p.nodes[i] + ")", p.ngNode[i],
                           p.engNode[i]));
    rows.push_back(row("BJT", model, topology, reg, "iC", p.ngIc, p.engIc));
    rows.push_back(row("BJT", model, topology, reg, "iB", p.ngIb, p.engIb));
    rows.push_back(row("BJT", model, topology, reg, "iE", p.ngIe, p.engIe));
}

BjtFixture bjtCommonEmitter(double vcc, double rb, double rc, double re) {
    return {{bjtSrc("vcc", "0", vcc), bjtRes("vcc", "c", rc), bjtRes("vcc", "b", rb),
             bjtNpn("c", "b", "e"), bjtRes("e", "0", re)},
            {"c", "b", "e"}};
}

// ---------------------------------------------------------------------------
// MOSFET fixtures
// ---------------------------------------------------------------------------

struct MosPreset {
    const char* name;
    double K;
    double Vth;
    double lambda;
};

// These are the exact K/Vth/lambda values written by the editor preset switch
// in editor/source/InspectorPanel.cpp (2N7000/BS170, BSS138, 2N7002, IRLZ44N,
// IRF540N, AO3400A, IRF840). There is no src/devices MOSFET preset registry;
// PartModels.cpp lists the names and the inspector supplies the values.
const MosPreset kMosPresets[] = {
    {"2N7000", 20.0e-3, 2.1, 0.01},  {"BSS138", 5.0e-3, 1.3, 0.02},
    {"2N7002", 20.0e-3, 2.0, 0.01},  {"IRLZ44N", 150.0e-3, 1.5, 0.01},
    {"IRF540N", 80.0e-3, 2.5, 0.01}, {"IRF840", 30.0e-3, 3.0, 0.005},
};

std::string mosCard(const MosPreset& p) {
    std::ostringstream s;
    s << ".model MMOD NMOS(Level=1 KP=" << number(p.K) << " VTO=" << number(p.Vth)
      << " LAMBDA=" << number(p.lambda) << ")";
    return s.str();
}

std::string mosCommonSourceNet(double vdd, double vg, double rd, const std::string& model,
                               const std::string& card, bool instanceWl = true) {
    std::ostringstream s;
    s << "* livewdf mosfet common-source oracle\n"
      << "VDD vdd 0 DC " << number(vdd) << "\n"
      << "VG g 0 DC " << number(vg) << "\n"
      << "RD vdd d " << number(rd) << "\n"
      << "M1 d g 0 0 " << model;
    if (instanceWl) s << " W=1 L=1";
    s << "\n" << card << "\n";
    return s.str();
}

std::string mosSourceFollowerNet(double vdd, double vg, double rs,
                                 const std::string& model, const std::string& card) {
    std::ostringstream s;
    s << "* livewdf mosfet source-follower oracle\n"
      << "VDD vdd 0 DC " << number(vdd) << "\n"
      << "VG g 0 DC " << number(vg) << "\n"
      << "RS s 0 " << number(rs) << "\n"
      << "M1 vdd g s s " << model << " W=1 L=1\n"
      << card << "\n";
    return s.str();
}

struct MosPoint {
    double ngNode = 0.0, engNode = 0.0;
    double ngId = 0.0, engId = 0.0;
    wdf::MosfetRegion region = wdf::MosfetRegion::Invalid;
    double maxNodeErr = 0.0;
    double maxCurrentErr = 0.0;
};

MosPoint runMos(const std::string& deckNet, const std::string& tag, const std::string& node,
                const wdf::Circuit& circuit) {
    REQUIRE(circuit.mosfet.size() == 1);
    const auto run = runDeck(
        deckNet + controlBlock({"op"}, {"v(" + node + ")", "@M1[id]"}, tag) + ".end\n", tag);
    REQUIRE(run.ok);
    const auto ngV = parseOpFile(vecFile(tag, 0));
    const auto ngI = parseOpFile(vecFile(tag, 1));
    REQUIRE(ngV.size() == 1);
    REQUIRE(ngI.size() == 1);

    wdf::Engine e(circuit, kFs);
    std::vector<double> nodeV, rootV;
    REQUIRE(e.solveDc(nodeV, rootV));
    const int idx = e.nodeIndex(node);
    REQUIRE(idx >= 0);
    std::vector<double> current, jac;
    e.currents(rootV, current, jac);
    REQUIRE(current.size() == 2);

    MosPoint p;
    p.ngNode = ngV[0];
    p.engNode = nodeV[static_cast<size_t>(idx)];
    p.ngId = ngI[0];
    p.engId = current[0];
    p.region = circuit.mosfet[0].model.evaluate(rootV[1], rootV[0]).region;
    p.maxNodeErr = std::abs(p.ngNode - p.engNode);
    p.maxCurrentErr = std::abs(p.ngId - p.engId);
    INFO("tag=" << tag << " " << node << " ng/lw=" << p.ngNode << "/" << p.engNode
                << " Id=" << p.ngId << "/" << p.engId << " region=" << regionName(p.region));
    return p;
}

void requireMosMatches(const MosPoint& p, const std::string& what) {
    INFO(what);
    REQUIRE(p.maxNodeErr <= kVoltageTol);
    REQUIRE(p.maxCurrentErr <= kCurrentTol);
}

void appendMosRows(std::vector<std::vector<std::string>>& rows, const std::string& model,
                   const std::string& topology, const std::string& node,
                   const MosPoint& p) {
    const std::string reg = regionName(p.region);
    rows.push_back(row("MOSFET", model, topology, reg, "v(" + node + ")", p.ngNode, p.engNode));
    rows.push_back(row("MOSFET", model, topology, reg, "iD", p.ngId, p.engId));
}

// ---------------------------------------------------------------------------
// JFET fixtures
// ---------------------------------------------------------------------------

std::string jfetCoreCard(const wdf::JfetModel& m) {
    const double beta = m.p.Is0 / (m.p.Vp * m.p.Vp);
    std::ostringstream s;
    s << ".model JMOD NJF(Beta=" << number(beta) << " Vto=" << number(m.p.Vp)
      << " Lambda=" << number(m.p.lambda) << " Rd=" << number(m.p.rd)
      << " Rs=" << number(m.p.rs) << ")";
    return s.str();
}

std::string jfetCommonSourceNet(double vdd, double vg, double rd,
                                const std::string& model, const std::string& card) {
    std::ostringstream s;
    s << "* livewdf jfet common-source oracle\n"
      << "VDD vdd 0 DC " << number(vdd) << "\n"
      << "VG g 0 DC " << number(vg) << "\n"
      << "RD vdd d " << number(rd) << "\n"
      << "J1 d g 0 " << model << "\n"
      << card << "\n";
    return s.str();
}

std::string jfetSelfBiasNet(double vdd, double rd, double rs, const std::string& model,
                            const std::string& card) {
    std::ostringstream s;
    s << "* livewdf jfet self-bias oracle\n"
      << "VDD vdd 0 DC " << number(vdd) << "\n"
      << "RD vdd d " << number(rd) << "\n"
      << "RS s 0 " << number(rs) << "\n"
      << "RG g 0 1meg\n"
      << "J1 d g s " << model << "\n"
      << card << "\n";
    return s.str();
}

std::string jfetSourceFollowerNet(double vdd, double vg, double rs,
                                  const std::string& model, const std::string& card) {
    std::ostringstream s;
    s << "* livewdf jfet source-follower oracle\n"
      << "VDD vdd 0 DC " << number(vdd) << "\n"
      << "VG g 0 DC " << number(vg) << "\n"
      << "RS s 0 " << number(rs) << "\n"
      << "J1 vdd g s " << model << "\n"
      << card << "\n";
    return s.str();
}

struct JfetPoint {
    std::vector<std::string> nodes;
    std::vector<double> ngNode;
    std::vector<double> engNode;
    double ngId = 0.0, engId = 0.0;
    wdf::JfetRegion region = wdf::JfetRegion::Invalid;
    double maxNodeErr = 0.0;
    double maxCurrentErr = 0.0;
};

JfetPoint runJfet(const std::string& deckNet, const std::string& tag,
                  const std::vector<std::string>& nodes, const wdf::Circuit& circuit) {
    REQUIRE(circuit.jfet.size() == 1);
    std::vector<std::string> vecs;
    for (const auto& n : nodes) vecs.push_back("v(" + n + ")");
    vecs.push_back("@J1[id]");
    const auto run = runDeck(deckNet + controlBlock({"op"}, vecs, tag) + ".end\n", tag);
    REQUIRE(run.ok);

    JfetPoint p;
    p.nodes = nodes;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto v = parseOpFile(vecFile(tag, i));
        REQUIRE(v.size() == 1);
        p.ngNode.push_back(v[0]);
    }
    const auto ngI = parseOpFile(vecFile(tag, nodes.size()));
    REQUIRE(ngI.size() == 1);
    p.ngId = ngI[0];

    wdf::Engine e(circuit, kFs);
    std::vector<double> nodeV, rootV;
    REQUIRE(e.solveDc(nodeV, rootV));
    for (const auto& n : nodes) {
        const int idx = e.nodeIndex(n);
        REQUIRE(idx >= 0);
        p.engNode.push_back(nodeV[static_cast<size_t>(idx)]);
    }
    std::vector<double> current, jac;
    e.currents(rootV, current, jac);
    REQUIRE(current.size() == 2);
    p.engId = current[0];
    p.region = circuit.jfet[0].model.evaluate(rootV[1], rootV[0]).region;

    for (size_t i = 0; i < nodes.size(); ++i)
        p.maxNodeErr = std::max(p.maxNodeErr, std::abs(p.ngNode[i] - p.engNode[i]));
    p.maxCurrentErr = std::abs(p.ngId - p.engId);
    INFO("tag=" << tag << " nodes ng/lw"
                << [&] {
                       std::ostringstream s;
                       for (size_t i = 0; i < nodes.size(); ++i)
                           s << " " << nodes[i] << "=" << p.ngNode[i] << "/" << p.engNode[i];
                       return s.str();
                   }()
                << " Id=" << p.ngId << "/" << p.engId << " region=" << regionName(p.region));
    return p;
}

void requireJfetMatches(const JfetPoint& p, const std::string& what) {
    INFO(what);
    REQUIRE(p.maxNodeErr <= kVoltageTol);
    REQUIRE(p.maxCurrentErr <= kCurrentTol);
}

void appendJfetRows(std::vector<std::vector<std::string>>& rows, const std::string& model,
                    const std::string& topology, const JfetPoint& p) {
    const std::string reg = regionName(p.region);
    for (size_t i = 0; i < p.nodes.size(); ++i)
        rows.push_back(row("JFET", model, topology, reg, "v(" + p.nodes[i] + ")", p.ngNode[i],
                           p.engNode[i]));
    rows.push_back(row("JFET", model, topology, reg, "iD", p.ngId, p.engId));
}

}  // namespace

TEST_CASE("ngspice DC oracle: BJT equation-matched Ebers-Moll topologies",
          "[bjt][ngspice][dc][equation]") {
    if (!requireNgspice("BJT equation-matched topologies")) return;

    wdf::BjtModel model;
    model.Is = 1e-14;
    model.Bf = 300.0;
    model.Br = 4.0;
    model.Nf = 1.0;
    model.Nr = 1.0;
    model.prepare();
    const std::string card = bjtCoreCard(model);

    struct Case {
        const char* name;
        BjtFixture fixture;
        wdf::BjtRegion expected;
    };
    const Case cases[] = {
        {"common-emitter", bjtCommonEmitter(5.0, 1.0e6, 220.0, 47.0),
         wdf::BjtRegion::ForwardActive},
        {"divider-bias",
         {{bjtSrc("vcc", "0", 12.0), bjtRes("vcc", "b", 100.0e3), bjtRes("b", "0", 22.0e3),
           bjtRes("vcc", "c", 4.7e3), bjtNpn("c", "b", "e"), bjtRes("e", "0", 1.0e3)},
          {"c", "b", "e"}},
         wdf::BjtRegion::ForwardActive},
        {"emitter-follower",
         {{bjtSrc("vcc", "0", 10.0), bjtRes("vcc", "b", 470.0e3), bjtNpn("vcc", "b", "e"),
           bjtRes("e", "0", 1.0e3)},
          {"b", "e"}},
         wdf::BjtRegion::ForwardActive},
        {"cutoff",
         {{bjtSrc("in", "0", 0.0), bjtRes("in", "b", 10.0e3), bjtSrc("vcc", "0", 5.0),
           bjtRes("vcc", "c", 1.0e3), bjtNpn("c", "b", "0")},
          {"c", "b"}},
         wdf::BjtRegion::Cutoff},
    };

    std::vector<std::vector<std::string>> rows;
    for (const auto& c : cases) {
        const auto p = runBjt(c.fixture, model, card, std::string("bjt_eq_") + c.name);
        requireBjtMatches(p, c.name);
        REQUIRE(p.region == c.expected);
        if (c.expected == wdf::BjtRegion::ForwardActive) {
            REQUIRE(p.ngIc / p.ngIb == Approx(model.Bf).epsilon(1e-4));
            REQUIRE(p.engIc / p.engIb == Approx(model.Bf).epsilon(1e-4));
        }
        appendBjtRows(rows, "core", c.name, p);
    }
    writeCsv("bjt_equation", kCsvHeader, rows);
    WARN("BJT equation-matched oracle: " << sizeof(cases) / sizeof(cases[0])
                                         << " topologies compared at the verify.py DC tolerance");
}

TEST_CASE("ngspice DC sweep: BJT cutoff, forward active, and saturation",
          "[bjt][ngspice][dc][sweep]") {
    if (!requireNgspice("BJT DC sweep")) return;

    wdf::BjtModel model;
    model.Is = 1e-14;
    model.Bf = 100.0;
    model.Br = 1.0;
    model.Nf = 1.0;
    model.Nr = 1.0;
    model.prepare();
    const std::string card = bjtCoreCard(model);

    const std::vector<double> biases{0.0, 0.1, 0.3, 0.45, 0.55, 0.6, 0.65, 0.7};
    std::vector<std::vector<std::string>> rows;
    int cutoff = 0, forward = 0, saturation = 0;
    double maxNodeErr = 0.0;
    double maxCurrentErr = 0.0;
    for (size_t i = 0; i < biases.size(); ++i) {
        BjtFixture f{{bjtSrc("vcc", "0", 5.0), bjtSrc("b", "0", biases[i]),
                      bjtRes("vcc", "c", 1000.0), bjtNpn("c", "b", "0")},
                     {"c"}};
        const auto p = runBjt(f, model, card, "bjt_sweep_" + std::to_string(i));
        requireBjtMatches(p, "bias " + number(biases[i]));
        maxNodeErr = std::max(maxNodeErr, p.maxNodeErr);
        maxCurrentErr = std::max(maxCurrentErr, p.maxCurrentErr);
        if (p.region == wdf::BjtRegion::Cutoff) ++cutoff;
        else if (p.region == wdf::BjtRegion::ForwardActive) ++forward;
        else if (p.region == wdf::BjtRegion::Saturation) ++saturation;
        appendBjtRows(rows, "core", "base-voltage " + number(biases[i]), p);
    }
    REQUIRE(cutoff > 0);
    REQUIRE(forward > 0);
    REQUIRE(saturation > 0);
    writeCsv("bjt_sweep", kCsvHeader, rows);
    WARN("BJT DC sweep: points=" << biases.size() << " cutoff/forward/sat=" << cutoff << "/"
                                 << forward << "/" << saturation << " maxNode=" << maxNodeErr
                                 << " V maxI=" << maxCurrentErr << " A");
}

TEST_CASE("ngspice DC oracle: BJT preset core parameter mapping",
          "[bjt][ngspice][dc][presets][equation]") {
    if (!requireNgspice("BJT preset core mapping")) return;

    const char* names[] = {"2N3904", "2N2222A", "2N2222", "BC547B", "2N5088"};
    std::vector<std::vector<std::string>> rows;
    double maxNodeErr = 0.0;
    double maxCurrentErr = 0.0;
    for (const char* name : names) {
        const wdf::BjtEntry* entry = wdf::findBjtByName(name);
        REQUIRE(entry != nullptr);
        const wdf::BjtModel model = entry->model;
        REQUIRE(model.valid());
        const auto p = runBjt(bjtCommonEmitter(5.0, 1.0e6, 220.0, 47.0), model,
                              bjtCoreCard(model), std::string("bjt_preset_core_") + name);
        requireBjtMatches(p, name);
        REQUIRE(p.region == wdf::BjtRegion::ForwardActive);
        REQUIRE(p.engIc / p.engIb == Approx(model.Bf).epsilon(1e-4));
        maxNodeErr = std::max(maxNodeErr, p.maxNodeErr);
        maxCurrentErr = std::max(maxCurrentErr, p.maxCurrentErr);
        appendBjtRows(rows, name, "common-emitter", p);
    }
    writeCsv("bjt_preset_core", kCsvHeader, rows);
    WARN("BJT preset core oracle: " << sizeof(names) / sizeof(names[0])
                                    << " NPN presets, maxNode=" << maxNodeErr
                                    << " V maxI=" << maxCurrentErr << " A");
}

TEST_CASE("ngspice DC characterization: BJT LTspice cards vs reduced LiveWDF model",
          "[bjt][ngspice][dc][presets][realism]") {
    if (!requireNgspice("BJT preset realism")) return;

    struct Realism {
        const char* name;
        const char* card;
    };
    const Realism cards[] = {
        {"2N3904", kBjt2N3904},
        {"2N2222", kBjt2N2222},
        {"BC547B", kBjtBC547B},
    };

    std::vector<std::vector<std::string>> rows;
    double maxNodeErr = 0.0;
    double maxCurrentErr = 0.0;
    for (const auto& c : cards) {
        const wdf::BjtEntry* entry = wdf::findBjtByName(c.name);
        REQUIRE(entry != nullptr);
        const auto p = runBjt(bjtCommonEmitter(5.0, 1.0e6, 220.0, 47.0), entry->model, c.card,
                              std::string("bjt_preset_realism_") + c.name, c.name);
        // Characterization only: the full card includes VAF/IKF/ISE/ISC and
        // series resistances that the reduced law does not implement.
        REQUIRE(p.region == wdf::BjtRegion::ForwardActive);
        REQUIRE(std::isfinite(p.ngIc));
        REQUIRE(std::isfinite(p.engIc));
        REQUIRE(p.ngIc > 0.0);
        REQUIRE(p.engIc > 0.0);
        maxNodeErr = std::max(maxNodeErr, p.maxNodeErr);
        maxCurrentErr = std::max(maxCurrentErr, p.maxCurrentErr);
        appendBjtRows(rows, c.name, "common-emitter", p);
    }
    writeCsv("bjt_preset_realism", kCsvHeader, rows);
    WARN("BJT preset realism characterization (not an equality assertion): maxNode="
         << maxNodeErr << " V maxI=" << maxCurrentErr
         << " A. Full LTspice cards include effects absent from the Ebers-Moll law. "
            "PNP polarity is covered by the device mirror and engine tests.");
}

TEST_CASE("ngspice DC oracle: MOSFET editor presets across regions and topologies",
          "[mosfet][ngspice][dc][presets][equation]") {
    if (!requireNgspice("MOSFET preset equation matrix")) return;

    std::vector<std::vector<std::string>> rows;
    double maxNodeErr = 0.0;
    double maxCurrentErr = 0.0;
    int saturation = 0, ohmic = 0;
    int index = 0;
    for (const auto& preset : kMosPresets) {
        const std::string card = mosCard(preset);
        const std::string tag = "mos_preset_" + std::to_string(index++);

        // Saturation: VOV=0.5 V and a small drain load.
        {
            const std::string net = mosCommonSourceNet(10.0, preset.Vth + 0.5, 100.0, "MMOD",
                                                       card);
            const wdf::Circuit c = wdf::buildFromString(net);
            const auto p = runMos(net, tag + "_sat", "d", c);
            requireMosMatches(p, std::string(preset.name) + " saturation");
            REQUIRE(p.region == wdf::MosfetRegion::Saturation);
            maxNodeErr = std::max(maxNodeErr, p.maxNodeErr);
            maxCurrentErr = std::max(maxCurrentErr, p.maxCurrentErr);
            ++saturation;
            appendMosRows(rows, preset.name, "common-source saturation", "d", p);
        }

        // Ohmic: large overdrive with a low drain supply.
        {
            const std::string net = mosCommonSourceNet(0.5, preset.Vth + 2.0, 100.0, "MMOD",
                                                       card);
            const wdf::Circuit c = wdf::buildFromString(net);
            const auto p = runMos(net, tag + "_ohmic", "d", c);
            requireMosMatches(p, std::string(preset.name) + " ohmic");
            REQUIRE(p.region == wdf::MosfetRegion::Ohmic);
            maxNodeErr = std::max(maxNodeErr, p.maxNodeErr);
            maxCurrentErr = std::max(maxCurrentErr, p.maxCurrentErr);
            ++ohmic;
            appendMosRows(rows, preset.name, "common-source ohmic", "d", p);
        }

        // Source follower: body tied to source, as the LiveWDF model requires.
        {
            const std::string net =
                mosSourceFollowerNet(10.0, preset.Vth + 2.0, 1000.0, "MMOD", card);
            const wdf::Circuit c = wdf::buildFromString(net);
            const auto p = runMos(net, tag + "_follower", "s", c);
            requireMosMatches(p, std::string(preset.name) + " source follower");
            REQUIRE(p.region == wdf::MosfetRegion::Saturation);
            maxNodeErr = std::max(maxNodeErr, p.maxNodeErr);
            maxCurrentErr = std::max(maxCurrentErr, p.maxCurrentErr);
            appendMosRows(rows, preset.name, "source-follower", "s", p);
        }
    }
    REQUIRE(saturation == static_cast<int>(sizeof(kMosPresets) / sizeof(kMosPresets[0])));
    REQUIRE(ohmic == static_cast<int>(sizeof(kMosPresets) / sizeof(kMosPresets[0])));
    writeCsv("mosfet_equation", kCsvHeader, rows);
    WARN("MOSFET preset equation oracle: presets="
         << sizeof(kMosPresets) / sizeof(kMosPresets[0]) << " topologies=3 maxNode="
         << maxNodeErr << " V maxI=" << maxCurrentErr << " A");
}

TEST_CASE("ngspice DC characterization: MOSFET LTspice cards vs editor preset fits",
          "[mosfet][ngspice][dc][presets][realism]") {
    if (!requireNgspice("MOSFET preset realism")) return;

    struct Realism {
        const char* name;
        const char* card;
        double K;
        double Vth;
        double lambda;
    };
    const Realism cards[] = {
        {"BS170", kMosBs170, 20.0e-3, 2.1, 0.01},
        {"2N7002", kMos2N7002, 20.0e-3, 2.0, 0.01},
    };

    std::vector<std::vector<std::string>> rows;
    for (const auto& r : cards) {
        const double vdd = 10.0;
        const double vg = 3.0;
        const double rd = 1000.0;
        const std::string net =
            mosCommonSourceNet(vdd, vg, rd, r.name, r.card, false);
        const auto run = runDeck(
            net + controlBlock({"op"}, {"v(d)", "@M1[id]"}, std::string("mos_realism_") + r.name) +
                ".end\n",
            std::string("mos_realism_") + r.name);
        REQUIRE(run.ok);
        const auto ngV = parseOpFile(vecFile(std::string("mos_realism_") + r.name, 0));
        const auto ngI = parseOpFile(vecFile(std::string("mos_realism_") + r.name, 1));
        REQUIRE(ngV.size() == 1);
        REQUIRE(ngI.size() == 1);

        wdf::Circuit c;
        c.addSrc("vdd", "0", vdd);
        c.addSrc("g", "0", vg);
        c.addRes("vdd", "d", rd);
        c.addMosfet("d", "g", "0", wdf::MosfetModel::fromK(r.K, r.Vth, r.lambda), "M1");
        wdf::Engine e(c, kFs);
        std::vector<double> node, rootV;
        REQUIRE(e.solveDc(node, rootV));
        const int dIdx = e.nodeIndex("d");
        REQUIRE(dIdx >= 0);
        std::vector<double> current, jac;
        e.currents(rootV, current, jac);
        REQUIRE(current.size() == 2);

        const double engV = node[static_cast<size_t>(dIdx)];
        const double engI = current[0];
        REQUIRE(std::isfinite(ngV[0]));
        REQUIRE(std::isfinite(ngI[0]));
        REQUIRE(std::isfinite(engV));
        REQUIRE(std::isfinite(engI));
        REQUIRE(ngI[0] > 0.0);
        REQUIRE(engI > 0.0);
        REQUIRE(engV >= 0.0);
        REQUIRE(engV <= vdd);

        const auto region = c.mosfet[0].model.evaluate(rootV[1], rootV[0]).region;
        REQUIRE(region != wdf::MosfetRegion::Invalid);
        rows.push_back(row("MOSFET", r.name, "common-source VG=3V", regionName(region), "v(d)",
                           ngV[0], engV));
        rows.push_back(row("MOSFET", r.name, "common-source VG=3V", regionName(region), "iD",
                           ngI[0], engI));
        WARN("MOSFET preset realism " << r.name << ": ng d/Id=" << ngV[0] << "/" << ngI[0]
                                      << " livewdf d/Id=" << engV << "/" << engI
                                      << " (full card vs fitted Level-1 approximation)");
    }
    writeCsv("mosfet_preset_realism", kCsvHeader, rows);
    WARN("MOSFET preset realism is characterization only: the reference cards are Level-3/VDMOS "
         "with geometry, capacitances and subthreshold/body behavior the reduced law does not "
         "implement.");
}

TEST_CASE("ngspice DC oracle: JFET presets across common-source, self-bias, and follower",
          "[jfet][ngspice][dc][presets][equation]") {
    if (!requireNgspice("JFET preset equation matrix")) return;

    const char* names[] = {"2N5952", "2N5459", "LSK170A", "LSK189A"};
    std::vector<std::vector<std::string>> rows;
    double maxNodeErr = 0.0;
    double maxCurrentErr = 0.0;
    int points = 0;
    for (const char* name : names) {
        const wdf::JfetEntry* entry = wdf::findJfetByName(name);
        REQUIRE(entry != nullptr);
        const wdf::JfetModel model = entry->model;
        REQUIRE(model.valid());
        const std::string card = jfetCoreCard(model);
        const std::string base = std::string("jfet_eq_") + name + "_";

        // Common-source, gate at half pinch-off.
        {
            const std::string net =
                jfetCommonSourceNet(10.0, 0.5 * model.p.Vp, 1000.0, "JMOD", card);
            const wdf::Circuit c = wdf::buildFromString(net);
            const auto p = runJfet(net, base + "cs", {"d"}, c);
            requireJfetMatches(p, std::string(name) + " common-source");
            REQUIRE(p.region == wdf::JfetRegion::Saturation);
            maxNodeErr = std::max(maxNodeErr, p.maxNodeErr);
            maxCurrentErr = std::max(maxCurrentErr, p.maxCurrentErr);
            appendJfetRows(rows, name, "common-source", p);
            ++points;
        }
        // Self-bias (source degeneration and gate returned to ground).
        {
            const std::string net = jfetSelfBiasNet(15.0, 2200.0, 1000.0, "JMOD", card);
            const wdf::Circuit c = wdf::buildFromString(net);
            const auto p = runJfet(net, base + "self", {"d", "s"}, c);
            requireJfetMatches(p, std::string(name) + " self-bias");
            REQUIRE(p.region == wdf::JfetRegion::Saturation);
            maxNodeErr = std::max(maxNodeErr, p.maxNodeErr);
            maxCurrentErr = std::max(maxCurrentErr, p.maxCurrentErr);
            appendJfetRows(rows, name, "self-bias", p);
            ++points;
        }
        // Source follower.
        {
            const std::string net =
                jfetSourceFollowerNet(15.0, 0.5 * model.p.Vp, 1000.0, "JMOD", card);
            const wdf::Circuit c = wdf::buildFromString(net);
            const auto p = runJfet(net, base + "follower", {"s"}, c);
            requireJfetMatches(p, std::string(name) + " source-follower");
            REQUIRE(p.region == wdf::JfetRegion::Saturation);
            maxNodeErr = std::max(maxNodeErr, p.maxNodeErr);
            maxCurrentErr = std::max(maxCurrentErr, p.maxCurrentErr);
            appendJfetRows(rows, name, "source-follower", p);
            ++points;
        }
    }
    writeCsv("jfet_equation", kCsvHeader, rows);
    WARN("JFET preset equation oracle: points=" << points << " maxNode=" << maxNodeErr
                                                << " V maxI=" << maxCurrentErr << " A");
}

TEST_CASE("ngspice DC characterization: JFET LTspice cards vs built-in presets",
          "[jfet][ngspice][dc][presets][realism]") {
    if (!requireNgspice("JFET preset realism")) return;

    struct Realism {
        const char* name;
        const char* card;
    };
    const Realism cards[] = {
        {"LSK170A", kJfetLsk170A}, {"LSK170B", kJfetLsk170B}, {"LSK170C", kJfetLsk170C},
        {"LSK189A", kJfetLsk189A}, {"LSK189B", kJfetLsk189B}, {"LSK189C", kJfetLsk189C},
    };

    std::vector<std::vector<std::string>> rows;
    double maxNodeErr = 0.0;
    double maxCurrentErr = 0.0;
    for (const auto& r : cards) {
        const wdf::JfetEntry* entry = wdf::findJfetByName(r.name);
        REQUIRE(entry != nullptr);
        const wdf::JfetModel model = entry->model;
        const std::string topology = jfetCommonSourceNet(
            10.0, 0.5 * model.p.Vp, 1000.0, r.name, "");
        const wdf::Circuit c = wdf::buildFromString(topology);
        const std::string deckNet = topology + r.card + "\n";
        const auto p = runJfet(deckNet, std::string("jfet_realism_") + r.name, {"d"}, c);
        REQUIRE(p.region == wdf::JfetRegion::Saturation);
        REQUIRE(p.ngId > 0.0);
        REQUIRE(p.engId > 0.0);
        maxNodeErr = std::max(maxNodeErr, p.maxNodeErr);
        maxCurrentErr = std::max(maxCurrentErr, p.maxCurrentErr);
        appendJfetRows(rows, r.name, "common-source", p);
    }
    writeCsv("jfet_preset_realism", kCsvHeader, rows);
    WARN("JFET preset realism characterization (not an equality assertion): maxNode="
         << maxNodeErr << " V maxI=" << maxCurrentErr
         << " A. The built-in LSK170 parameter sets were transcribed from a different library "
            "revision than the embedded standard.jft cards; the difference is reported, not "
            "tuned away.");
}
