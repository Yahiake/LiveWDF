#include <catch2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "spice/SpiceExport.h"
#include "support/NgspiceOracle.h"
#include "wdf/Engine.h"

namespace {

constexpr double sampleRate = 48000.0;

std::string exportNetwork(const wdf::Circuit& circuit) {
    const auto result = wdf::spiceExport(circuit, sampleRate, 0.0);
    REQUIRE(result.ok);
    return result.netlist.substr(0, result.netlist.find(".control"));
}

double dcOutput(const wdf::Circuit& circuit) {
    wdf::Engine engine(circuit, sampleRate);
    std::vector<double> nodes, roots;
    REQUIRE(engine.solveDc(nodes, roots));
    const auto out = engine.nodeIndex("out");
    REQUIRE(out >= 0);
    return nodes[out];
}

} // namespace

TEST_CASE("THAT2180 control sweep matches ngspice and the manufacturer law",
          "[parts-validation][that2180][ngspice]") {
    if (!lwdf_test::requireNgspice("THAT2180 transfer"))
        return;

    constexpr double input = 0.02;
    constexpr double inputResistance = 20025.0;
    constexpr double load = 20000.0;
    const double unityVoltage = input * load / inputResistance;
    std::vector<std::vector<std::string>> rows;
    double maxSpiceError = 0.0, maxFactoryError = 0.0;
    int index = 0;
    // THAT 2180 Series datasheet equation 2, 25 C, both control ports.
    for (double db : {-100., -80., -60., -40., -20., -10., 0., 10., 20., 30., 40.}) {
        const double control = db * 0.0061;
        CAPTURE(db, control);
        wdf::Circuit circuit;
        circuit.addSrc("in", "0", input);
        circuit.addSrc("ecp", "0", std::max(0.0, control));
        circuit.addSrc("ecm", "0", std::max(0.0, -control));
        circuit.addRes("in", "sum", 20000.0);
        circuit.addThat2180("sum", "out", "ecp", "ecm", "0", 25.0, "Gain");
        circuit.addRes("out", "0", load);
        const auto tag = "that2180_transfer_" + std::to_string(index++);
        const auto command = lwdf_test::controlBlock({"op"}, {"v(out)"}, tag);
        REQUIRE(lwdf_test::runDeck(exportNetwork(circuit) + command + ".end\n", tag).ok);

        const double spice = lwdf_test::parseOpFile(lwdf_test::vecFile(tag, 0)).front();
        const double native = dcOutput(circuit);
        REQUIRE(native < 0.0);
        REQUIRE(spice < 0.0);
        const double nativeDb = 20.0 * std::log10(-native / unityVoltage);
        const double spiceDb = 20.0 * std::log10(-spice / unityVoltage);
        maxSpiceError = std::max(maxSpiceError, std::abs(nativeDb - spiceDb));
        maxFactoryError = std::max(maxFactoryError, std::abs(nativeDb - db));
        rows.push_back({lwdf_test::csvNumber(control), lwdf_test::csvNumber(nativeDb),
                        lwdf_test::csvNumber(spiceDb), lwdf_test::csvNumber(db)});
    }
    lwdf_test::writeCsv("parts_that2180", {"control_V", "livewdf_dB", "ngspice_dB", "factory_dB"}, rows);
    WARN("THAT2180 max gain error versus ngspice " << maxSpiceError
         << " dB, versus nominal manufacturer curve " << maxFactoryError << " dB");
    CHECK(maxSpiceError < 0.01);
    CHECK(maxFactoryError < 0.01);
}

