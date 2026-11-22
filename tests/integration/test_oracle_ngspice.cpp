#include <catch2.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "support/NgspiceOracle.h"
#include "wdf/Engine.h"
#include "spice/SpiceNet.h"

// Phase 1 increment 3: ngspice golden-reference checks (tools/verify.py
// methodology in-process). Thresholds borrowed from verify.py: DC <= 5e-4 V;
// time-domain pointwise agreement on a 1 V-scale signal.

using lwdf_test::controlBlock;
using lwdf_test::parseOpFile;
using lwdf_test::parseTranFile;
using lwdf_test::runDeck;
using lwdf_test::TranTable;
using lwdf_test::vecFile;

namespace {
constexpr double kFs = 48000.0;
}  // namespace

// An absent external simulator is a skip, not a failure of the offline suite.
#define LWDF_REQUIRE_NGSPICE(what)                    \
    do {                                              \
        if (!lwdf_test::requireNgspice(what)) return; \
    } while (0)

TEST_CASE("ngspice DC oracle: resistive divider + RC", "[phase1][ngspice][dc]") {
    LWDF_REQUIRE_NGSPICE("resistive divider DC oracle");
    const std::string deck =
        "* livewdf dc oracle\n"
        "V1 in 0 DC 5\n"
        "R1 in out 1k\n"
        "R2 out 0 1k\n"
        "C1 out 0 1u\n" +
        controlBlock({"op"}, {"v(out)"}, "dc1") + ".end\n";

    const auto run = runDeck(deck, "dc1");
    REQUIRE(run.ok);
    const double ngOut = parseOpFile(vecFile("dc1", 0)).front();

    wdf::Circuit c = wdf::buildFromString(
        "V1 in 0 DC 5\nR1 in out 1k\nR2 out 0 1k\nC1 out 0 1u\n");
    wdf::Engine e(c, kFs);
    std::vector<double> node, rootV;
    REQUIRE(e.solveDc(node, rootV));
    const double engOut = node[static_cast<size_t>(e.nodeIndex("out"))];

    // verify.py DC threshold
    REQUIRE(std::abs(ngOut - engOut) <= 5e-4);
}

TEST_CASE("ngspice DC oracle: 2N5952 common-source stage", "[jfet][ngspice][dc]") {
    LWDF_REQUIRE_NGSPICE("2N5952 common-source DC oracle");
    // The same card and topology as the engine fixture. The JFET law is the
    // paper's simplified square law, which is also SPICE's Level-1 JFET law
    // after `Is0 = Beta * Vto^2`; this checks the parameter mapping rather than
    // only the isolated equation.
    const std::string model =
        ".model J2N5952 NJF(Beta=1.314m Vto=-2.021 Lambda=4m Rd=1 Rs=1 "
        "Cgd=1.6p Cgs=2.414p)\n";
    // Keep the gate-source junction reverse-biased. The selected law has zero
    // gate current; at Vgs near +0.6 V the real SPICE junction starts clamping
    // the gate through RG, which is outside this model's stated assumptions.
    const std::vector<double> biases{-3.0, -1.5, -0.5, 0.3};
    double maxErr = 0.0;
    for (const double bias : biases) {
        char input[64];
        std::snprintf(input, sizeof input, "VIN in 0 DC %.17g\n", bias);
        const std::string net = std::string("* livewdf jfet dc oracle\nVDD vdd 0 DC 15\n") + input +
                                "RIN in g 1k\n"
                                "RG g 0 1meg\n"
                                "RGATE g 0 1e9\n"
                                "RD vdd d 2.2k\n"
                                "J1 d g 0 J2N5952\n" +
                                model;
        const std::string tag = "jfet_dc_" + std::to_string(static_cast<int>(bias * 10 + 30));
        const auto run = runDeck(net + controlBlock({"op"}, {"v(d)", "v(g)"}, tag) + ".end\n", tag);
        REQUIRE(run.ok);
        const auto ngD = parseOpFile(vecFile(tag, 0));
        const auto ngG = parseOpFile(vecFile(tag, 1));
        REQUIRE(ngD.size() == 1);
        REQUIRE(ngG.size() == 1);

        wdf::Circuit c = wdf::buildFromString(net);
        wdf::Engine e(c, kFs);
        std::vector<double> node, rootV;
        REQUIRE(e.solveDc(node, rootV));
        const double engD = node[static_cast<size_t>(e.nodeIndex("d"))];
        const double engG = node[static_cast<size_t>(e.nodeIndex("g"))];
        INFO("bias " << bias << " ng d/g " << ngD[0] << "/" << ngG[0] << " eng d/g " << engD
                     << "/" << engG);
        maxErr = std::max({maxErr, std::abs(ngD[0] - engD), std::abs(ngG[0] - engG)});
        REQUIRE(std::abs(ngD[0] - engD) <= 5e-4);
        REQUIRE(std::abs(ngG[0] - engG) <= 5e-4);
    }
    WARN("ngspice JFET DC oracle: maxErr=" << maxErr << " V over " << biases.size()
                                            << " bias points");
}

