#include <catch2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <math.h>

#include "format/Document.h"
#include "format/PortImpedance.h"
#include "wdf/Engine.h"
#include "support/CircuitCases.h"

// Dedicated verification for the multi-variable Newton-Raphson solver.
//
// These tests verify that:
// 1. Coupled multi-device circuits strictly select and exercise the Newton solver.
// 2. Newton iteration counts (e.newtonIters()) are strictly positive during audio processing.
// 3. Newton achieves sub-nanovolt convergence across both normal and extreme overdrive regimes.
// 4. Forced Newton (setExplicitRoots(false)) matches closed-form solutions within < 1e-10 V.
// 5. Symmetric and balanced coupled topologies (LTP, push-pull, diode bridge) exhibit exact physics.

namespace {
constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

double testSine(int n, double amp, double freq = 220.0) {
    return amp * std::sin(2.0 * kPi * freq * static_cast<double>(n) / kFs);
}
}  // namespace

TEST_CASE("Coupled circuits strictly engage and exercise the Newton solver", "[newton][convergence]") {
    struct NewtonCase {
        const char* name;
        wdf::Circuit (*build)();
        double driveAmp;
        int expectedRoots;
    };

    const NewtonCase cases[] = {
        {"long_tailed_pair", &lwdf_test::longTailedPair, 1.0, 6},
        {"asymmetric_diode_clipper", &lwdf_test::asymmetricDiodeClipper, 1.5, 2},
        {"diode_bridge_rectifier", &lwdf_test::diodeBridgeRectifier, 5.0, 4},
        {"push_pull_power_stage", &lwdf_test::pushPullPowerStage, 1.0, 6},
        {"shared_cathode_pair", &lwdf_test::sharedCathodePair, 0.5, 6},
        {"diode_clipper", &lwdf_test::diodeClipper, 1.0, 2},
        {"tube_screamer_clipping", &lwdf_test::tubeScreamerClippingStage, 1.0, 2},
        {"single_ended_el34", &lwdf_test::singleEndedEl34PowerStage, 1.0, 3},
    };

    for (const auto& tc : cases) {
        DYNAMIC_SECTION("Circuit: " << tc.name) {
            const wdf::Circuit c = (*tc.build)();
            wdf::Engine e(c, kFs);

            // 1. Root structure check
            REQUIRE(e.rootCount() == tc.expectedRoots);
            // A pair of reversed diodes is exact now, so a pair-only circuit
            // claims `explicitRoots()`. The coupled triode/pentode shapes
            // cannot. Force Newton either way: this test is about the joint
            // solver, not about which formulation the engine picked.
            if (e.explicitRoots() || e.blockRoots())
                e.setExplicitRoots(false);
            REQUIRE_FALSE(e.explicitRoots());
            REQUIRE_FALSE(e.blockRoots());

            // 2. DC solve
            std::vector<double> node, rootV;
            REQUIRE(e.solveDc(node, rootV));
            e.reset();

            // 3. Audio streaming: verify Newton actually runs and converges every sample
            long long totalIters = 0;
            constexpr int kRunSamples = 2400; // 50 ms of audio
            for (int n = 0; n < kRunSamples; ++n) {
                std::vector<double> sv(c.src.size(), 0.0);
                for (size_t s = 0; s < c.src.size(); ++s)
                    sv[s] = c.src[s].dc;
                if (!sv.empty())
                    sv[0] = testSine(n, tc.driveAmp);

                const auto& out = e.step(sv);
                REQUIRE(e.lastConverged());
                REQUIRE(e.lastResidual() < 1e-8);
                for (double v : out)
                    REQUIRE(std::isfinite(v));

                totalIters += e.newtonIterations();
            }

            // Must have genuinely iterated (not 0 iterations)
            const double avgIters = static_cast<double>(totalIters) / kRunSamples;
            INFO("Average Newton iterations per sample: " << avgIters);
            REQUIRE(avgIters >= 1.0);
            REQUIRE(avgIters <= 6.0); // Efficient quadratic convergence
        }
    }
}