TEST_CASE("Log RMS transient follows ngspice and the specified time constant",
          "[parts-validation][rms][ngspice]") {
    if (!lwdf_test::requireNgspice("log RMS response"))
        return;

    constexpr double timeConstant = 0.016;
    constexpr double scale = 0.0061;
    constexpr double stepTime = 0.01;
    const std::string tag = "parts_rms_step";
    wdf::RmsDetectorModel model;
    model.timeConstantSeconds = timeConstant;
    wdf::Circuit circuit;
    circuit.addSrc("in", "0", 1.0);
    circuit.addRmsDetector("in", "out", "0", model, "Level");
    auto network = exportNetwork(circuit);
    const auto source = network.find(" DC 1\n");
    REQUIRE(source != std::string::npos);
    network.replace(source, 6, " DC 1 PWL(0 1 9.99999m 1 10m 2)\n");
    REQUIRE(lwdf_test::runDeck(network +
        ".control\nset numdgt=15\ntran 20.8333333333u 70m 0 2u\n"
        "linearize v(out)\nwrdata parts_rms_step_0.txt v(out)\n.endc\n.end\n", tag).ok);
    const auto spice = lwdf_test::parseTranFile(lwdf_test::vecFile(tag, 0), 2);

    wdf::Engine engine(circuit, sampleRate);
    const auto out = engine.nodeIndex("out");
    REQUIRE(out >= 0);
    std::vector<std::vector<std::string>> rows;
    double maxSpiceError = 0.0, sumSquaredError = 0.0, maxFactoryError = 0.0;
    for (size_t n = 0; n < spice.nRows(); ++n) {
        const double t = n / sampleRate;
        const double native = engine.step({t < stepTime ? 1.0 : 2.0})[out];
        const double reference = 10.0 * scale * std::log10(
            4.0 - 3.0 * std::exp(-std::max(0.0, t - stepTime) / timeConstant));
        const double error = native - spice.cols[1][n];
        maxSpiceError = std::max(maxSpiceError, std::abs(error));
        maxFactoryError = std::max(maxFactoryError, std::abs(native - reference));
        sumSquaredError += error * error;
        rows.push_back({lwdf_test::csvNumber(t), lwdf_test::csvNumber(native),
                        lwdf_test::csvNumber(spice.cols[1][n]), lwdf_test::csvNumber(reference)});
    }
    lwdf_test::writeCsv("parts_rms", {"time_s", "livewdf_V", "ngspice_V", "factory_V"}, rows);
    WARN("RMS detector max error " << maxSpiceError << " V versus ngspice, RMS error "
         << std::sqrt(sumSquaredError / spice.nRows()) << " V, max error " << maxFactoryError
         << " V versus the nominal datasheet-based response");
    CHECK(engine.stateRejectCount() == 0);
    CHECK(maxSpiceError < 50e-6);
    CHECK(maxFactoryError < 50e-6);
}

TEST_CASE("MN3005 held-charge delay follows the factory clock-to-delay curve",
          "[parts-validation][bbd]") {
    std::vector<std::vector<std::string>> rows;
    double maxError = 0.0;
    for (double clock : {10000., 15000., 20000., 30000., 40000., 50000., 63000., 80000., 100000.}) {
        // Panasonic MN3005: 4096 stages, 20.48–204.8 ms at 100–10 kHz.
        wdf::BucketBrigadeModel model;
        model.clockHz = clock;
        model.inputBias = model.outputBias = 0.0;
        model.inputSwing = 1.0;
        model.gain = 1.0;
        wdf::BucketBrigadeState bbd(model);
        constexpr double duration = 0.25;
        double observed = duration;
        for (int n = 1; n <= static_cast<int>(duration * sampleRate); ++n) {
            if (bbd.step(1.0, 0.0, 1.0 / sampleRate) > 0.5) {
                observed = n / sampleRate;
                break;
            }
        }
        const double reference = 2048.0 / clock;
        maxError = std::max(maxError, std::abs(observed - reference));
        rows.push_back({lwdf_test::csvNumber(clock), lwdf_test::csvNumber(observed),
                        lwdf_test::csvNumber(reference)});
    }
    lwdf_test::writeCsv("parts_mn3005", {"clock_Hz", "livewdf_s", "factory_s"}, rows);
    WARN("MN3005 maximum clock-to-delay error " << maxError * 1e6 << " us");
    CHECK(maxError <= 1.0 / sampleRate + 1.0 / 10000.0);
}

