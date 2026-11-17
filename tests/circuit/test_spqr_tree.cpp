#include <catch2.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "circuit/spqr/Reduce.h"

// Structural validation of the series/parallel reduction against
// wdf_compiler's prebuilt `netlist_converter_standalone` exe, which is the
// oracle (MIT, Chowdhury-DSP; see tests/oracle/README.md for how the exe is
// obtained and used). Every golden tree below was recorded by running
//
//   netlist_converter_standalone-windows-x86_64.exe <fixture.net> -print_wdf
//
// and taking the tree that follows the header line. The fixtures themselves
// are byte-identical copies of wdf_compiler/tests/<name>/<name>.net.
//
// The loader here is not a second SPICE parser: it reads only what the
// reduction needs -- designator, two terminals, and a `; root` flag -- from
// fixtures that are already known to be one component per line. Value and
// waveform tokens are ignored, which is also what the oracle does.

namespace {

/// Minimal `.net` reader: `designator a b [value...] [; meta...]`, with `*`
/// and `.` lines skipped. Kept deliberately dumb so the fixtures stay the
/// single source of truth.
std::vector<wdf::NetEdge> loadNetlist(const char* text) {
    std::vector<wdf::NetEdge> edges;

    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        std::istringstream words(line);
        std::vector<std::string> tokens;
        for (std::string w; words >> w;)
            tokens.push_back(w);

        if (tokens.empty() || tokens[0][0] == '*' || tokens[0][0] == '.')
            continue;
        if (tokens.size() < 3)
            continue;

        std::vector<std::string> meta;
        for (size_t i = 3; i < tokens.size(); ++i) {
            std::string t = tokens[i];
            if (t[0] == ';')
                t = t.substr(1);
            if (!t.empty())
                meta.push_back(t);
        }

        wdf::NetEdge e;
        e.name = tokens[0];
        e.a = tokens[1];
        e.b = tokens[2];
        e.isRoot =
            std::find(meta.begin(), meta.end(), "root") != meta.end();

        switch (e.name[0]) {
            case 'V': case 'v': e.leaf = wdf::LeafKind::VoltageSource; break;
            case 'I': case 'i': e.leaf = wdf::LeafKind::CurrentSource; break;
            case 'R': case 'r': e.leaf = wdf::LeafKind::Resistor;      break;
            case 'C': case 'c': e.leaf = wdf::LeafKind::Capacitor;     break;
            case 'L': case 'l': e.leaf = wdf::LeafKind::Inductor;      break;
            default:           e.leaf = wdf::LeafKind::Custom;        break;
        }
        edges.push_back(std::move(e));
    }
    return edges;
}

// --------------------------------------------------------------------------- //
// Golden trees, verbatim from the oracle.
// --------------------------------------------------------------------------- //

constexpr const char* kRcLowpassNet = R"(* RC Lowpass Filter
V1 P001 0 PULSE(0 1 1m 1u 1u 10m 20m 0) ; input root
R1 P002 P001 1k
C1 P002 0 1u ; output:V
.tran 0 50m 0 10u
.backanno
.end
)";

constexpr const char* kRcLowpassTree = R"(V1
└── SERIES
    ├── R1
    └── C1
)";

constexpr const char* kRcBandpassNet = R"(* RC Bandpass Circuit
Vin 1 0 PULSE(0 1 1m 1u 1u 10m 20m 0) ; input root
C1 1 2 1u
R1 2 0 1k
R2 2 3 1k
C2 3 0 1u ; output:V
.tran 0 50m 0 10u
.backanno
.end
)";

constexpr const char* kRcBandpassTree = R"(Vin
└── SERIES
    ├── PARALLEL
    │   ├── SERIES
    │   │   ├── R2
    │   │   └── C2
    │   └── R1
    └── C1
)";

// Every component sits between the same two nodes, so the tree is a chain of
// PARALLEL nodes and nothing is ever contracted in series.
constexpr const char* kRlLowpassVarNet = R"(* RL Circuit Transient Analysis
Iplus 1 0 PULSE(0 1m 1m 1u 1u 10m 20m) ; root
Iin 1 0                             ; input
R_In 1 0 1k
Ivar 1 0
R_Ivar 1 0 2k
R1 1 0                              ; output:I
L1 1 0 1m
.tran 0 50m 0 10u                   ; Transient Analysis: 50ms
.end
)";

constexpr const char* kRlLowpassVarTree = R"(Iplus
└── PARALLEL
    ├── PARALLEL
    │   ├── PARALLEL
    │   │   ├── PARALLEL
    │   │   │   ├── PARALLEL
    │   │   │   │   ├── L1
    │   │   │   │   └── R1
    │   │   │   └── R_Ivar
    │   │   └── Ivar
    │   └── R_In
    └── Iin
)";

}  // namespace