TEST_CASE("ngspice DC sweep: 2N5952 cutoff, ohmic, saturation, and boundaries",
          "[jfet][ngspice][dc][sweep]") {
    LWDF_REQUIRE_NGSPICE("2N5952 DC sweep");
    // Same common-source topology and card as the point oracle above. The grid
    // crosses the cut-off boundary (VGS = Vto = -2.021 V) and the
    // ohmic/saturation boundary at several drain supplies. The first deck line
    // is a title in ngspice, so the actual circuit starts on line 2.
    const std::string model =
        ".model J2N5952 NJF(Beta=1.314m Vto=-2.021 Lambda=4m Rd=1 Rs=1 "
        "Cgd=1.6p Cgs=2.414p)\n";
    const std::vector<double> vdds{0.5, 1.0, 1.3, 2.0, 5.0, 8.25, 15.0};
    const std::vector<double> vgss{-2.03, -2.021, -1.5, -0.5, 0.2};

    double maxNodeErr = 0.0;
    double maxCurrentErr = 0.0;
    double sumNodeSq = 0.0;
    double sumCurrentSq = 0.0;
    double sumNodeAbs = 0.0;
    double sumCurrentAbs = 0.0;
    int points = 0;
    int cutoff = 0, ohmic = 0, saturation = 0;
    int index = 0;
    for (double vdd : vdds) {
        for (double vgs : vgss) {
            char supply[64], gate[64];
            std::snprintf(supply, sizeof supply, "VDD vdd 0 DC %.17g\n", vdd);
            std::snprintf(gate, sizeof gate, "VIN in 0 DC %.17g\n", vgs);
            const std::string net = std::string("* livewdf jfet dc sweep\n") + supply + gate +
                                    "RIN in g 1k\n"
                                    "RG g 0 1meg\n"
                                    "RGATE g 0 1e9\n"
                                    "RD vdd d 2.2k\n"
                                    "J1 d g 0 J2N5952\n" +
                                    model;
            const std::string tag = "jfet_sweep_" + std::to_string(index++);
            const auto run = runDeck(net + controlBlock({"op"}, {"v(d)", "v(g)"}, tag) + ".end\n", tag);
            REQUIRE(run.ok);
            const auto ngD = parseOpFile(vecFile(tag, 0));
            const auto ngG = parseOpFile(vecFile(tag, 1));
            REQUIRE(ngD.size() == 1);
            REQUIRE(ngG.size() == 1);

            wdf::Circuit c = wdf::buildFromString(net);
            wdf::Engine e(c, kFs);
            std::vector<double> node, rootV;
            REQUIRE(e.solveDc(node, rootV));
            const double engD = node[static_cast<size_t>(e.nodeIndex("d"))];
            const double engG = node[static_cast<size_t>(e.nodeIndex("g"))];
            const double ngCurrent = (vdd - ngD[0]) / 2200.0;
            const double engCurrent = (vdd - engD) / 2200.0;
            const double nodeErr = std::max(std::abs(ngD[0] - engD), std::abs(ngG[0] - engG));
            const double currentErr = std::abs(ngCurrent - engCurrent);
            INFO("VDD=" << vdd << " VGS=" << vgs << " ng d/g=" << ngD[0] << "/" << ngG[0]
                         << " eng d/g=" << engD << "/" << engG);
            maxNodeErr = std::max(maxNodeErr, nodeErr);
            maxCurrentErr = std::max(maxCurrentErr, currentErr);
            sumNodeSq += nodeErr * nodeErr;
            sumCurrentSq += currentErr * currentErr;
            sumNodeAbs += nodeErr;
            sumCurrentAbs += currentErr;
            ++points;

            const auto ev = c.jfet[0].model.evaluate(rootV[1], rootV[0]);
            REQUIRE(ev.valid);
            if (ev.region == wdf::JfetRegion::Cutoff)
                ++cutoff;
            else if (ev.region == wdf::JfetRegion::Ohmic)
                ++ohmic;
            else if (ev.region == wdf::JfetRegion::Saturation)
                ++saturation;

            REQUIRE(nodeErr <= 5e-4);
            REQUIRE(currentErr <= 1e-6);
        }
    }
    REQUIRE(cutoff > 0);
    REQUIRE(ohmic > 0);
    REQUIRE(saturation > 0);
    const double nodeRms = std::sqrt(sumNodeSq / static_cast<double>(points));
    const double currentRms = std::sqrt(sumCurrentSq / static_cast<double>(points));
    const double nodeMean = sumNodeAbs / static_cast<double>(points);
    const double currentMean = sumCurrentAbs / static_cast<double>(points);
    WARN("ngspice JFET DC sweep: points=" << points << " cutoff/ohmic/sat=" << cutoff << "/"
                                           << ohmic << "/" << saturation << " maxNode="
                                           << maxNodeErr << " V rmsNode=" << nodeRms
                                           << " V meanNode=" << nodeMean << " V maxI="
                                           << maxCurrentErr << " A rmsI=" << currentRms
                                           << " A meanI=" << currentMean << " A");
}