TEST_CASE("CA3080 transconductance tracks bias and matches the SPICE export",
          "[parts-validation][ca3080][ngspice]") {
    if (!lwdf_test::requireNgspice("CA3080 transconductance"))
        return;

    constexpr double drive = 0.0001;
    constexpr double load = 1000.0;
    std::vector<std::vector<std::string>> rows;
    double maxRelativeError = 0.0;
    int index = 0;
    double gmAt500uA = 0.0;
    for (double bias : {5e-6, 10e-6, 25e-6, 50e-6, 100e-6, 200e-6, 500e-6, 1e-3}) {
        CAPTURE(bias);
        wdf::Circuit circuit;
        circuit.addSrc("p", "0", drive);
        circuit.addSrc("vcc", "0", 15.0);
        circuit.addSrc("vee", "0", -15.0);
        circuit.addIsrc("0", "bias", bias, 0.0, {}, "Iabc");
        circuit.addCa3080("out", "0", "p", "vcc", "vee", "bias", "OTA1");
        circuit.addRes("out", "0", load);
        const auto tag = "ca3080_transfer_" + std::to_string(index++);
        const auto control = lwdf_test::controlBlock({"op"}, {"v(out)"}, tag);
        REQUIRE(lwdf_test::runDeck(exportNetwork(circuit) + control + ".end\n", tag).ok);
        const double spice = std::abs(lwdf_test::parseOpFile(lwdf_test::vecFile(tag, 0)).front());
        const double native = std::abs(dcOutput(circuit));
        REQUIRE(native > 0.0);
        REQUIRE(spice > 0.0);
        maxRelativeError = std::max(maxRelativeError, std::abs(native / spice - 1.0));
        const double gm = native / (drive * load);
        if (bias == 500e-6)
            gmAt500uA = gm;
        // Intersil FN475 uses approximately proportional gm at 25 C in
        // Figure 21. Anchor a reference line to the 500 uA table point.
        rows.push_back({lwdf_test::csvNumber(bias), lwdf_test::csvNumber(gm),
                        lwdf_test::csvNumber(spice / (drive * load)),
                        lwdf_test::csvNumber(9.6e-3 * bias / 500e-6)});
    }
    lwdf_test::writeCsv("parts_ca3080", {"bias_A", "livewdf_S", "ngspice_S", "factory_S"}, rows);
    WARN("CA3080 maximum relative ngspice error " << maxRelativeError
         << ", gm at 500 uA " << gmAt500uA << " S (manufacturer typical 0.0096 S)");
    CHECK(maxRelativeError < 0.005);
    CHECK(gmAt500uA == Approx(9.6e-3).epsilon(0.03));
}