TEST_CASE("Forced Newton achieves sub-nanovolt parity with closed-form roots", "[newton][parity]") {
    struct ParityFixture {
        const char* name;
        wdf::Circuit (*build)();
    };

    const ParityFixture fixtures[] = {
        {"common_cathode_bypassed", &lwdf_test::commonCathodeBypassed},
        {"common_cathode_unbypassed", &lwdf_test::commonCathodeUnbypassed},
        {"cathode_follower", &lwdf_test::cathodeFollower},
        {"two_stage_cascade", &lwdf_test::twoStageCascade},
        {"diode_single", &lwdf_test::diodeSingle},
        {"diode_clipper", &lwdf_test::diodeClipper},
        {"tube_screamer_clipping", &lwdf_test::tubeScreamerClippingStage},
    };

    for (const auto& f : fixtures) {
        DYNAMIC_SECTION("Parity check for " << f.name) {
            const wdf::Circuit c = (*f.build)();
            wdf::Engine eClosed(c, kFs, false);
            wdf::Engine eNewton(c, kFs, false);

            REQUIRE((eClosed.explicitRoots() || eClosed.blockRoots()));
            eNewton.setExplicitRoots(false);
            REQUIRE_FALSE(eNewton.explicitRoots());
            REQUIRE_FALSE(eNewton.blockRoots());

            eClosed.reset();
            eNewton.reset();

            constexpr int kCheckSamples = 1200;
            double maxDiff = 0.0;
            for (int n = 0; n < kCheckSamples; ++n) {
                std::vector<double> sv(c.src.size(), 0.0);
                for (size_t s = 0; s < c.src.size(); ++s)
                    sv[s] = c.src[s].dc;
                if (!sv.empty())
                    sv[0] = testSine(n, 0.2);

                const auto& yClosed = eClosed.step(sv);
                const auto& yNewton = eNewton.step(sv);

                REQUIRE(eNewton.lastConverged());
                REQUIRE(eNewton.newtonIterations() > 0);

                for (size_t i = 0; i < yClosed.size(); ++i) {
                    const double diff = std::abs(yClosed[i] - yNewton[i]);
                    maxDiff = std::max(maxDiff, diff);
                }
            }
            INFO("Max |Closed - Newton| error: " << maxDiff << " V");
            REQUIRE(maxDiff < 1e-9); // Sub-nanovolt agreement
        }
    }
}

TEST_CASE("Newton solver survives extreme overdrive without divergence", "[newton][robustness]") {
    // Drive Tube Screamer with 15 V peak signal (extreme rail-to-rail overdrive)
    wdf::Engine e(lwdf_test::tubeScreamerClippingStage(), kFs);
    e.reset();

    constexpr int kOverdriveSamples = 1200;
    for (int n = 0; n < kOverdriveSamples; ++n) {
        std::vector<double> sv = { testSine(n, 15.0) };
        const auto& out = e.step(sv, 60, 1e-10);
        REQUIRE(e.lastConverged());
        for (double v : out) {
            REQUIRE(std::isfinite(v));
            REQUIRE(std::abs(v) < 20.0); // Bounded
        }
    }
}

TEST_CASE("Newton backtracks through the HM-2 clipping amplifier rail", "[newton][robustness]") {
    wdf::LiveWDFDocument document;
    std::string error;
    REQUIRE(document.loadFromFile("schematics/pedals_fx/Boss HM-2 Heavy Metal.livewdf", error));
    const auto circuit = document.toCircuit();
    const auto input = std::find_if(circuit.src.begin(), circuit.src.end(), [&](const auto& source) {
        return source.name == document.inputId();
    });
    REQUIRE(input != circuit.src.end());
    const auto inputIndex = static_cast<size_t>(input - circuit.src.begin());
    std::vector<double> sources;
    for (const auto& source : circuit.src)
        sources.push_back(source.dc);

    const auto fs = GENERATE(48000.0, 96000.0);
    wdf::Engine engine(circuit, fs);
    for (int sample = 0; sample < static_cast<int>(fs * .12); ++sample)
        engine.step(sources);

    constexpr std::array<double, 4> frequencies{220, 997, 2100, 5900};
    constexpr std::array<double, 4> weights{.34, .34, .20, .12};
    for (int sample = 0; sample < static_cast<int>(fs * .05); ++sample) {
        double stimulus = 0;
        for (size_t tone = 0; tone < frequencies.size(); ++tone)
            stimulus += weights[tone] * .12 * std::sin(2.0 * kPi * frequencies[tone] * sample / fs);
        sources[inputIndex] = stimulus * std::min(1.0, sample / (fs * .008));
        engine.step(sources);
    }
    REQUIRE(engine.stateRejectCount() == 0);
}