TEST_CASE("ngspice DC oracle: 2N5952 reverse-biased common-source stage",
          "[jfet][ngspice][dc][reverse]") {
    LWDF_REQUIRE_NGSPICE("2N5952 reverse DC oracle");
    // Source at +5 V, drain returned to ground through 1 k, gate at +3 V. The
    // JFET is reverse-biased (VDS < 0) but both junctions remain reverse-biased.
    // This checks the source/drain-reversed extension against ngspice's
    // symmetric JFET model, including the card's Rd/Rs/Lambda.
    const std::string model =
        ".model J2N5952 NJF(Beta=1.314m Vto=-2.021 Lambda=4m Rd=1 Rs=1 "
        "Cgd=1.6p Cgs=2.414p)\n";
    double maxNodeErr = 0.0;
    double maxCurrentErr = 0.0;
    int points = 0;
    for (double gateBias : {2.9, 3.0, 3.1, 3.2}) {
        char gate[64];
        std::snprintf(gate, sizeof gate, "VG g 0 DC %.17g\n", gateBias);
        const std::string net = std::string("* livewdf jfet reverse dc oracle\n"
                                            "VDD s 0 DC 5\n"
                                            "RD d 0 1k\n") +
                                gate + "J1 d g s J2N5952\n" + model;
        const std::string tag = "jfet_reverse_" + std::to_string(points);
        const auto run = runDeck(net + controlBlock({"op"}, {"v(d)", "v(g)", "v(s)"}, tag) + ".end\n", tag);
        REQUIRE(run.ok);
        const auto ngD = parseOpFile(vecFile(tag, 0));
        const auto ngG = parseOpFile(vecFile(tag, 1));
        const auto ngS = parseOpFile(vecFile(tag, 2));
        REQUIRE(ngD.size() == 1);
        REQUIRE(ngG.size() == 1);
        REQUIRE(ngS.size() == 1);

        wdf::Circuit c = wdf::buildFromString(net);
        wdf::Engine e(c, kFs);
        std::vector<double> node, rootV;
        REQUIRE(e.solveDc(node, rootV));
        REQUIRE(e.nodeIndex("d") >= 0);
        std::vector<double> current, jac;
        e.currents(rootV, current, jac);
        REQUIRE(current.size() == 2);
        const double engD = node[static_cast<size_t>(e.nodeIndex("d"))];
        const double ngVds = ngD[0] - ngS[0];
        const double ngVgs = ngG[0] - ngS[0];
        // RD returns the drain to ground. Its current is +Vd/RD; the JFET's
        // drain-to-source current is the negative of that (KCL at the drain).
        const double ngCurrent = -ngD[0] / 1000.0;
        // The card has Rd = Rs = 1 ohm. The engine's rootV is the intrinsic
        // port voltage; add the series drops before comparing with the external
        // ngspice node voltages.
        const double engVdsExt = rootV[0] + current[0] * (1.0 + 1.0);
        const double engVgsExt = rootV[1] + current[0] * 1.0;
        INFO("VG=" << gateBias << " ng d/Vds/Vgs/I=" << ngD[0] << "/" << ngVds << "/"
                    << ngVgs << "/" << ngCurrent << " eng d/Vds/Vgs/I=" << engD << "/"
                    << engVdsExt << "/" << engVgsExt << "/" << current[0]);
        maxNodeErr = std::max({maxNodeErr, std::abs(ngD[0] - engD),
                               std::abs(ngVds - engVdsExt),
                               std::abs(ngVgs - engVgsExt)});
        maxCurrentErr = std::max(maxCurrentErr, std::abs(ngCurrent - current[0]));
        REQUIRE(std::abs(ngD[0] - engD) <= 5e-4);
        REQUIRE(std::abs(ngVds - engVdsExt) <= 5e-4);
        REQUIRE(std::abs(ngVgs - engVgsExt) <= 5e-4);
        REQUIRE(std::abs(ngCurrent - current[0]) <= 5e-7);
        REQUIRE(rootV[0] < 0.0);
        ++points;
    }
    WARN("ngspice JFET reverse DC oracle: maxNodeErr=" << maxNodeErr << " V maxI="
                                                       << maxCurrentErr << " A over " << points
                                                       << " gate-bias points");
}