TEST_CASE("Measure OP275 closed-loop response against an external SPICE model",
          "[.][op275-reference][ngspice]") {
    if (!lwdf_test::requireNgspice("OP275 response"))
        return;

    const char* path = std::getenv("LIVEWDF_OP275_SPICE_MODEL");
    REQUIRE(path != nullptr);
    std::ifstream file(path);
    REQUIRE(file.good());
    std::ostringstream vendorModel;
    vendorModel << file.rdbuf();

    constexpr double rate = 192000.0;
    constexpr double amplitude = 0.01;
    constexpr int warmupFrames = 1920;
    constexpr int frames = 5760;
    const std::string timestep = lwdf_test::csvNumber(1.0 / rate);
    const std::string maxStep = lwdf_test::csvNumber(1.0 / (rate * 16));
    wdf::Circuit circuit;
    circuit.addSrc("in", "0");
    circuit.addSrc("vp", "0", 15.0);
    circuit.addSrc("vn", "0", -15.0);
    circuit.addRes("out", "fb", 99000.0);
    circuit.addRes("fb", "0", 1000.0);
    circuit.addRes("out", "0", 2000.0);
    circuit.addOpAmp("out", "fb", "in", "vp", "vn",
                     wdf::OpAmpModel::fromType(wdf::OpAmpType::Op275), "U1");

    std::vector<std::vector<std::string>> rows, wave;
    for (const int hz : {1000, 2000, 4000, 8000, 12000, 16000, 20000}) {
        CAPTURE(hz);
        const std::string tag = "op275_gain_" + std::to_string(hz);
        const auto deck = "OP275 gain of 100 with 2 kohm load\n"
            "Vin in 0 SIN(0 10m " + std::to_string(hz) + ")\n"
            "Vplus vp 0 15\nVminus vn 0 -15\n"
            "Rf out fb 99k\nRg fb 0 1k\nRL out 0 2k\nX1 in fb vp vn out OP275\n"
            + vendorModel.str() + "\n.control\nset numdgt=15\ntran "
            + timestep + " 30m 0 " + maxStep + "\nlinearize v(out)\nwrdata "
            + lwdf_test::redirectName(tag, 0) + " v(out)\n.endc\n.end\n";
        REQUIRE(lwdf_test::runDeck(deck, tag).ok);
        const auto spice = lwdf_test::parseTranFile(lwdf_test::vecFile(tag, 0), 2);
        REQUIRE(spice.nRows() >= frames);

        wdf::Engine engine(circuit, rate);
        const auto out = engine.nodeIndex("out");
        REQUIRE(out >= 0);
        std::complex<double> nativeTone, spiceTone;
        double errorSquared = 0.0, errorPeak = 0.0;
        for (int n = 0; n < frames; ++n) {
            const double t = n / rate;
            const double phase = 2.0 * wdf::kPi * hz * t;
            const double native = engine.step({amplitude * std::sin(phase), 15.0, -15.0})[out];
            if (n < warmupFrames)
                continue;

            const double reference = spice.cols[1][n];
            const auto weight = std::polar(1.0, -phase);
            nativeTone += native * weight;
            spiceTone += reference * weight;
            const double error = native - reference;
            errorSquared += error * error;
            errorPeak = std::max(errorPeak, std::abs(error));
            if (hz == 20000 && n < warmupFrames + 48)
                wave.push_back({lwdf_test::csvNumber(t - warmupFrames / rate),
                                lwdf_test::csvNumber(native), lwdf_test::csvNumber(reference)});
        }
        const auto gainDb = [&](std::complex<double> tone) {
            return 20.0 * std::log10(2.0 * std::abs(tone) / (amplitude * (frames - warmupFrames)));
        };
        const double nativeDb = gainDb(nativeTone);
        const double spiceDb = gainDb(spiceTone);
        REQUIRE(spiceDb == Approx(40.0).margin(1.0));
        const double phaseError = std::arg(nativeTone / spiceTone) * 180.0 / wdf::kPi;
        const double rms = std::sqrt(errorSquared / (frames - warmupFrames));
        // ADI Rev. C TPC 5 shows the +100 trace at about 40 dB over this band.
        rows.push_back({std::to_string(hz), lwdf_test::csvNumber(nativeDb),
                        lwdf_test::csvNumber(spiceDb), "40", lwdf_test::csvNumber(phaseError),
                        lwdf_test::csvNumber(rms), lwdf_test::csvNumber(errorPeak)});
        WARN("OP275 " << hz << " Hz gain error " << nativeDb - spiceDb
             << " dB, phase error " << phaseError << " deg, waveform RMS error " << rms << " V");
        CHECK(std::abs(nativeDb - spiceDb) < 0.4);
        CHECK(std::abs(phaseError) < 1.1);
        CHECK(engine.stateRejectCount() == 0);
    }
    lwdf_test::writeCsv("parts_op275", {"frequency_Hz", "livewdf_dB", "ngspice_dB",
                      "factory_dB", "phase_error_deg", "rms_error_V", "peak_error_V"}, rows);
    lwdf_test::writeCsv("parts_op275_wave", {"time_s", "livewdf_V", "ngspice_V"}, wave);
}