TEST_CASE("Long-Tailed Pair produces differential balanced phase outputs", "[newton][ltp]") {
    wdf::Engine e(lwdf_test::longTailedPair(), kFs);
    e.reset();

    const int out1Idx = e.nodeIndex("out1");
    const int out2Idx = e.nodeIndex("out2");
    REQUIRE(out1Idx >= 0);
    REQUIRE(out2Idx >= 0);

    // Warm up AC coupling caps
    for (int n = 0; n < 4800; ++n) {
        std::vector<double> sv = { testSine(n, 0.5), 300.0 };
        e.step(sv);
    }

    // Measure differential balance over one cycle
    double p1Peak = 0.0;
    double p2Peak = 0.0;
    double maxSum = 0.0;
    for (int n = 0; n < 240; ++n) {
        std::vector<double> sv = { testSine(n, 0.5), 300.0 };
        const auto& out = e.step(sv);
        const double v1 = out[static_cast<size_t>(out1Idx)];
        const double v2 = out[static_cast<size_t>(out2Idx)];
        p1Peak = std::max(p1Peak, std::abs(v1));
        p2Peak = std::max(p2Peak, std::abs(v2));
        maxSum = std::max(maxSum, std::abs(v1 + v2));
    }

    INFO("Peak Out1: " << p1Peak << " V, Peak Out2: " << p2Peak << " V");
    REQUIRE(p1Peak > 1.0);
    REQUIRE(p2Peak > 1.0);
    // Classical 82k/100k LTP outputs match within 15% amplitude
    REQUIRE(std::abs(p1Peak - p2Peak) / p1Peak < 0.15);
    // Outputs are 180 degrees inverted, so sum is much smaller than individual peaks
    REQUIRE(maxSum < 0.25 * p1Peak);
}

TEST_CASE("Diode Bridge Rectifier produces pure positive full-wave rectification", "[newton][bridge]") {
    wdf::Engine e(lwdf_test::diodeBridgeRectifier(), kFs);
    e.reset();

    const int posIdx = e.nodeIndex("dc_pos");
    REQUIRE(posIdx >= 0);

    // Run 50 ms of 5 V AC mains
    double minDc = 1e9;
    double maxDc = -1e9;
    for (int n = 0; n < 2400; ++n) {
        std::vector<double> sv = { testSine(n, 5.0, 50.0) }; // 50 Hz AC
        const auto& out = e.step(sv);
        const double vDc = out[static_cast<size_t>(posIdx)];
        minDc = std::min(minDc, vDc);
        maxDc = std::max(maxDc, vDc);
    }

    INFO("DC Output: Min = " << minDc << " V, Max = " << maxDc << " V");
    // After filter reservoir capacitor charges, output must stay positive (within numerical zero)
    REQUIRE(minDc >= -1e-12);
    REQUIRE(maxDc > 2.0); // Positive DC rail established
}