TEST_CASE("ngspice DC oracle: representative built-in JFET presets",
          "[jfet][ngspice][dc][presets]") {
    LWDF_REQUIRE_NGSPICE("built-in JFET preset DC oracle");
    // The source cards are the electrical contents of the LTspice
    // `standard.jft` entries. `Mfg` is manufacturer metadata and is omitted
    // because ngspice rejects it as an unknown parameter; no electrical
    // parameter is removed. LiveWDF uses the built-in preset, which stores only
    // the subset its simplified law reads. This is a small parameter-mapping
    // check, not a claim of general model accuracy.
    struct PresetOracle {
        const char* name;
        const char* card;
        double vto;
    };
    const PresetOracle models[] = {
        {"J201",
         ".model J201 NJF(Beta=1.07m Betatce=-0.5 Vto=-0.93 Vtotc=-2.5m Lambda=6.75m "
         "Is=1.72f Xti=0 Rd=10 Rs=12 Cgd=1p Cgs=1p Fc=0.5 M=500m Pb=0.8 Kf=0 Af=1)\n",
         -0.93},
        {"2SK170",
         ".MODEL 2sk170 NJF(Beta=59.86m Rs=4.151 Rd=4.151 Betatce=-500m Lambda=1.923m "
         "Vto=-.5024 Vtotc=-2.5m Cgd=20p M=.3805 Pb=.4746 Fc=.5 Cgs=25.48p "
         "Isr=84.77p Nr=2 Is=8.477p N=1 Xti=3 Alpha=10u Vk=100 Kf=111.3E-18 Af=1)\n",
         -0.5024},
        {"J111",
         ".model J111 NJF(Beta=2.91m Betatce=-0.5 Vto=-4.047 Vtotc=-2.5m Lambda=17.5m "
         "Is=205.2f Xti=3 Isr=1988f Nr=2 Alpha=20.98u N=1 Rd=1 Rs=1 Cgd=6.46p "
         "Cgs=5.74p Fc=0.5 Vk=123.7 M=407m Pb=1 Kf=37860f Af=1)\n",
         -4.047},
        {"2N3819",
         ".model 2N3819 NJF(Beta=1.304m Betatce=-.5 Rd=1 Rs=1 Lambda=2.25m Vto=-3 "
         "Vtotc=-2.5m Is=33.57f Isr=322.4f N=1 Nr=2 Xti=3 Alpha=311.7u Vk=243.6 "
         "Cgd=1.6p M=.3622 Pb=1 Fc=.5 Cgs=2.414p Kf=9.882E-18 Af=1)\n",
         -3.0},
        {"J202",
         ".model J202 NJF(Beta=0.88m Betatce=-0.5 Vto=-1.81 Vtotc=-2.5m Lambda=8.47m "
         "Is=2.56f Xti=0 Rd=11 Rs=13 Cgd=1p Cgs=1p Fc=0.5 M=500m Pb=0.8 Kf=0 Af=1)\n",
         -1.81},
    };

    struct BiasPoint {
        double vdd;
        double vgs;
    };

    double maxNodeErr = 0.0;
    double maxCurrentErr = 0.0;
    int points = 0;
    int modelIndex = 0;
    for (const auto& model : models) {
        const BiasPoint biases[] = {{15.0, 0.5 * model.vto}, {1.0, 0.0}};
        int pointIndex = 0;
        for (const auto& bias : biases) {
            char supply[64], gate[64];
            std::snprintf(supply, sizeof supply, "VDD vdd 0 DC %.17g\n", bias.vdd);
            std::snprintf(gate, sizeof gate, "VIN in 0 DC %.17g\n", bias.vgs);
            const std::string topology =
                std::string("* livewdf JFET preset oracle\n") + supply + gate +
                "RIN in g 1k\n"
                "RG g 0 1meg\n"
                "RD vdd d 100\n"
                "J1 d g 0 " + model.name + "\n";
            const std::string tag = "jfet_preset_" + std::to_string(modelIndex) + "_" +
                                    std::to_string(pointIndex++);
            const auto run =
                runDeck(topology + model.card + controlBlock({"op"}, {"v(d)", "v(g)"}, tag) +
                            ".end\n",
                        tag);
            REQUIRE(run.ok);
            const auto ngD = parseOpFile(vecFile(tag, 0));
            const auto ngG = parseOpFile(vecFile(tag, 1));
            REQUIRE(ngD.size() == 1);
            REQUIRE(ngG.size() == 1);

            const wdf::Circuit c = wdf::buildFromString(topology);
            wdf::Engine e(c, kFs);
            std::vector<double> node, rootV;
            REQUIRE(e.solveDc(node, rootV));
            const double engD = node[static_cast<size_t>(e.nodeIndex("d"))];
            const double engG = node[static_cast<size_t>(e.nodeIndex("g"))];
            const double nodeErr =
                std::max(std::abs(ngD[0] - engD), std::abs(ngG[0] - engG));
            const double ngCurrent = (bias.vdd - ngD[0]) / 100.0;
            const double engCurrent = (bias.vdd - engD) / 100.0;
            const double currentErr = std::abs(ngCurrent - engCurrent);
            INFO(model.name << " VDD=" << bias.vdd << " VGS=" << bias.vgs
                            << " ng d/g=" << ngD[0] << "/" << ngG[0] << " eng d/g="
                            << engD << "/" << engG);
            maxNodeErr = std::max(maxNodeErr, nodeErr);
            maxCurrentErr = std::max(maxCurrentErr, currentErr);
            ++points;
            REQUIRE(nodeErr <= 5e-4);
            REQUIRE(currentErr <= 1e-5);
        }
        ++modelIndex;
    }
    WARN("ngspice JFET preset DC oracle: models=" << modelIndex << " points=" << points
                                                   << " maxNodeErr=" << maxNodeErr
                                                   << " V maxCurrentErr=" << maxCurrentErr
                                                   << " A");
}