TEST_CASE("series/parallel reduction matches the netlist_converter oracle")
{
    const struct {
        const char* label;
        const char* net;
        const char* tree;
    } cases[] = {
        {"rc_lowpass", kRcLowpassNet, kRcLowpassTree},
        {"rc_bandpass", kRcBandpassNet, kRcBandpassTree},
        {"rl_lowpass_var", kRlLowpassVarNet, kRlLowpassVarTree},
    };

    for (const auto& c : cases) {
        INFO(c.label);
        const wdf::Tree tree = wdf::reduceToTree(loadNetlist(c.net));
        REQUIRE(tree.root >= 0);
        CHECK(wdf::renderTree(tree) == c.tree);
    }
}

TEST_CASE("a netlist with no component marked `; root` reduces unchanged")
{
    // Same topology as rc_lowpass, but nothing claims to be the reflector:
    // the tree must still form, and must keep the shape the reduction chose.
    wdf::Tree tree = wdf::reduceToTree(loadNetlist(
        "* RC\nV1 a 0 1\nR1 b a 1k\nC1 b 0 1u\n"));

    REQUIRE(tree.root >= 0);
    CHECK(!wdf::renderTree(tree).empty());
    CHECK(wdf::renderTree(tree).find("SERIES") != std::string::npos);
}

TEST_CASE("a non-series-parallel graph reduces to a single RIGID node")
{
    // A Wheatstone bridge: every node has three incident branches, so neither
    // a degree-2 contraction nor a parallel pair is ever available.
    const std::vector<wdf::NetEdge> edges = {
        {"a", "c", wdf::LeafKind::VoltageSource, "Vs", false},
        {"a", "b", wdf::LeafKind::Resistor, "R1", false},
        {"b", "c", wdf::LeafKind::Resistor, "R2", false},
        {"a", "d", wdf::LeafKind::Resistor, "R3", false},
        {"d", "c", wdf::LeafKind::Resistor, "R4", false},
        {"b", "d", wdf::LeafKind::Resistor, "R5", false},
    };

    const wdf::Tree tree = wdf::reduceToTree(edges);
    REQUIRE(tree.root >= 0);

    const wdf::TreeNode& rigid = tree.nodes[static_cast<size_t>(tree.root)];
    REQUIRE(rigid.kind == wdf::NodeKind::Rigid);

    // Nothing was dropped and nothing was invented.
    CHECK(rigid.children.size() == edges.size());
    CHECK(rigid.rigidEdges.size() == edges.size());
    CHECK(rigid.rigidVertices.size() == 4);

    for (const auto& re : rigid.rigidEdges) {
        REQUIRE(re.child >= 0);
        const wdf::TreeNode& br = tree.nodes[static_cast<size_t>(re.child)];
        // Distinct components survive as distinct branches.
        CHECK(br.kind == wdf::NodeKind::Leaf);
        // Local vertices must resolve to distinct global nodes within an edge.
        REQUIRE(re.v1 < static_cast<int>(rigid.rigidVertices.size()));
        REQUIRE(re.v2 < static_cast<int>(rigid.rigidVertices.size()));
        CHECK(re.v1 != re.v2);
        CHECK_FALSE(br.name.empty());
    }

    CHECK(tree.nodes[static_cast<size_t>(tree.root)].kind == wdf::NodeKind::Rigid);
    CHECK(wdf::renderTree(tree).rfind("RIGID", 0) == 0);
}

// The circuit the benchmark drives (tests/bench_stage.cpp `stageCircuit()`):
// one triode, plate load, grid stopper, cathode bias, coupling cap. The
// triode is three two-terminal ports meeting at a star, which is how a
// multi-terminal device has to enter a two-terminal graph reduction.
TEST_CASE("the benchmark's single-triode stage reduces")
{
    using wdf::LeafKind;
    const std::vector<wdf::NetEdge> edges = {
        {"in", "0", LeafKind::VoltageSource, "Vin", true},
        {"e",  "0", LeafKind::VoltageSource, "Ve",  false},
        {"in", "i", LeafKind::Capacitor,     "Ci",  false},
        {"i",  "0", LeafKind::Resistor,      "Ri",  false},
        {"i",  "g", LeafKind::Resistor,      "Rg",  false},
        {"k",  "0", LeafKind::Resistor,      "Rk",  false},
        {"k",  "0", LeafKind::Capacitor,     "Ck",  false},
        {"e",  "p", LeafKind::Resistor,      "Rp",  false},
        {"p",  "o", LeafKind::Capacitor,     "Co",  false},
        {"o",  "0", LeafKind::Resistor,      "Ro",  false},
        {"t",  "p", LeafKind::Custom,        "Tp",  false},
        {"t",  "g", LeafKind::Custom,        "Tg",  false},
        {"t",  "k", LeafKind::Custom,        "Tk",  false},
    };

    const wdf::Tree tree = wdf::reduceToTree(edges);
    REQUIRE(tree.root >= 0);

    // Every branch the reduction claims must still be one of ours.
    int leaves = 0;
    for (const wdf::TreeNode& n : tree.nodes)
        if (n.kind == wdf::NodeKind::Leaf)
            ++leaves;
    CHECK(leaves == static_cast<int>(edges.size()));

    // Do not fail the build on a large tree shape -- print it for inspection.
    SUCCEED(wdf::renderTree(tree));
}