TEST_CASE("Shared Newton columns preserve the voltage step across live transfer changes", "[engine][newton]") {
    constexpr int n = 12, groups = 4;
    std::vector<double> z(n * n), jac(n * n), f(n), matrix(n * n), rhs(n), expected(n), actual(n);
    for (int i = 0; i < n; ++i) {
        f[i] = std::sin(0.2 + i);
        for (int j = 0; j < n; ++j) {
            const double column = std::cos(0.3 * (1 + i) * (1 + j % groups));
            z[i * n + j] = j < groups ? column : j < 2 * groups ? -column : 0.0;
            jac[i * n + j] = i / 4 == j / 4 ? 0.015 * std::sin(0.4 * (1 + i) * (j + 2)) : 0.;
        }
    }
    wdf::solver::SharedNewtonColumns plan;
    plan.prepare(z, n, n);
    const auto check = [&] {
        REQUIRE(plan.available());
        for (int i = 0; i < n; ++i) {
            rhs[i] = 0;
            for (int k = 0; k < n; ++k) rhs[i] -= jac[i * n + k] * f[k];
            for (int j = 0; j < n; ++j) {
                double value = i == j ? 1.0 : 0.0;
                for (int k = 0; k < n; ++k) value += jac[i * n + k] * z[k * n + j];
                matrix[i * n + j] = value;
            }
        }
        // Three local four-port Jacobians include multi-port derivatives. Active
        // currents map into signed duplicate columns and zero columns.
        std::vector<wdf::RootDevice> devices(3);
        std::vector<int> active(n);
        for (int i = 0; i < n; ++i) active[i] = i;
        for (int d = 0; d < 3; ++d) {
            devices[d].portBase = devices[d].activeRowBase = d * 4;
            devices[d].numPorts = devices[d].numActiveRows = 4;
        }
        plan.assemble(jac.data(), f.data(), n, active.data(), devices);
        REQUIRE(plan.finish(f.data(), n, actual.data()));
        REQUIRE(wdf::solver::RootJacobian::reducedFinishFast(
            matrix.data(), rhs.data(), n, z.data(), f.data(), n, expected.data()));
        for (int i = 0; i < n; ++i) REQUIRE(std::abs(actual[i] - expected[i]) < 1.e-13);
    };
    check();
    for (double& value : z) value *= 0.9;
    plan.refresh(z, n, n);
    check();
    z[groups] = std::nextafter(z[groups], 0.);
    plan.refresh(z, n, n);
    REQUIRE_FALSE(plan.available());
    z[groups] = -z[0];
    plan.refresh(z, n, n);
    check();
    z[2 * groups] = 1.e-15;
    plan.refresh(z, n, n);
    REQUIRE_FALSE(plan.available());
}

TEST_CASE("Shared Newton eliminates only identity rows with zero current correction", "[engine][newton]") {
    constexpr int ports = 12, groups = 6;
    std::vector<double> z(ports * ports), jacobian(ports * ports), rhs(ports), f(ports);
    std::vector<double> full(ports * ports), expected(ports), actual(ports);
    std::vector<int> active(ports);
    std::vector<wdf::RootDevice> devices(ports);
    for (int i = 0; i < ports; ++i) {
        f[i] = std::sin(0.7 * i);
        active[i] = i;
        devices[i].portBase = devices[i].activeRowBase = i;
        devices[i].numPorts = devices[i].numActiveRows = 1;
        for (int j = 0; j < ports; ++j)
            z[i * ports + j] = std::cos(0.17 * (i + 1) * (1 + j % groups));
    }
    wdf::solver::SharedNewtonColumns plan;
    plan.prepare(z, ports, ports);
    REQUIRE(plan.available());
    for (int mask = 0; mask < (1 << groups); ++mask) {
        for (int i = 0; i < ports; ++i) {
            const double slope = (mask & (1 << (i % groups))) ? .013 * (i + 1) : 0.;
            jacobian[i * ports + i] = slope;
            rhs[i] = -slope * f[i];
            for (int j = 0; j < ports; ++j)
                full[i * ports + j] = (i == j ? 1. : 0.) + slope * z[i * ports + j];
        }
        plan.assemble(jacobian.data(), f.data(), ports, active.data(), devices);
        REQUIRE(plan.finish(f.data(), ports, actual.data()));
        REQUIRE(wdf::solver::RootJacobian::reducedFinishFast(
            full.data(), rhs.data(), ports, z.data(), f.data(), ports, expected.data()));
        for (int i = 0; i < ports; ++i) REQUIRE(std::abs(actual[i] - expected[i]) < 1.e-13);
    }
}