TEST_CASE("ngspice transient oracle: RC lowpass, 100 Hz sine", "[phase1][ngspice][tran]") {
    LWDF_REQUIRE_NGSPICE("RC transient oracle");
    const std::string net =
        "V1 in 0 SIN(0 1 100)\n"
        "R1 in out 1k\n"
        "C1 out 0 1u\n";

    const double tstep = 1.0 / kFs;
    const double tstop = 0.05;  // 50 ms = 2400 samples
    char tranLine[128];
    std::snprintf(tranLine, sizeof tranLine, ".tran %.17g %.17g 0 %.17g\n", tstep, tstop, tstep);

    // Note: after `linearize`, `print v(out)` prints only `Index v(out)` (the
    // time scale is not emitted), so ask for `time v(out)` explicitly. TMAX is
    // pinned to TSTEP so ngspice's own points land on our grid and `linearize`
    // is a no-op (otherwise its chord interpolation is the dominant error).
    const std::string deck = "* livewdf tran oracle\n" + std::string(tranLine) + net +
                             controlBlock({"run", "linearize v(out)"}, {"time v(out)"}, "tran1") +
                             ".end\n";

    const auto run = runDeck(deck, "tran1");
    REQUIRE(run.ok);
    const TranTable ng = parseTranFile(vecFile("tran1", 0), 1);
    REQUIRE(ng.nRows() >= 2400);
    // Sanity: uniform grid at the requested tstep.
    REQUIRE(std::abs((ng.time[1] - ng.time[0]) - tstep) < tstep * 0.01);
    REQUIRE(std::abs(ng.time[ng.nRows() - 1] - tstop) < tstep * 0.5);

    wdf::Circuit c = wdf::buildFromString(net);
    wdf::Engine e(c, kFs);
    std::vector<double> node, rootV;
    REQUIRE(e.solveDc(node, rootV));  // ngspice tran starts from its DC OP as well
    e.reset();                        // seed capacitor states from that OP
    const int outIdx = e.nodeIndex("out");
    REQUIRE(outIdx >= 0);

    double maxErr = 0.0;
    int compared = 0;
    for (size_t row = 0; row < ng.nRows(); ++row) {
        // Drive the engine one step per ngspice grid row, using ngspice's own
        // (printed) time values; the grid is uniform to within print precision.
        const double t = ng.time[row];
        const double vin = std::sin(2.0 * M_PI * 100.0 * t);
        const std::vector<double> vEng = e.step({vin});
        const double engOut = vEng[static_cast<size_t>(outIdx)];
        REQUIRE(std::isfinite(engOut));
        REQUIRE(e.lastConverged());
        maxErr = std::max(maxErr, std::abs(engOut - ng.cols[0][row]));
        ++compared;
    }
    REQUIRE(compared >= 2400);
    WARN("ngspice tran oracle: maxErr=" << maxErr << " over " << compared << " samples");
    // Verify.py TD threshold is 0.5 dB on the fundamental; pointwise on a
    // smooth 1 V signal this is considerably stricter in relative terms.
    // Achieved 2.6e-05 (measured 2026-10-18); 5e-4 keeps 20x regression margin.
    REQUIRE(maxErr <= 5e-4);
}