TEST_CASE("dbx 200 matched pairs follow the original transistor drawing and symmetry port",
          "[parts-validation][dbx200][ngspice]") {
    if (!lwdf_test::requireNgspice("dbx 200 module drawing 110557"))
        return;
    const auto fixture = std::filesystem::path(__FILE__).parent_path().parent_path()
                         / "fixtures/parts/dbx200-110557.lib";
    std::ifstream referenceFile(fixture);
    REQUIRE(referenceFile.good());
    std::ostringstream device;
    device << referenceFile.rdbuf();
    std::vector<std::vector<std::string>> rows;
    int run = 0;
    for (const auto& [ec, trim] : std::vector<std::pair<double, double>>{
             {-0.060, 0.0}, {0.0, 0.0}, {0.060, 0.0}, {0.0, 0.003}, {0.030, 0.003}}) {
        for (double signal : {-2.0, 0.0, 2.0}) {
            CAPTURE(ec, trim, signal);
            wdf::Circuit c;
            c.addSrc("in", "0", signal);
            c.addSrc("ec", "0", ec);
            if (trim != 0.0) c.addSrc("trim", "0", trim);
            const std::string po = trim == 0.0 ? "0" : "trim";
            c.addRes("in", "summing", 100e3);
            c.addDbx200("summing", "sink", "ec", "0", po, "ec", "0", "M1");
            c.addVCVS("out", "0", "0", "sink", 1e6, 1);
            c.addRes("out", "sink", 1000.0);
            wdf::Engine engine(c, sampleRate);
            engine.reset();
            std::vector<double> sources;
            for (const auto& src : c.src) sources.push_back(src.dc);
            for (int k = 0; k < 8; ++k) engine.step(sources);
            const double actual = engine.step(sources)[engine.nodeIndex("out")];
            REQUIRE(engine.lastConverged());
            CHECK(engine.stateRejectCount() == 0);
            const auto tag = "dbx200_ref_" + std::to_string(run++);
            const auto commands = lwdf_test::controlBlock({"op"}, {"v(out)"}, tag);
            std::ostringstream ref;
            ref << "dbx 200 original reference\n.temp 25\n" << device.str()
                << "\nVp vp 0 7.5\nVm vm 0 -15\nVec ec 0 " << ec
                << "\nVtrim trim 0 " << trim << "\nVin in 0 " << signal
                << "\nRin in s 100k\nX200 s sink ec 0 trim ec vp vm 0 DBX200_REF"
                   "\nEout out 0 0 sink 1e6\nRfb out sink 1k\n" << commands << ".end\n";
            REQUIRE(lwdf_test::runDeck(ref.str(), tag).ok);
            const double transistor = lwdf_test::parseOpFile(lwdf_test::vecFile(tag, 0)).front();
            CHECK(std::abs(actual - transistor) < 0.015 * std::abs(transistor) + 0.00008);

            const auto exportedTag = tag + "_export";
            const auto exportedCommands = lwdf_test::controlBlock({"op"}, {"v(out)"}, exportedTag);
            REQUIRE(lwdf_test::runDeck(exportNetwork(c) + exportedCommands + ".end\n", exportedTag).ok);
            const double exported = lwdf_test::parseOpFile(lwdf_test::vecFile(exportedTag, 0)).front();
            CHECK(actual == Approx(exported).margin(3e-7));
            rows.push_back({lwdf_test::csvNumber(ec), lwdf_test::csvNumber(trim),
                            lwdf_test::csvNumber(signal), lwdf_test::csvNumber(actual),
                            lwdf_test::csvNumber(transistor), lwdf_test::csvNumber(exported)});
        }
    }
    lwdf_test::writeCsv("parts_dbx200", {"control_V", "trim_V", "signal_V",
                      "livewdf_V", "ngspice_transistor_V", "ngspice_export_V"}, rows);
}