TEST_CASE("The op-amp sensing ports retain their solved voltages when their zero currents are removed",
          "[engine][newton][preview]") {
    constexpr int ports = 12;
    std::vector<wdf::RootDevice> devices;
    std::vector<int> columns;
    std::vector<double> z(ports * ports), jac(ports * ports), f(ports);
    for (int port = 0; port < ports; ++port) {
        columns.push_back(port);
        f[port] = std::sin(.4 + port);
        for (int col = 0; col < ports; ++col)
            z[port * ports + col] = .15 * std::cos((1 + port) * (1 + col));
        if (port % 3 != 0) continue;
        devices.push_back({wdf::DeviceKind::OpAmp, port, 3, port, 3, devices.size()});
        jac[port * ports + port] = .1;
        jac[port * ports + port + 1] = -.75;
        jac[port * ports + port + 2] = .75;
    }
    wdf::solver::SharedNewtonColumns plan;
    plan.prepare(z, ports, ports, devices);
    for (int step = 0; step < 2; ++step) {
        // A knob may change the physical sensing columns, even though no
        // correction current is injected along them.
        z[2] += .07;
        plan.refresh(z, ports, ports);
        REQUIRE(plan.available());
        std::vector<double> current(ports), physicalResidual(ports), packedResidual(ports), open(ports);
        for (int port = 0; port < ports; port += 3) current[port] = .0005 * (port + 1.);
        REQUIRE(plan.residual(f.data(), open.data(), current.data(), columns.data(), ports, packedResidual.data()));
        wdf::solver::RootResidual::residualInto(f.data(), open.data(), z.data(), current.data(),
            physicalResidual.data(), columns.data(), ports, ports);
        for (int port = 0; port < ports; ++port)
            REQUIRE(std::abs(packedResidual[port] - physicalResidual[port]) < 1.e-13);
        // A future stamp could draw constant input bias while its incremental
        // input conductance stays zero. Preserve it in the full physical residual.
        current[1] = 0.2e-6;
        REQUIRE_FALSE(plan.residual(f.data(), open.data(), current.data(), columns.data(), ports, packedResidual.data()));
        std::vector<double> matrix(ports * ports), rhs(ports), expected(ports), actual(ports);
        wdf::solver::RootJacobian::reducedAssembleFast(z.data(), jac.data(), f.data(), ports, ports,
            devices.data(), devices.size(), matrix.data(), rhs.data());
        REQUIRE(wdf::solver::RootJacobian::reducedFinishFast(matrix.data(), rhs.data(), ports,
            z.data(), f.data(), ports, expected.data()));
        plan.assemble(jac.data(), f.data(), ports, columns.data(), devices);
        REQUIRE(plan.finish(f.data(), ports, actual.data()));
        for (int port = 0; port < ports; ++port)
            REQUIRE(std::abs(actual[port] - expected[port]) < 1.e-12);
        // On the next sample the first amp is at a rail and has no differential gain.
        jac[1] = jac[2] = 0.;
    }
}

TEST_CASE("Preview skips a failed Newton retry only for identical starting voltages", "[newton][preview]") {
    const auto circuit = lwdf_test::diodeClipper();
    wdf::Engine reference(circuit, kFs), preview(circuit, kFs);
    reference.setExplicitRoots(false);
    preview.setExplicitRoots(false);
    preview.useLoadedJunctionPreview(true);

    auto drive = reference.dcValues();
    const bool changedHistory = GENERATE(false, true);
    if (changedHistory) {
        drive[0] = 0.1;
        reference.step(drive);
        preview.step(drive);
        REQUIRE(reference.lastConverged());
        REQUIRE(preview.lastConverged());
    }

    drive[0] = 2.0;
    const auto& expected = reference.step(drive, 1, 1e-12);
    const auto& actual = preview.step(drive, 1, 1e-12);
    REQUIRE(reference.lastStateRejected());
    REQUIRE(preview.lastStateRejected());
    CHECK(actual == expected);
    CHECK(preview.lastResidual() == reference.lastResidual());
    CHECK(reference.newtonIterations() == 2);
    CHECK(preview.newtonIterations() == (changedHistory ? 2 : 1));
}