TEST_CASE("ngspice transient oracle: blockwise JFET pair plus diode clippers",
          "[blocksolve][ngspice][tran]") {
    LWDF_REQUIRE_NGSPICE("blockwise JFET pair transient oracle");

    // The JFET pair shares one source resistor, so their drain currents are
    // mutually coupled and form one non-scalar block. The two independent
    // diode clippers are scalar blocks. This is the exact shape the block
    // solver exists for, checked against a simulator that has never seen its
    // partition or its WDF discretization.
    const std::string model =
        ".model J2N5952 NJF(Beta=1.314m Vto=-2.021 Lambda=4m Rd=0 Rs=0 Cgd=0 Cgs=0)\n"
        ".model DTEST D(IS=1e-9 N=1 RS=0 RP=0)\n";  // ngspice's plain law explicitly
    const std::string net =
        "* livewdf blockwise transient oracle\n"
        "VDD vdd 0 DC 15\n"
        "VIN in 0 SIN(0 1 100)\n"
        "VCA ca_in 0 SIN(0 1 100)\n"
        "VCB cb_in 0 SIN(0 1 100)\n"
        "RIN in g1 1k\n"
        "RG1 g1 0 1meg\n"
        "RG2 g2 0 1meg\n"
        "RD1 vdd d1 2.2k\n"
        "RD2 vdd d2 2.2k\n"
        "RS s 0 1k\n"
        "J1 d1 g1 s J2N5952\n"
        "J2 d2 g2 s J2N5952\n"
        "RCA ca_in ca 1k\n"
        "CCA ca 0 1u\n"
        "DA ca 0 DTEST\n"
        "RCB cb_in cb 1k\n"
        "CCB cb 0 1u\n"
        "DB cb 0 DTEST\n" +
        model;

    const double tstep = 1.0 / kFs;
    const double tstop = 0.05;
    char tranLine[128];
    std::snprintf(tranLine, sizeof tranLine, ".tran %.17g %.17g 0 %.17g\n", tstep, tstop, tstep);
    // One vector per file: ngspice wraps wide `print` tables into repeated
    // headers and split rows at its default terminal width.
    const std::string control =
        ".control\n"
        "set filetype=ascii\n"
        "set numdgt=15\n"
        "run\n"
        "linearize v(d1) v(d2) v(ca) v(cb)\n"
        "print time v(d1) > tran_block_d1.txt\n"
        "print time v(d2) > tran_block_d2.txt\n"
        "print time v(ca) > tran_block_ca.txt\n"
        "print time v(cb) > tran_block_cb.txt\n"
        ".endc\n";
    const auto run =
        runDeck("* livewdf blockwise tran oracle\n" + std::string(tranLine) + net + control,
                "tran_block");
    REQUIRE(run.ok);
    const TranTable ng[4] = {
        parseTranFile(lwdf_test::scratchDir() / "tran_block_d1.txt", 1),
        parseTranFile(lwdf_test::scratchDir() / "tran_block_d2.txt", 1),
        parseTranFile(lwdf_test::scratchDir() / "tran_block_ca.txt", 1),
        parseTranFile(lwdf_test::scratchDir() / "tran_block_cb.txt", 1)};
    REQUIRE(ng[0].nRows() >= 2400);
    REQUIRE(ng[1].nRows() == ng[0].nRows());
    REQUIRE(ng[2].nRows() == ng[0].nRows());
    REQUIRE(ng[3].nRows() == ng[0].nRows());

    // Parse the same topology the deck uses. The parser builds the JFET gate
    // resistor and intrinsic nodes exactly as the SPICE card does, so the only
    // remaining difference between the two runs is the solver.
    const wdf::Circuit c = wdf::buildFromString(net);

    wdf::Engine e(c, kFs);
    std::vector<double> node, rootV;
    REQUIRE(e.solveDc(node, rootV));
    REQUIRE(e.rootBlockCount() >= 3);
    REQUIRE(e.blockRoots());
    e.reset();
    const int d1 = e.nodeIndex("d1");
    const int d2 = e.nodeIndex("d2");
    const int ca = e.nodeIndex("ca");
    const int cb = e.nodeIndex("cb");
    REQUIRE(d1 >= 0);
    REQUIRE(d2 >= 0);
    REQUIRE(ca >= 0);
    REQUIRE(cb >= 0);
    const int idx[4] = {d1, d2, ca, cb};

    double maxErr = 0.0;
    size_t compared = 0;
    for (size_t row = 0; row < ng[0].nRows(); ++row) {
        const double t = ng[0].time[row];
        const double vin = std::sin(2.0 * M_PI * 100.0 * t);
        std::vector<double> sv(c.src.size(), 0.0);
        for (size_t i = 0; i < c.src.size(); ++i) {
            const std::string& name = c.src[i].name;
            sv[i] = (name == "VDD") ? 15.0 : vin;
        }
        const std::vector<double>& y = e.step(sv);
        REQUIRE(e.lastConverged());
        for (int k = 0; k < 4; ++k) {
            const double got = y[static_cast<size_t>(idx[k])];
            REQUIRE(std::isfinite(got));
            maxErr = std::max(maxErr, std::abs(got - ng[k].cols[0][row]));
        }
        ++compared;
    }
    REQUIRE(compared >= 2400);
    REQUIRE(e.blockFallbackCount() == 0);
    WARN("ngspice blockwise tran oracle: maxErr=" << maxErr << " over " << compared
                                                   << " samples");
    REQUIRE(maxErr <= 2e-3);
}