TEST_CASE("dbx 209 power integration agrees with a log-capacitor ngspice reference",
          "[parts-validation][dbx209][ngspice]") {
    if (!lwdf_test::requireNgspice("dbx 209 terminal ballistics"))
        return;
    // Blackmer US 3,681,618: rectified, doubled-log input feeds an antilog
    // current into a storage capacitor opposed by a fixed discharge current.
    // Use normalized input RMS so this tests the charge equation independently
    // of the implementation's linear power integration.
    const std::string tag = "dbx209_log_cap";
    const auto command = lwdf_test::controlBlock({"tran 20.833333333u 150m 0 5u uic", "linearize"},
                                                  {"time v(env)"}, tag);
    const std::string deck =
        "dbx 209 terminal timing reference\n"
        "Vin in 0 PWL(0 1 10m 1 10.00001m 10 80m 10 80.00001m 0)\n"
        "C15 env 0 22u IC=0\n"
        "Bcharge 0 env I=15.84u*(1-V(in)^2*exp(min(V(env)/0.026057669,40)))\n";
    REQUIRE(lwdf_test::runDeck(deck + command + ".end\n", tag).ok);
    const auto trace = lwdf_test::parseTranFile(lwdf_test::vecFile(tag, 0), 1);
    const auto model = wdf::RmsDetectorModel::dbx209(23.34e-6, 22e-6);
    REQUIRE(trace.nRows() >= 7200);
    wdf::RmsDetectorState detector(model, 1.0 / sampleRate);
    detector.reset(model.referenceVolts);
    double worst = 0.0;
    std::vector<std::vector<std::string>> rows;
    for (size_t i = 0; i < trace.nRows(); ++i) {
        const double t = trace.time[i];
        const double normalized = t < .010 ? 1.0 : (t < .080 ? 10.0 : 0.0);
        if (i != 0) detector.step(normalized * model.referenceVolts);
        const double actual = detector.outputCurrent() * model.outputOhms;
        // A continuous ngspice transition and a sample-held step differ by
        // up to one frame at the transition. Check the resolved envelope.
        if (t > .012 && std::abs(t - .080) > .001)
            worst = std::max(worst, std::abs(actual - trace.cols[0][i]));
        rows.push_back({lwdf_test::csvNumber(t), lwdf_test::csvNumber(actual),
                        lwdf_test::csvNumber(trace.cols[0][i])});
    }
    CAPTURE(worst);
    CHECK(worst < 0.00035); // <0.06 dB
    lwdf_test::writeCsv("parts_dbx209", {"time_s", "livewdf_V", "ngspice_V"}, rows);
}

TEST_CASE("Fairchild 2N4403 input pair agrees with the full transistor card and gain limits",
          "[parts-validation][2n4403][ngspice]") {
    if (!lwdf_test::requireNgspice("Soundcraft 2N4403 input pair")) return;
    const auto* part = wdf::findBjtByName("2N4403");
    REQUIRE(part != nullptr);
    REQUIRE(part->model.pnp);
    // Fairchild card from LTspice standard.bjt. The vendor fixture retains
    // the ohmic and charge terms that the native static transport omits.
    const std::string device =
        ".model full PNP(Is=650.6e-18 Xti=3 Eg=1.11 Vaf=115.7 Bf=216.2 Ne=1.829 Ise=58.72f "
        "Ikf=1.079 Xtb=1.5 Br=3.578 Nc=2 Isc=0 Ikr=0 Rc=.715 Cjc=14.76p Mjc=.5383 "
        "Vjc=.75 Fc=.5 Cje=19.82p Mje=.3357 Vje=.75 Tr=111.6n Tf=603.7p Itf=.65 "
        "Vtf=5 Xtf=1.7 Rb=10)\n";
    struct Point { double amps, vce, minBeta, maxBeta; };
    int i = 0;
    for (const Point pt : {Point{0.1e-3, 1, 30, 1e6}, Point{1e-3, 1, 60, 1e6},
                           Point{10e-3, 1, 100, 1e6}, Point{150e-3, 2, 100, 300}}) {
        CAPTURE(pt.amps);
        const auto tag = "parts_2n4403_" + std::to_string(i++);
        std::ostringstream ref;
        ref << "2N4403 25.85mV thermal voltage\n.options tnom=26.83\n.temp 26.83\n" << device
            << "Vref ref 0 " << -pt.vce << "\nIload c 0 " << pt.amps
            << "\nEbase b 0 c ref 1e6\nQ1 c b 0 full\n"
            << lwdf_test::controlBlock({"op"}, {"i(Ebase)", "v(b)"}, tag) << ".end\n";
        REQUIRE(lwdf_test::runDeck(ref.str(), tag).ok);
        const double baseAmps = lwdf_test::parseOpFile(lwdf_test::vecFile(tag, 0)).front();
        const double baseVolts = lwdf_test::parseOpFile(lwdf_test::vecFile(tag, 1)).front();
        wdf::Circuit c;
        c.addSrc("rail", "0", -pt.vce);
        c.addRes("out", "rail", 1.0);
        c.addRes("base", "0", 1e6);
        c.addIsrc("base", "0", baseAmps - baseVolts / 1e6);
        c.addBjt("out", "base", "0", part->model, "Q1");
        wdf::Engine engine(c, sampleRate);
        std::vector<double> nodes, roots;
        REQUIRE(engine.solveDc(nodes, roots));
        const double actual = nodes[engine.nodeIndex("out")] + pt.vce;
        CHECK(actual == Approx(pt.amps).epsilon(0.02));
        CHECK(nodes[engine.nodeIndex("base")] == Approx(baseVolts).margin(0.012));
        CHECK(actual / baseAmps >= pt.minBeta);
        CHECK(actual / baseAmps <= pt.maxBeta);
    }
}