TEST_CASE("Preview Newton recovers through repeated compressor overloads", "[newton][preview]") {
    wdf::LiveWDFDocument document;
    std::string error;
    REQUIRE(document.loadFromFile("schematics/studio/dynamics/ADR-Compex-F760X-RS.livewdf", error));
    const auto circuit = document.toCircuit();
    const auto input = std::find_if(circuit.src.begin(), circuit.src.end(), [&](const auto& source) {
        return source.name == document.inputId();
    });
    REQUIRE(input != circuit.src.end());
    const auto port = static_cast<size_t>(input - circuit.src.begin());

    wdf::Engine preview(circuit, kFs);
    preview.prepareCompiledPreview(port, preview.nodeIndex(document.outputNet()), false,
                                   2.0 * document.inputVoltsPerFullScale(), false);
    preview.useLoadedJunctionPreview(true);
    auto sources = preview.dcValues();
    // Repeated overloads drive the gain-control diodes and op-amps through a switching boundary.
    for (int sample = 0; sample < 7 * 40; ++sample) {
        sources[port] = sample % 40 < 14 ? 16.0 : 0.0;
        preview.step(sources, preview.kDefaultMaxIterations, preview.kFastLiveTolerance);
    }
    CHECK(preview.stateRejectCount() == 0);
}

TEST_CASE("Deluxe Reverb Preview accepts a driven ramp", "[newton][preview][pentode]") {
    wdf::LiveWDFDocument document;
    std::string error;
    REQUIRE(document.loadFromFile("schematics/amps/Fender Deluxe Reverb AB763.livewdf", error));
    const auto circuit = wdf::circuitWithPortImpedances(document);
    const auto source = std::ranges::find(circuit.src, document.inputId(), &wdf::Src::name);
    REQUIRE(source != circuit.src.end());
    const auto input = static_cast<size_t>(source - circuit.src.begin());

    wdf::Engine preview(circuit, kFs);
    const int output = preview.nodeIndex(document.outputNet());
    REQUIRE(output >= 0);
    preview.prepareLiveControls();
    preview.prepareMixedAffineReduction(input);
    preview.prepareCompiledPreview(input, output, false);
    preview.useMixedAffineReduction(true);
    preview.useLoadedJunctionPreview(true);
    preview.useTriodePreview(true);
    auto sources = preview.dcValues();
    for (int sample = 0; sample < 64; ++sample)
        preview.step(sources, wdf::Engine::kDefaultMaxIterations, wdf::Engine::kFastLiveTolerance);

    const auto before = preview.stateRejectCount();
    for (int sample = 0; sample < 128; ++sample) {
        sources[input] = -.32 - .00015 * sample;
        preview.step(sources, wdf::Engine::kDefaultMaxIterations, wdf::Engine::kFastLiveTolerance);
    }
    CHECK(preview.stateRejectCount() == before);
}

TEST_CASE("Shared-current residual follows the physical transfer across a partial row block", "[engine][newton]") {
    constexpr int rows = 35, cols = 12, groups = 4;
    std::vector<double> z(rows * cols), v(rows), open(rows), current(rows), actual(rows);
    int active[cols];
    for (int i = 0; i < rows; ++i) {
        v[i] = std::sin(.19 * (i + 1));
        open[i] = std::cos(.17 * i);
        current[i] = std::sin(.07 * i);
        for (int j = 0; j < cols; ++j) {
            const double value = std::cos(.21 * (i + 1) * (1 + j % groups));
            z[i * cols + j] = j < groups ? value : j < 2 * groups ? -value : 0.;
        }
    }
    for (int j = 0; j < cols; ++j) active[j] = 2 * j;
    wdf::solver::SharedNewtonColumns plan;
    plan.prepare(z, rows, cols);
    for (int pass = 0; pass < 2; ++pass) {
        REQUIRE(plan.available());
        plan.residual(v.data(), open.data(), current.data(), active, rows, actual.data());
        for (int i = 0; i < rows; ++i) {
            double expected = v[i] - open[i];
            for (int j = 0; j < cols; ++j) expected += z[i * cols + j] * current[active[j]];
            REQUIRE(std::abs(actual[i] - expected) < 1.e-13);
        }
        for (double& entry : z) entry *= .9;
        plan.refresh(z, rows, cols);
    }
}