TEST_CASE("ngspice DC sweep: Level-1 NMOS cutoff, ohmic, and saturation",
          "[mosfet][ngspice][dc][sweep]") {
    LWDF_REQUIRE_NGSPICE("Level-1 NMOS DC sweep");
    // The paper's K is SPICE's KP*W/L. With W/L = 1, the matched card below
    // isolates the constitutive law itself: no body effect (bulk tied to
    // source), no series resistances, no capacitance, no subthreshold model.
    // The gate is driven by an ideal source, which is consistent with the
    // paper's zero gate current. This is a generic Level-1 card, not a preset.
    const std::string model =
        ".model NM NMOS(Level=1 KP=1m VTO=1 LAMBDA=0.02)\n";
    const std::vector<double> vdds{0.2, 0.5, 1.0, 2.0, 5.0, 10.0};
    const std::vector<double> vgss{0.5, 1.0, 1.25, 1.5, 2.5, 4.0};

    double maxNodeErr = 0.0;
    double maxCurrentErr = 0.0;
    double sumNodeSq = 0.0;
    double sumCurrentSq = 0.0;
    double sumNodeAbs = 0.0;
    double sumCurrentAbs = 0.0;
    int points = 0;
    int cutoff = 0, ohmic = 0, saturation = 0;
    int index = 0;
    for (double vdd : vdds) {
        for (double vgs : vgss) {
            char supply[64], gate[64];
            std::snprintf(supply, sizeof supply, "VDD vdd 0 DC %.17g\n", vdd);
            std::snprintf(gate, sizeof gate, "VG g 0 DC %.17g\n", vgs);
            const std::string net = std::string("* livewdf nmos dc sweep\n") + supply + gate +
                                    "RD vdd d 1k\n"
                                    "M1 d g 0 0 NM W=1 L=1\n" +
                                    model;
            const std::string tag = "nmos_sweep_" + std::to_string(index++);
            const auto run =
                runDeck(net + controlBlock({"op"}, {"v(d)", "v(g)"}, tag) + ".end\n", tag);
            REQUIRE(run.ok);
            const auto ngD = parseOpFile(vecFile(tag, 0));
            const auto ngG = parseOpFile(vecFile(tag, 1));
            REQUIRE(ngD.size() == 1);
            REQUIRE(ngG.size() == 1);

            wdf::Circuit c = wdf::buildFromString(net);
            REQUIRE(c.mosfet.size() == 1);
            wdf::Engine e(c, kFs);
            std::vector<double> node, rootV;
            REQUIRE(e.solveDc(node, rootV));
            const double engD = node[static_cast<size_t>(e.nodeIndex("d"))];
            // The gate is driven by an ideal source and draws no current, so
            // the engine can eliminate it from the unknown-node list. Its
            // voltage is the gate-source root voltage (the source is ground).
            const int gIndex = e.nodeIndex("g");
            const double engG = gIndex >= 0 ? node[static_cast<size_t>(gIndex)] : rootV[1];
            const double ngCurrent = (vdd - ngD[0]) / 1000.0;
            const double engCurrent = (vdd - engD) / 1000.0;
            const double nodeErr = std::max(std::abs(ngD[0] - engD), std::abs(ngG[0] - engG));
            const double currentErr = std::abs(ngCurrent - engCurrent);
            INFO("VDD=" << vdd << " VGS=" << vgs << " ng d/g=" << ngD[0] << "/" << ngG[0]
                         << " eng d/g=" << engD << "/" << engG << " I=" << ngCurrent << "/"
                         << engCurrent);
            maxNodeErr = std::max(maxNodeErr, nodeErr);
            maxCurrentErr = std::max(maxCurrentErr, currentErr);
            sumNodeSq += nodeErr * nodeErr;
            sumCurrentSq += currentErr * currentErr;
            sumNodeAbs += nodeErr;
            sumCurrentAbs += currentErr;
            ++points;

            const auto ev = c.mosfet[0].model.evaluate(rootV[1], rootV[0]);
            REQUIRE(ev.valid);
            if (ev.region == wdf::MosfetRegion::Cutoff)
                ++cutoff;
            else if (ev.region == wdf::MosfetRegion::Ohmic)
                ++ohmic;
            else if (ev.region == wdf::MosfetRegion::Saturation)
                ++saturation;

            REQUIRE(nodeErr <= 5e-4);
            REQUIRE(currentErr <= 1e-6);
        }
    }
    REQUIRE(cutoff > 0);
    REQUIRE(ohmic > 0);
    REQUIRE(saturation > 0);
    const double nodeRms = std::sqrt(sumNodeSq / static_cast<double>(points));
    const double currentRms = std::sqrt(sumCurrentSq / static_cast<double>(points));
    const double nodeMean = sumNodeAbs / static_cast<double>(points);
    const double currentMean = sumCurrentAbs / static_cast<double>(points);
    WARN("ngspice NMOS DC sweep: points=" << points << " cutoff/ohmic/sat=" << cutoff << "/"
                                           << ohmic << "/" << saturation << " maxNode="
                                           << maxNodeErr << " V rmsNode=" << nodeRms
                                           << " V meanNode=" << nodeMean << " V maxI="
                                           << maxCurrentErr << " A rmsI=" << currentRms
                                           << " A meanI=" << currentMean << " A");
}