TEST_CASE("Legacy LM358 changes sink paths at its lower rail", "[lm358][parts-validation]") {
    // Independent .op readings from MiSTer-Discrete 692e777 lm358.lib,
    // 15 V supply and the non-inverting input driven to -1 V. The transition
    // above the pulldown range is visible in TI SLOS068AB Figure 5-47.
    struct Point { double loadAmps, spiceVolts; };
    for (const Point pt : {Point{0, 0.0005243}, Point{10e-6, 0.011823},
                           Point{20e-6, 0.032349}, Point{35e-6, 0.56953},
                           Point{100e-6, 0.63445}, Point{1e-3, 0.75166}, Point{10e-3, 1.3144}}) {
        CAPTURE(pt.loadAmps);
        wdf::Circuit c;
        c.addSrc("vcc", "0", 15.0);
        c.addSrc("input", "0", -1.0);
        auto model = wdf::OpAmpModel::fromType(wdf::opAmpTypeFromString("LM358"));
        REQUIRE(model.type == wdf::OpAmpType::Lm358);
        model.setVRailBounds(0.245, 14.53);
        c.addOpAmp("out", "out", "input", "vcc", "0", model, "U1");
        c.addIsrc("vcc", "out", pt.loadAmps);
        wdf::Engine engine(c, sampleRate);
        engine.reset();
        const auto out = engine.nodeIndex("out");
        REQUIRE(out >= 0);
        for (int i = 0; i < 16; ++i) {
            const auto& y = engine.step({15.0, -1.0});
            REQUIRE(engine.lastConverged());
            CHECK(y[out] == Approx(pt.spiceVolts).margin(0.012));
        }
        CHECK(engine.stateRejectCount() == 0);
    }
}

TEST_CASE("LM358 sources load current from its positive supply and reaches its drive limit",
          "[lm358][parts-validation]") {
    // Full upstream SPICE model at 15 V with a 1 ohm positive-rail ammeter,
    // a 10 kohm output load and an independently applied load current.
    struct Point { double loadAmps, spiceOut, spiceSupply; };
    for (const Point pt : {Point{0.0, 13.5486, 14.99829}, Point{.010, 13.4219, 14.98831},
                           Point{.030, 13.3185, 14.96832}, Point{.041, -.08834, 14.95954}}) {
        CAPTURE(pt.loadAmps);
        wdf::Circuit c;
        c.addSrc("rail", "0", 15.0);
        c.addRes("rail", "vcc", 1.0);
        c.addSrc("input", "0", 16.0);
        c.addRes("out", "0", 10000.0);
        c.addIsrc("out", "0", pt.loadAmps);
        auto model = wdf::OpAmpModel::fromType(wdf::OpAmpType::Lm358);
        model.setVRailBounds(.245, 14.53);
        c.addOpAmp("out", "out", "input", "vcc", "0", model, "U1");
        wdf::Engine engine(c, sampleRate);
        engine.reset();
        for (int i = 0; i < 16; ++i) {
            const auto& y = engine.step({15.0, 16.0});
            REQUIRE(engine.lastConverged());
            CHECK(y[engine.nodeIndex("out")] == Approx(pt.spiceOut).margin(0.050));
            CHECK(y[engine.nodeIndex("vcc")] == Approx(pt.spiceSupply).margin(0.0001));
        }
        if (pt.loadAmps == .010 && lwdf_test::ngspiceAvailable()) {
            const std::string tag = "parts_lm358_loaded_export";
            const auto measure = lwdf_test::controlBlock({"op"}, {"v(out)", "v(vcc)"}, tag);
            REQUIRE(lwdf_test::runDeck(exportNetwork(c) + measure + ".end\n", tag).ok);
            const double exported = lwdf_test::parseOpFile(lwdf_test::vecFile(tag, 0)).front();
            const double supply = lwdf_test::parseOpFile(lwdf_test::vecFile(tag, 1)).front();
            CHECK(exported == Approx(pt.spiceOut).margin(0.050));
            CHECK(supply == Approx(pt.spiceSupply).margin(0.0001));
        }
        CHECK(engine.stateRejectCount() == 0);
    }
}