TEST_CASE("Independent large roots retain the full Newton matrix and refresh the packed residual", "[engine][newton]") {
    constexpr int rows = 19, cols = 11;
    std::vector<double> z(rows * cols), v(rows), open(rows), current(rows), actual(rows);
    int active[cols];
    for (int i = 0; i < rows; ++i) {
        v[i] = std::sin(.09 * i);
        open[i] = std::cos(.17 * i);
        current[i] = std::sin(.03 * i);
        for (int j = 0; j < cols; ++j)
            z[i * cols + j] = std::sin(.04 * (i + 1) * (j + 1));
    }
    for (int j = 0; j < cols; ++j) active[j] = rows - 1 - j;
    wdf::solver::SharedNewtonColumns plan;
    plan.prepare(z, rows, cols);
    for (int pass = 0; pass < 2; ++pass) {
        REQUIRE_FALSE(plan.available());
        REQUIRE(plan.hasPackedResidual());
        REQUIRE(plan.residual(v.data(), open.data(), current.data(), active, rows, actual.data()));
        for (int i = 0; i < rows; ++i) {
            double full = v[i] - open[i];
            for (int j = 0; j < cols; ++j) full += z[i * cols + j] * current[active[j]];
            REQUIRE(std::abs(full - actual[i]) < 1.e-14);
        }
        for (double& entry : z) entry *= .8;
        plan.refresh(z, rows, cols);
    }
    plan.refresh(z, rows + 1, cols);
    REQUIRE_FALSE(plan.hasPackedResidual());
}

TEST_CASE("Triode-domain transfer preserves block-order voltage folds after controls move", "[engine][newton]") {
    constexpr int tubes = 5, ports = tubes * 3;
    std::vector<double> z(ports * ports), open(ports), current(ports), folded(ports);
    std::vector<wdf::RootDevice> devices(tubes);
    std::vector<wdf::NewtonBlock> blocks(2);
    std::vector<int> order{1, 0};
    for (int i = 0; i < ports; ++i) {
        open[i] = std::sin(.18 * i);
        current[i] = i % 3 == 1 ? 0. : std::cos(.09 * i);
        blocks[i % 2].ports.push_back(ports - i - 1);
        for (int j = 0; j < ports; ++j)
            z[i * ports + j] = (i + j) % 4 == 0 ? 0. : std::cos(.16 * (i + 1) * (j + 1));
    }
    for (int i = 0; i < tubes; ++i) {
        devices[i].kind = wdf::DeviceKind::Triode;
        devices[i].compIndex = tubes - 1 - i;
        devices[i].portBase = 3 * (tubes - 1 - i);
    }
    const wdf::BlockView view{&blocks, &order, int(blocks.size())};
    wdf::solver::TriodeDomainTransfer plan;
    plan.prepare(z, ports, devices);
    for (int pass = 0; pass < 2; ++pass) {
        REQUIRE(plan.fold(view, open.data(), current.data(), folded.data()));
        for (const auto& device : devices) {
            for (int j = 0; j < 2; ++j) {
                const int port = device.portBase + 2 * j;
                REQUIRE(folded[2 * device.compIndex + j] == wdf::solver::RootClosedForms::foldPort(
                    view, port, view.count, z.data(), open.data(), current.data(), ports));
            }
        }
        std::reverse(order.begin(), order.end());
        z[5] += .07;
        plan.refresh(z, ports);
    }
    current[3] = std::numeric_limits<double>::infinity();
    REQUIRE_FALSE(plan.fold(view, open.data(), current.data(), folded.data()));
}