TEST_CASE("NE570 expander and compressor follow the data sheet's level laws and timing",
          "[ne570][parts-validation]") {
    // onsemi NE570/D figure 3 and AN174 figures 1-2 at VCC 15 V, CRECT 2.2 uF.
    // 0 dBm = 775 mVrms; tracking error is specified +-0.2 dB typical, -0.5/+1.0 dB worst.
    const auto build = [](bool expander) {
        wdf::Circuit c;
        c.addSrc("vcc", "0", 15.0);
        c.addSrc("in", "0");
        c.addNe570("rect", "crect", "cell", "inv", "r3", "out", "vcc", "0", "U1");
        c.addCap("crect", "0", 2.2e-6);
        const std::string drive = expander ? "in" : "out";
        c.addCap(drive, "rect", 2.2e-6);
        c.addCap(drive, "cell", 2.2e-6);
        if (expander) {
            c.addRes("r3", "out", 1.0);
        } else {
            c.addCap("in", "r3", 2.2e-6);
            c.addRes("out", "dc", 20e3);
            c.addRes("dc", "inv", 20e3);
            c.addCap("dc", "0", 10e-6);
        }
        c.addRes("out", "load", 1.0);
        c.addCap("load", "probe", 10e-6);
        c.addRes("probe", "0", 100e3);
        return c;
    };
    const auto run = [&](bool expander, double dbm, double seconds, std::vector<double>* trace = nullptr) {
        const auto circuit = build(expander);
        wdf::Engine engine(circuit, sampleRate);
        const int probe = engine.nodeIndex("probe");
        const double amplitude = 0.775 * std::sqrt(2.0) * std::pow(10.0, dbm / 20.0);
        const int n = static_cast<int>(seconds * sampleRate), tail = static_cast<int>(sampleRate / 10);
        double power = 0.0;
        for (int i = 0; i < n; ++i) {
            const std::array<double, 2> sources{15.0, amplitude * std::sin(2.0 * M_PI * 1000.0 * i / sampleRate)};
            const double v = engine.step(sources.data(), sources.size())[probe];
            if (trace) trace->push_back(v);
            if (i >= n - tail) power += v * v;
        }
        return 20.0 * std::log10(std::sqrt(power / tail) / 0.775);
    };
    double worst = 0.0;
    for (double level = 6.0; level >= -34.0; level -= 10.0) {
        const double expanded = run(true, level, 1.0) - 2.0 * level;
        const double compressed = run(false, level, 1.0) - 0.5 * level;
        worst = std::max({worst, std::abs(expanded), std::abs(compressed)});
        CHECK(expanded == Approx(0.0).margin(1.0));
        CHECK(compressed == Approx(0.0).margin(1.0));
    }
    WARN("NE570 worst level-law error from +6 to -34 dBm: " << worst << " dB");

    // Gain follows a step with tau = 10k x CRECT = 22 ms.
    std::vector<double> trace;
    run(true, 0.0, 0.3, &trace);
    std::vector<double> envelope;
    for (size_t start = 0; start + 48 <= trace.size(); start += 48)
        envelope.push_back(*std::max_element(trace.begin() + start, trace.begin() + start + 48));
    const double settled = envelope.back();
    // The expander output is gain times input, so its envelope reaches
    // 1 - 1/e of the final value one time constant after the step.
    const auto rise = std::find_if(envelope.begin(), envelope.end(),
                                   [&](double v) { return v >= (1.0 - std::exp(-1.0)) * settled; });
    const double tau = (rise - envelope.begin()) * 1e-3;
    WARN("NE570 expander attack time constant " << tau * 1e3 << " ms");
    CHECK(tau == Approx(0.022).epsilon(0.2));
}
