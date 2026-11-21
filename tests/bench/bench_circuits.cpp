// Manual multi-circuit micro-benchmark: the generic engine across the fixture
// library, not just `rc_lowpass` and the one paper stage.
//
// NOT a correctness gate and NOT registered with CTest -- it is an instrument.
// Build target `livewdf_bench_circuits`; run it by hand:
//
//     build\tests\Release\livewdf_bench_circuits.exe
//
// Why this exists: `bench_stage.cpp` answers "how close does the generic engine
// get to a hand-built closed-form stage?" on a single topology, and a single
// topology is not enough to decide anything about the compiler or the engine.
// `rc_lowpass` is N=1, P=2, K=0 -- no device root, no Newton, no reactive state
// worth the name -- so a decision made on it is a decision about the wrong
// circuit. This instrument runs the whole `CircuitCases` library and reports,
// per circuit:
//
//   * shape        N nodes / P elements / K root ports
//   * formulation  whether the exact closed-form root is eligible, how many
//                  Newton iterations per sample it costs when it is not
//   * cost         ns/sample (median of repeated measured runs, with spread),
//                  and the phase shares
//   * agreement    max |explicit - Newton| on the same topology, so the fast
//                  path is checked against the fallback on every circuit that
//                  can take it rather than only on the paper stage
//   * avoidable    how many columns of `Xe*w` are provably zero from structure
//                  alone -- the measurement a sparse matvec has to beat
//   * independence which root-port groups can be solved one at a time. This is
//                  the structural fact a per-stage closed form needs, and the
//                  one Cathodyne hardcodes: a coupling cap makes `Z` nonzero in
//                  both directions, but a triode's grid port carries no
//                  current, so the coupling is one-way once `J` multiplies it.
//
// Two triodes (`two_stage_cascade`) is the important row: it closes the
// closed-form gate, so it is the only fixture here that prices the multiport
// Newton on a realistic topology -- and the only one the independence table has
// more than a single group to say anything about.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "format/LiveWDFFile.h"
#include "wdf/Engine.h"
#include "support/CircuitCases.h"
#include "support/RootGroups.h"

namespace {

constexpr double kFs = 48000.0;
constexpr int kSamples = 48000;  // one second
constexpr int kRepeats = 7;      // odd: median plus min/max spread
constexpr int kWarmupSamples = 2048;  // untimed, before the measured repeats

// `--tol <value>` overrides the solver stop tolerance for measurement only: the
// engine default is 1e-9. The block solver floors its acceptance at 1e-9
// (`std::max(tol, 1e-9)`), so values below that read as 1e-9 for the block
// path. The floor is the achievable block residual on the shared-cathode and
// op-amp blocks; a tighter stop tolerance asks the Newton loop for precision
// the block formulation cannot deliver and sends every sample to the joint
// fallback.
static double kBenchTol = 1e-9;
static bool kTolOverride = false;
static bool kFineBlocks = false;
static bool kPrintBlocks = false;
static std::string kPotId;
static double kPotPos = -1.0;
static long long kPotAtSample = -1;
static double kPotAtPos = 1.0;
static std::vector<std::string> kSwitchIds;
static long long kSwitchAtSample = -1;
static long long kSwitchPeriod = 0;
static long long kSwitchCount = -1;
static double kDriveVolts = 0.25;
static bool kGridCurrent = true;
static std::string kPentodeModel = "reefman-d";

static void applyPentodeModel(wdf::Circuit& c) {
    if (kPentodeModel == "koren") {
        for (auto& pt : c.pent) {
            pt.model.modelKind = wdf::PentodeModelKind::Koren;
            pt.model.useReefman = false;
        }
        return;
    }
    if (kPentodeModel == "reefman-e" || kPentodeModel == "reefman-derke") {
        for (auto& pt : c.pent) {
            pt.model.modelKind = wdf::PentodeModelKind::Reefman;
            pt.model.useReefman = true;
            pt.model.reefman.knee = wdf::PentodeKneeType::DerkE;
        }
        return;
    }
    if (kPentodeModel == "reefman-se" || kPentodeModel == "reefman-d-se") {
        for (auto& pt : c.pent) {
            pt.model.modelKind = wdf::PentodeModelKind::Reefman;
            pt.model.useReefman = true;
            pt.model.reefman.knee = wdf::PentodeKneeType::DerkD;
            pt.model.reefman.secondaryEmission = true;
        }
        return;
    }
    // Default: reefman-d
    for (auto& pt : c.pent) {
        pt.model.modelKind = wdf::PentodeModelKind::Reefman;
        pt.model.useReefman = true;
        pt.model.reefman.knee = wdf::PentodeKneeType::DerkD;
        pt.model.reefman.secondaryEmission = false;
    }
}

struct Result {
    double nsPerSample = 0.0;  // median over kRepeats
    double nsMin = 0.0;
    double nsMax = 0.0;
    double checksum = 0.0;
    double peak = 0.0;
    int nonFinite = 0;  // samples whose output was NaN/inf; `peak` cannot see them
    int converged = 0;
    double itersPerSample = 0.0;
    double lineTrialsPerSample = 0.0;
    double deviceEvalsPerSample = 0.0;
    long long jfetFallbacks = 0;
    long long blockFallbacks = 0;
    double lastBlockResidual = 0.0;
    bool lastBlockFallback = false;
    long long stateRejects = 0;
    bool explicitActive = false;
    int nodes = 0;
    int elements = 0;
    int roots = 0;
    int blocks = 0;
    int blockPorts[16] = {};
    long long runtimeBlockSamples = 0;
    long long blockStructureSamples = 0;
    long long stateRejectSamples = 0;
    long long gridLimitHitsTotal = 0;
    int deadColumns = 0;
    wdf::Engine::PhaseProfile prof;
    std::vector<double> out;
};

/// Median of the measured repeats, plus min/max for the reported spread.
/// Keeping every sample (rather than the minimum) makes a noisy machine visible
/// instead of hiding it behind the best run.
void summarizeTiming(std::array<double, kRepeats> times, Result& r) {
    std::sort(times.begin(), times.end());
    r.nsMin = times.front();
    r.nsMax = times.back();
    r.nsPerSample = times[times.size() / 2];
}

double spreadPercent(const Result& r) {
    return r.nsPerSample > 0.0 ? 100.0 * (r.nsMax - r.nsMin) / r.nsPerSample : 0.0;
}

/// The node whose trace the run records: the fixture's output if it names one,
/// otherwise the last free node (which is where these fixtures put it).
int outputIndex(const wdf::Engine& e) {
    for (const char* nm : {"out", "o"}) {
        const int i = e.nodeIndex(nm);
        if (i >= 0)
            return i;
    }
    const auto& names = e.nodeNames();
    return names.empty() ? -1 : static_cast<int>(names.size()) - 1;
}

/// Columns of `u = Xe*w` that are provably zero on every sample, from structure
/// alone: `w_j = b_j - eoff_j`, and `b_j` is nonzero only for a reactive element
/// while `eoff_j` is nonzero only for an element tied to an ideal source. Same
/// test as `bench_stage.cpp::printColumns`, applied to whatever circuit is here.
int deadColumns(const wdf::Engine& e) {
    const std::vector<int>& capOf = e.capOf();
    const std::vector<int>& indOf = e.indOf();
    const std::vector<wdf::AffineOffset>& off = e.elementOffsets();
    int dead = 0;
    for (int j = 0; j < e.elementCount(); ++j) {
        const bool reactive = capOf[static_cast<size_t>(j)] >= 0 || indOf[static_cast<size_t>(j)] >= 0;
        const bool coupled = !off[static_cast<size_t>(j)].coef.empty() || off[static_cast<size_t>(j)].c0 != 0.0;
        if (!reactive && !coupled)
            ++dead;
    }
    return dead;
}

Result run(const lwdf_test::CircuitCase& tc, bool allowExplicit, bool profile) {
    Result r;
    const wdf::Circuit c = (*tc.build)();
    wdf::Engine e(c, kFs, kGridCurrent);
    if (!allowExplicit)
        e.setExplicitRoots(false);
    r.explicitActive = e.explicitRoots();
    r.nodes = e.nodeCount();
    r.elements = e.elementCount();
    r.roots = e.rootCount();
    r.deadColumns = deadColumns(e);
    if (profile)
        e.setProfile(true);

    std::vector<double> node, rootV;
    if (!e.solveDc(node, rootV)) {
        std::printf("  %s: solveDc did not converge\n", tc.name);
        return r;
    }
    const int outIdx = outputIndex(e);

    // One source vector and the whole input sequence, built before the timed
    // loop. Two artifacts to keep out of the timing: allocating a source vector
    // per sample puts the allocator inside the loop, and calling `std::sin` per
    // sample (the fixture's input) charges the engine for the test's arithmetic.
    // `bench_stage.cpp` does include its `std::sin`; the `sin` baseline printed
    // once at the end is the bridge between the two instruments.
    std::vector<double> sv(c.src.size(), 0.0);
    for (size_t i = 0; i < c.src.size(); ++i)
        sv[i] = c.src[i].dc;

    std::vector<double> in(static_cast<size_t>(kSamples), 0.0);
    for (int n = 0; n < kSamples; ++n)
        in[static_cast<size_t>(n)] = (*tc.input)(n);

    // Warm caches and branch predictors once; this pass is not measured.
    e.reset();
    for (int n = 0; n < kWarmupSamples; ++n) {
        if (!sv.empty())
            sv[0] = in[static_cast<size_t>(n % kSamples)];
        if (kTolOverride)
            e.step(sv, 60, kBenchTol);
        else
            e.step(sv);
    }

    const long long fallbacksBefore = e.jfetFallbackCount();
    std::array<double, kRepeats> times{};
    long long iters = 0;
    for (int rep = 0; rep < kRepeats; ++rep) {
        e.reset();
        e.resetPhaseProfile();
        r.out.assign(static_cast<size_t>(kSamples), 0.0);
        double acc = 0.0;
        r.peak = 0.0;
        r.nonFinite = 0;
        r.converged = 0;
        r.runtimeBlockSamples = 0;
        r.blockStructureSamples = 0;
        r.stateRejectSamples = 0;
        r.gridLimitHitsTotal = 0;

        const auto t0 = std::chrono::steady_clock::now();
        for (int n = 0; n < kSamples; ++n) {
            if (!sv.empty())
                sv[0] = in[static_cast<size_t>(n)];
            const std::vector<double>& y = kTolOverride ? e.step(sv, 60, kBenchTol) : e.step(sv);
            const double v = outIdx >= 0 ? y[static_cast<size_t>(outIdx)] : 0.0;
            r.out[static_cast<size_t>(n)] = v;
            acc += v;
            // `std::max` ignores a NaN comparison, so `peak` reports a quiet
            // waveform for a network that is producing NaN. Count them instead.
            if (!std::isfinite(v))
                ++r.nonFinite;
            r.peak = std::max(r.peak, std::abs(v));
            if (e.lastConverged())
                ++r.converged;
            // `step()` zeroes its own counter at the top, so this has to be read
            // *inside* the sample loop. Reading it once after the loop measures
            // one sample's iterations and divides them by all of them, which is
            // how this column used to print 0.00 for every Newton circuit.
            iters += e.newtonIterations();
            if (e.lastRuntimeBlockUsed())
                ++r.runtimeBlockSamples;
            if (e.rootBlockCount() > 0)
                ++r.blockStructureSamples;
            if (e.lastStateRejected())
                ++r.stateRejectSamples;
            r.gridLimitHitsTotal += e.gridLimitHits();
        }
        const auto t1 = std::chrono::steady_clock::now();
        times[static_cast<size_t>(rep)] =
            std::chrono::duration<double>(t1 - t0).count() / kSamples * 1e9;
        r.checksum = acc;
    }
    summarizeTiming(times, r);
    r.itersPerSample = static_cast<double>(iters) / (kSamples * kRepeats);
    r.jfetFallbacks = e.jfetFallbackCount() - fallbacksBefore;
    r.prof = e.phaseProfile();
    return r;
}

const char* deviceKindName(wdf::DeviceKind k) {
    switch (k) {
        case wdf::DeviceKind::Triode: return "triode";
        case wdf::DeviceKind::Pentode: return "pentode";
        case wdf::DeviceKind::Diode: return "diode";
        case wdf::DeviceKind::OpAmp: return "opamp";
        case wdf::DeviceKind::ControlledRes: return "ctrlres";
        case wdf::DeviceKind::Jfet: return "jfet";
        case wdf::DeviceKind::Mosfet: return "mosfet";
        case wdf::DeviceKind::Bjt: return "bjt";
    }
    return "?";
}

/// The same timed loop as `run()`, but for a `.livewdf` document rather than a
/// hard-coded fixture. The input source and output node come from the document,
/// not from the fixture convention that source 0 is the input and `out` is the
/// output -- a real sheet is allowed to name them anything.
///
/// `channels` builds independent engines exactly as the plugin does for stereo.
/// The result is per engine sample (all channels), so it is directly comparable
/// to the engine half of the plugin's block time.
Result runDocument(const wdf::LiveWDFDocument& doc, double fs, int channels,
                   bool allowExplicit, bool profile) {
    Result r;
    wdf::Circuit c = doc.toCircuit();
    applyPentodeModel(c);

    if (const char* dn = std::getenv("LIVEWDF_DUMP_NODE")) {
        const std::string node = dn;
        const auto hit = [&](const std::string& s) { return s == node; };
        std::printf("node %s connections:\n", dn);
        for (const auto& e : c.res)
            if (hit(e.a) || hit(e.b))
                std::printf("  R   %-12s % .6g  %s - %s\n", e.name.c_str(), e.value, e.a.c_str(), e.b.c_str());
        for (const auto& e : c.cap)
            if (hit(e.a) || hit(e.b))
                std::printf("  C   %-12s % .6g  %s - %s\n", e.name.c_str(), e.value, e.a.c_str(), e.b.c_str());
        for (const auto& e : c.ind)
            if (hit(e.a) || hit(e.b))
                std::printf("  L   %-12s % .6g  %s - %s\n", e.name.c_str(), e.value, e.a.c_str(), e.b.c_str());
        for (const auto& e : c.tri)
            if (hit(e.g) || hit(e.p) || hit(e.k) || hit(e.gi))
                std::printf("  TRI %-12s g=%s p=%s k=%s gi=%s\n", e.name.c_str(), e.g.c_str(), e.p.c_str(), e.k.c_str(), e.gi.c_str());
        for (const auto& e : c.pent)
            if (hit(e.p) || hit(e.s) || hit(e.g) || hit(e.k))
                std::printf("  PEN %-12s p=%s s=%s g=%s k=%s\n", e.name.c_str(), e.p.c_str(), e.s.c_str(), e.g.c_str(), e.k.c_str());
        for (const auto& e : c.diode)
            if (hit(e.a) || hit(e.b))
                std::printf("  D   %-12s %s - %s\n", e.name.c_str(), e.a.c_str(), e.b.c_str());
        for (const auto& e : c.opamp)
            if (hit(e.out) || hit(e.neg) || hit(e.pos) || hit(e.vcc) || hit(e.vee))
                std::printf("  OP  %-12s out=%s neg=%s pos=%s vcc=%s vee=%s\n", e.name.c_str(), e.out.c_str(),
                            e.neg.c_str(), e.pos.c_str(), e.vcc.c_str(), e.vee.c_str());
        for (const auto& e : c.vcvs)
            if (hit(e.outA) || hit(e.outB) || hit(e.inA) || hit(e.inB))
                std::printf("  E   %-12s out=%s,%s in=%s,%s gain=%g\n", e.name.c_str(), e.outA.c_str(), e.outB.c_str(),
                            e.inA.c_str(), e.inB.c_str(), e.gain);
    }

    std::vector<std::unique_ptr<wdf::Engine>> engines;
    engines.reserve(static_cast<size_t>(channels));
    for (int ch = 0; ch < channels; ++ch) {
        auto e = std::make_unique<wdf::Engine>(c, fs, kGridCurrent);
        if (!allowExplicit)
            e->setExplicitRoots(false);
        if (kFineBlocks)
            e->setFineBlockSolver(true);
        engines.push_back(std::move(e));
    }

    if (!kPotId.empty()) {
        for (auto& e : engines) {
            if (!e->setPotPosition(kPotId, kPotPos))
                std::printf("  pot override failed: '%s' (position %.3f)\n",
                            kPotId.c_str(), kPotPos);
        }
    }

    auto& first = *engines.front();
    r.explicitActive = first.explicitRoots();
    r.nodes = first.nodeCount();
    r.elements = first.elementCount();
    r.roots = first.rootCount();
    r.blocks = first.rootBlockCount();
    for (int s = 0; s < r.blocks && s < 16; ++s)
        r.blockPorts[s] = first.rootBlockPortCount(s);
    r.deadColumns = deadColumns(first);
    if (kPrintBlocks) {
        for (int s = 0; s < r.blocks; ++s) {
            std::printf("  block %d: %d ports, %d devices:", s, first.rootBlockPortCount(s),
                        first.rootBlockDeviceCount(s));
            for (int j = 0; j < first.rootBlockDeviceCount(s); ++j) {
                const int di = first.rootBlockDevice(s, j);
                std::printf(" %s#%d(%dp)", deviceKindName(first.rootDeviceKind(di)),
                            first.rootDeviceComp(di), first.rootDevicePortCount(di));
            }
            std::printf("\n");
        }
    }
    if (profile)
        first.setProfile(true);

    for (auto& e : engines) {
        std::vector<double> node, rootV;
        const auto dcStart = std::chrono::steady_clock::now();
        const bool dcOk = e->solveDc(node, rootV);
        if (std::getenv("LIVEWDF_DUMP_DC"))
            std::printf("solveDc %s in %.1f ms\n", dcOk ? "ok" : "fail",
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - dcStart)
                            .count());
        if (!dcOk) {
            std::printf("  %s: solveDc did not converge\n", doc.name.c_str());
            return r;
        }
        if (e.get() == engines.front().get() && std::getenv("LIVEWDF_DUMP_DC")) {
            std::printf("DC operating point (%d nodes):\n", e->nodeCount());
            for (int n = 0; n < e->nodeCount(); ++n)
                std::printf("  %-28s % .9f\n", e->nodeNames()[static_cast<size_t>(n)].c_str(),
                            node[static_cast<size_t>(n)]);
        }
        e->reset();
    }

    int outIdx = first.nodeIndex(doc.outputNet());
    if (outIdx < 0)
        outIdx = outputIndex(first);

    int inIdx = -1;
    const std::string inputId = doc.inputId();
    for (size_t i = 0; i < c.src.size(); ++i) {
        if (!inputId.empty() && c.src[i].name == inputId) {
            inIdx = static_cast<int>(i);
            break;
        }
    }
    if (inIdx < 0 && !c.src.empty())
        inIdx = 0;

    std::vector<double> sv(c.src.size(), 0.0);
    for (size_t i = 0; i < c.src.size(); ++i)
        sv[i] = c.src[i].dc;

    // One second at the engine's rate, and the same 440 Hz / 0.25 input the
    // plugin benchmark drives.
    const int samples = static_cast<int>(fs);
    std::vector<double> in(static_cast<size_t>(samples), 0.0);
    const double w = 2.0 * 3.14159265358979323846 * 440.0 / fs;
    for (int n = 0; n < samples; ++n)
        in[static_cast<size_t>(n)] = kDriveVolts * std::sin(w * n);

    // Warm every channel once; this pass is not measured.
    for (auto& e : engines)
        e->reset();
    for (int n = 0; n < kWarmupSamples; ++n) {
        if (inIdx >= 0)
            sv[static_cast<size_t>(inIdx)] = in[static_cast<size_t>(n % samples)];
        if (kTolOverride)
            first.step(sv, 60, kBenchTol);
        else
            first.step(sv);
        for (size_t e = 1; e < engines.size(); ++e) {
            if (kTolOverride)
                engines[e]->step(sv, 60, kBenchTol);
            else
                engines[e]->step(sv);
        }
    }

    const long long fallbacksBefore = first.jfetFallbackCount();
    const long long blockFallbacksBefore = first.blockFallbackCount();
    const long long stateRejectsBefore = first.stateRejectCount();
    std::array<double, kRepeats> times{};
    long long iters = 0;
    long long trials = 0;
    long long devEvals = 0;
    const bool trace = std::getenv("LIVEWDF_TRACE") != nullptr;
    const auto& traceNames = first.nodeNames();
    double traceMax = 0.0;
    int traceNode = -1;
    long long traceSample = -1;
    bool traceNonfinite = false;
    for (int rep = 0; rep < kRepeats; ++rep) {
        for (auto& e : engines)
            e->reset();
        first.resetPhaseProfile();
        r.out.assign(static_cast<size_t>(samples), 0.0);
        double acc = 0.0;
        r.peak = 0.0;
        r.nonFinite = 0;
        r.converged = 0;
        r.runtimeBlockSamples = 0;
        r.blockStructureSamples = 0;
        r.stateRejectSamples = 0;
        r.gridLimitHitsTotal = 0;

        const auto t0 = std::chrono::steady_clock::now();
        for (int n = 0; n < samples; ++n) {
            if (inIdx >= 0)
                sv[static_cast<size_t>(inIdx)] = in[static_cast<size_t>(n)];

            if (kPotAtSample >= 0 && n == kPotAtSample) {
                for (auto& e : engines)
                    e->setPotPosition(kPotId, kPotAtPos);
            }

            if (kSwitchAtSample >= 0 && !kSwitchIds.empty()) {
                const bool due = kSwitchPeriod > 0
                                     ? (n >= kSwitchAtSample && (n - kSwitchAtSample) % kSwitchPeriod == 0)
                                     : (n == kSwitchAtSample);
                if (due) {
                    const long long step = kSwitchPeriod > 0 ? (n - kSwitchAtSample) / kSwitchPeriod : 0;
                    if (kSwitchCount < 0 || step < kSwitchCount) {
                        const bool throwB = (step % 2) != 0;
                        const double rA = throwB ? wdf::kSpdtOpenOhms : wdf::kSpdtClosedOhms;
                        const double rB = throwB ? wdf::kSpdtClosedOhms : wdf::kSpdtOpenOhms;
                        std::vector<std::pair<std::string, double>> updates;
                        updates.reserve(kSwitchIds.size() * 2);
                        for (const auto& id : kSwitchIds) {
                            updates.emplace_back(id + "_A", rA);
                            updates.emplace_back(id + "_B", rB);
                        }
                        for (auto& e : engines)
                            e->setResistances(updates);
                    }
                }
            }

            const std::vector<double>* yp = nullptr;
            if (kTolOverride)
                yp = &first.step(sv, 60, kBenchTol);
            else
                yp = &first.step(sv);
            for (size_t e = 1; e < engines.size(); ++e) {
                if (kTolOverride)
                    engines[e]->step(sv, 60, kBenchTol);
                else
                    engines[e]->step(sv);
            }
            const std::vector<double>& y = *yp;

            const double v = outIdx >= 0 ? y[static_cast<size_t>(outIdx)] : 0.0;
            r.out[static_cast<size_t>(n)] = v;
            acc += v;
            if (!std::isfinite(v))
                ++r.nonFinite;
            r.peak = std::max(r.peak, std::abs(v));
            if (first.lastConverged())
                ++r.converged;
            iters += first.newtonIterations();
            trials += first.lineSearchTrials();
            devEvals += first.deviceEvaluations();
            if (first.lastRuntimeBlockUsed())
                ++r.runtimeBlockSamples;
            if (first.rootBlockCount() > 0)
                ++r.blockStructureSamples;
            if (first.lastStateRejected())
                ++r.stateRejectSamples;
            r.gridLimitHitsTotal += first.gridLimitHits();
            if (trace && rep == 0) {
                for (size_t i = 0; i < y.size(); ++i) {
                    if (!std::isfinite(y[i])) {
                        if (!traceNonfinite) {
                            std::printf("  trace: first nonfinite at sample %d node '%s' = %g\n",
                                        n, traceNames[i].c_str(), y[i]);
                            traceNonfinite = true;
                        }
                    } else if (std::abs(y[i]) > traceMax) {
                        traceMax = std::abs(y[i]);
                        traceNode = static_cast<int>(i);
                        traceSample = n;
                    }
                }
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        times[static_cast<size_t>(rep)] =
            std::chrono::duration<double>(t1 - t0).count() / samples * 1e9;
        if (trace && rep == 0) {
            if (traceNode >= 0)
                std::printf("  trace: max node '%s' = %.6g at sample %lld\n",
                            traceNames[static_cast<size_t>(traceNode)].c_str(), traceMax,
                            traceSample);
            else
                std::printf("  trace: no finite node voltages\n");
        }
        r.checksum = acc;
    }
    summarizeTiming(times, r);
    r.itersPerSample = static_cast<double>(iters) / (samples * kRepeats);
    r.lineTrialsPerSample = static_cast<double>(trials) / (samples * kRepeats);
    r.deviceEvalsPerSample = static_cast<double>(devEvals) / (samples * kRepeats);
    r.jfetFallbacks = first.jfetFallbackCount() - fallbacksBefore;
    r.blockFallbacks = first.blockFallbackCount() - blockFallbacksBefore;
    r.lastBlockResidual = first.lastBlockResidual();
    r.lastBlockFallback = first.lastBlockFallback();
    r.stateRejects = first.stateRejectCount() - stateRejectsBefore;
    r.prof = first.phaseProfile();
    return r;
}

/// Cost of generating one sample of the fixture input (`std::sin`), best of a
/// few runs. Printed once so the gap to `bench_stage.cpp` -- whose timing loop
/// includes its own `std::sin` -- is accounted for rather than guessed at.
double sinBaselineNs() {
    double best = 1e300;
    for (int rep = 0; rep < kRepeats; ++rep) {
        double acc = 0.0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int n = 0; n < kSamples; ++n)
            acc += std::sin(0.001 * n);
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double>(t1 - t0).count() / kSamples * 1e9);
        if (acc == 12345.0)
            std::printf("");
    }
    return best;
}

double maxDiff(const std::vector<double>& a, const std::vector<double>& b) {
    double m = 0.0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        m = std::max(m, std::abs(a[i] - b[i]));
    return m;
}

/// Compact phase shares, in percent of the timed phases. Absolute ns per phase
/// are not comparable across runs once the clock is read inside `step()`, so
/// only the shares are printed here.
///
/// For a circuit with no root ports the share split is *clock reads*, not work:
/// there is nothing to time but the tick/tock pairs. That case prints a note
/// instead of a table that would read as if `device` were doing something.
void printPhaseShares(const char* name, int roots, const wdf::Engine::PhaseProfile& p) {
    const double total = p.total();
    if (roots == 0) {
        std::printf("    %-26s (linear, no root ports: phase shares are clock reads)\n", name);
        return;
    }
    if (p.samples <= 0 || total <= 0.0) {
        std::printf("    %-26s (no timed phases)\n", name);
        return;
    }
    const double inv = 1.0e9 / static_cast<double>(p.samples);
    std::printf("    %-26s offs %5.1f  net %5.1f  solve %5.1f  post %5.1f ns/sample\n",
                name, p.offsets * inv, p.network * inv, p.solve * inv, p.post * inv);
    if (p.solve > 0.0) {
        const double inside = p.device + p.jacobian + p.lu + p.linesearch;
        std::printf("    %-26s   solve: dev %5.1f  jac %5.1f  lu %5.1f  line %5.1f  block %5.1f\n",
                    "", p.device * inv, p.jacobian * inv, p.lu * inv, p.linesearch * inv,
                    (p.solve - inside) * inv);
    }
}

// ---------------------------------------------------------------------- //
// Independence of the root-port groups.
//
// The measurement itself lives in `support/RootGroups.h` so the test can assert
// the same numbers this prints -- see that header for the derivation. In short:
// the residual coupling between two device groups is `Z*J`, not `Z`; a
// coupling capacitor makes `Z` nonzero in both directions, but a triode's
// grid port carries no current, so `J`'s grid entries are zero and the
// backwards coupling is annihilated. Solving group by group in that order is
// then exact -- not the approximation the reference makes with its handoff.
void printCouplingMatrix(const char* title, const std::vector<double>& m, int n) {
    std::printf("    %-20s", title);
    for (int h = 0; h < n; ++h)
        std::printf("  %9d", h + 1);
    std::printf("\n");
    for (int g = 0; g < n; ++g) {
        std::printf("    %-20s", g == 0 ? "rows = group" : "");
        for (int h = 0; h < n; ++h)
            std::printf("  %9.2e", m[static_cast<size_t>(g) * n + h]);
        std::printf("\n");
    }
}

void printCoupling(const lwdf_test::CircuitCase& tc, const lwdf_test::RootGroupCoupling& cp) {
    if (cp.nGroups() < 2)
        return;
    const int n = cp.nGroups();
    const double diag = cp.minDiagonal();
    const double above = cp.maxAboveDiagonal();
    std::printf("  %s: %d group(s) -- %d triode (3 ports each), %d diode (1 port each)\n", tc.name, n,
                cp.nTri, cp.nD);
    printCouplingMatrix("max |Z[r][c]|", cp.z, n);
    printCouplingMatrix("max |(Z*J)[r][c]|", cp.zj, n);
    std::printf("    min |Z*J| on the diagonal %.3e, max above it %.3e  (ratio %.1e)\n", diag, above,
                diag > 0.0 ? above / diag : 0.0);
    std::printf("    -> %s\n",
                above == 0.0
                    ? "above-diagonal blocks are exactly zero: group-by-group in this order is exact"
                    : "above-diagonal blocks are nonzero: this order needs iteration between groups");

    // The same judgement, made from `Z` alone: the only column of `J` that is
    // ever nonzero is a group's live column -- `pk_t` for a triode and the
    // diode's own port for a diode. So a compile-time test can decide coupling
    // from `Z` alone, with no per-sample `J` -- and the test suite asserts this
    // prediction equals the measured `Z*J` pattern.
    printCouplingMatrix("max |Z[r][live_h]|", cp.zLive, n);
    std::printf("    compile-time prediction (from Z alone):");
    for (int g = 0; g < n; ++g)
        for (int h = 0; h < n; ++h)
            if (cp.predictedCoupled(g, h))
                std::printf(" %d<-%d", g + 1, h + 1);
    std::printf("\n");
}

/// Real-playing proxy: the steady 440 Hz tone is the cheapest hard case, not
/// the worst one. Gate it into 120 ms bursts and time the engine in host-sized
/// blocks, so the transient samples that trip the block solver into its joint
/// fallback show up as dropped host blocks instead of hiding in a mean.
void runBurst(const wdf::LiveWDFDocument& doc, double fs, int hostBlock, double seconds) {
    wdf::Circuit c = doc.toCircuit();
    applyPentodeModel(c);
    wdf::Engine e(c, fs, kGridCurrent);
    if (!kPotId.empty())
        e.setPotPosition(kPotId, kPotPos);
    std::vector<double> node, rootV;
    if (!e.solveDc(node, rootV)) {
        std::printf("burst: solveDc did not converge\n");
        return;
    }
    e.reset();

    int inIdx = -1;
    const std::string inputId = doc.inputId();
    for (size_t i = 0; i < c.src.size(); ++i) {
        if (!inputId.empty() && c.src[i].name == inputId) {
            inIdx = static_cast<int>(i);
            break;
        }
    }
    if (inIdx < 0 && !c.src.empty())
        inIdx = 0;
    int outIdx = e.nodeIndex(doc.outputNet());
    if (outIdx < 0)
        outIdx = outputIndex(e);

    std::vector<double> sv(c.src.size(), 0.0);
    for (size_t i = 0; i < c.src.size(); ++i)
        sv[i] = c.src[i].dc;

    const double w = 2.0 * 3.14159265358979323846 * 440.0 / fs;
    const auto stepOnce = [&](const std::vector<double>& sources) -> const std::vector<double>& {
        return kTolOverride ? e.step(sources, 60, kBenchTol) : e.step(sources);
    };
    const auto inputAt = [&](long long n) {
        const double phase = std::fmod(static_cast<double>(n) / fs, 0.2);
        const double env = phase < 0.12 ? 1.0 : 0.0;
        return 0.25 * env * std::sin(w * static_cast<double>(n));
    };

    // Warm caches, then count from a clean slate: silence into the first attack.
    for (int k = 0; k < 400; ++k) {
        if (inIdx >= 0)
            sv[static_cast<size_t>(inIdx)] = 0.0;
        stepOnce(sv);
    }

    const long long total = static_cast<long long>(seconds * fs);
    const int blocks = static_cast<int>(total / hostBlock);
    std::vector<double> blockMs;
    blockMs.reserve(static_cast<size_t>(blocks));
    long long iters = 0;
    long long gridLimits = 0;
    const long long fbBefore = e.blockFallbackCount();
    int nonfinite = 0, unconverged = 0, maxIter = 0, overruns = 0;
    double peak = 0.0, worstMs = 0.0;
    long long n = 0;

    for (int b = 0; b < blocks; ++b) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < hostBlock; ++k, ++n) {
            if (inIdx >= 0)
                sv[static_cast<size_t>(inIdx)] = inputAt(n);
            const auto& y = stepOnce(sv);
            if (outIdx >= 0) {
                const double v = y[static_cast<size_t>(outIdx)];
                if (std::isfinite(v))
                    peak = std::max(peak, std::abs(v));
                else
                    ++nonfinite;
            }
            const int it = static_cast<int>(e.newtonIterations());
            iters += it;
            gridLimits += e.gridLimitHits();
            maxIter = std::max(maxIter, it);
            if (!e.lastConverged())
                ++unconverged;
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        blockMs.push_back(ms);
        worstMs = std::max(worstMs, ms);
        if (ms > 1000.0 * hostBlock / fs)
            ++overruns;
    }

    const long long fallbacks = e.blockFallbackCount() - fbBefore;
    std::sort(blockMs.begin(), blockMs.end());
    const auto pct = [&](double p) {
        const size_t i = std::min(blockMs.size() - 1,
                                  static_cast<size_t>(p * static_cast<double>(blockMs.size())));
        return blockMs[i];
    };
    std::printf("\nburst: %.1f s @ %.0f Hz, host block %d, 120 ms on / 80 ms off, 0.25 V 440 Hz\n",
                seconds, fs, hostBlock);
    std::printf("  mean it/sample %.2f (max %d)   grid-limit hits/sample %.2f   block fallbacks %lld (%.2f%% of samples)   unconverged %d   nonfinite %d   peak %.6g\n",
                static_cast<double>(iters) / static_cast<double>(n), maxIter,
                static_cast<double>(gridLimits) / static_cast<double>(n), fallbacks,
                100.0 * static_cast<double>(fallbacks) / static_cast<double>(n), unconverged,
                nonfinite, peak);
    std::printf("  block time: p50 %.3f ms   p99 %.3f ms   p99.9 %.3f ms   max %.3f ms   overruns %d/%d (deadline %.3f ms)\n",
                pct(0.5), pct(0.99), pct(0.999), worstMs, overruns, blocks,
                1000.0 * hostBlock / fs);
}

/// Silence soak: drive the circuit for a while, then hold the input at zero
/// and print the watched nodes. A supply or bias network that keeps charging
/// after the signal stops shows up here as a slow drift in the node columns and
/// a rising iteration count.
void runSoak(const wdf::LiveWDFDocument& doc, double fs, double driveSeconds,
             double silenceSeconds, double driveVolts) {
    wdf::Circuit c = doc.toCircuit();
    applyPentodeModel(c);
    wdf::Engine e(c, fs, kGridCurrent);
    std::vector<double> node, rootV;
    if (!e.solveDc(node, rootV)) {
        std::printf("soak: solveDc did not converge\n");
        return;
    }
    e.reset();

    int inIdx = -1;
    const std::string inputId = doc.inputId();
    for (size_t i = 0; i < c.src.size(); ++i) {
        if (!inputId.empty() && c.src[i].name == inputId) {
            inIdx = static_cast<int>(i);
            break;
        }
    }
    if (inIdx < 0 && !c.src.empty())
        inIdx = 0;
    int outIdx = e.nodeIndex(doc.outputNet());
    if (outIdx < 0)
        outIdx = outputIndex(e);

    std::vector<double> sv(c.src.size(), 0.0);
    for (size_t i = 0; i < c.src.size(); ++i)
        sv[i] = c.src[i].dc;

    std::vector<std::pair<std::string, int>> watch;
    for (const char* name : {"n26", "n49", "n19", "n34", "n25", "n7", "n13", "n16",
                             "n35", "n24", "n47", "out"}) {
        const int idx = e.nodeIndex(name);
        if (idx >= 0)
            watch.emplace_back(name, idx);
    }

    const double w = 2.0 * 3.14159265358979323846 * 440.0 / fs;
    const auto stepOnce = [&](const std::vector<double>& sources) -> const std::vector<double>& {
        return kTolOverride ? e.step(sources, 60, kBenchTol) : e.step(sources);
    };

    for (int k = 0; k < 400; ++k) {
        if (inIdx >= 0)
            sv[static_cast<size_t>(inIdx)] = 0.0;
        stepOnce(sv);
    }

    std::printf("\nsoak: %.1f s drive + %.1f s silence @ %.0f Hz, %.3f V 440 Hz\n",
                driveSeconds, silenceSeconds, fs, driveVolts);
    const long long driveN = static_cast<long long>(driveSeconds * fs);
    const long long total = static_cast<long long>((driveSeconds + silenceSeconds) * fs);
    const long long interval = static_cast<long long>(fs * 0.5);
    long long iters = 0, samples = 0;
    const long long fb0 = e.blockFallbackCount();
    for (long long n = 0; n < total; ++n) {
        if (inIdx >= 0) {
            if (n < driveN)
                sv[static_cast<size_t>(inIdx)] = driveVolts * std::sin(w * static_cast<double>(n));
            else
                sv[static_cast<size_t>(inIdx)] = 0.0;
        }
        const auto& y = stepOnce(sv);
        iters += static_cast<long long>(e.newtonIterations());
        ++samples;
        if ((n + 1) % interval == 0) {
            std::printf("  t=%6.2f", static_cast<double>(n + 1) / fs);
            if (outIdx >= 0)
                std::printf(" out=% .6f", y[static_cast<size_t>(outIdx)]);
            for (const auto& [name, idx] : watch)
                std::printf(" %s=% .6f", name.c_str(), y[static_cast<size_t>(idx)]);
            std::printf("  it=%.2f fb=%lld\n", static_cast<double>(iters) /
                                                   static_cast<double>(samples),
                        e.blockFallbackCount() - fb0);
            iters = 0;
            samples = 0;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool extra = false;
    bool forceJoint = false;
    for (int a = 1; a < argc; ++a) {
        if (std::string(argv[a]) == "--extra")
            extra = true;
        if (std::string(argv[a]) == "--joint")
            forceJoint = true;
    }
    if (argc >= 2 && std::string(argv[1]) == "--file") {
        if (argc < 3) {
            std::printf("usage: livewdf_bench_circuits --file <circuit.livewdf> [--profile] [--joint] [--fine] [--tol <value>] [--drive <volts>] [--grid-current on|off] [--burst] [--pentode <koren|reefman-d|reefman-e|reefman-se|compare>]\n");
            return 2;
        }
        bool profile = false;
        bool joint = false;
        bool burst = false;
        bool soak = false;
        double driveVolts = 0.25;
        for (int a = 3; a < argc; ++a) {
            if (std::string(argv[a]) == "--profile")
                profile = true;
            if (std::string(argv[a]) == "--joint")
                joint = true;
            if (std::string(argv[a]) == "--fine")
                kFineBlocks = true;
            if (std::string(argv[a]) == "--blocks")
                kPrintBlocks = true;
            if (std::string(argv[a]) == "--burst")
                burst = true;
            if (std::string(argv[a]) == "--soak")
                soak = true;
            if (std::string(argv[a]) == "--drive" && a + 1 < argc) {
                driveVolts = std::stod(argv[++a]);
                kDriveVolts = driveVolts;
            }
            if (std::string(argv[a]) == "--grid-current" && a + 1 < argc) {
                const std::string value = argv[++a];
                if (value == "on")
                    kGridCurrent = true;
                else if (value == "off")
                    kGridCurrent = false;
                else {
                    std::printf("invalid --grid-current value: %s\n", value.c_str());
                    return 2;
                }
            }
            if (std::string(argv[a]) == "--tol" && a + 1 < argc) {
                kBenchTol = std::stod(argv[++a]);
                kTolOverride = true;
            }
            if (std::string(argv[a]) == "--pot" && a + 2 < argc) {
                kPotId = argv[++a];
                kPotPos = std::stod(argv[++a]);
            }
            if (std::string(argv[a]) == "--pot-at" && a + 3 < argc) {
                kPotId = argv[++a];
                kPotAtSample = std::stoll(argv[++a]);
                kPotAtPos = std::stod(argv[++a]);
            }
            if (std::string(argv[a]) == "--switch-at" && a + 3 < argc) {
                const std::string ids = argv[++a];
                kSwitchAtSample = std::stoll(argv[++a]);
                kSwitchPeriod = std::stoll(argv[++a]);
                size_t start = 0;
                while (start <= ids.size()) {
                    const size_t comma = ids.find(',', start);
                    kSwitchIds.push_back(ids.substr(start, comma == std::string::npos
                                                               ? std::string::npos
                                                               : comma - start));
                    if (comma == std::string::npos)
                        break;
                    start = comma + 1;
                }
            }
            if (std::string(argv[a]) == "--switch-count" && a + 1 < argc)
                kSwitchCount = std::stoll(argv[++a]);
            if (std::string(argv[a]) == "--pentode" && a + 1 < argc)
                kPentodeModel = argv[++a];
        }

        wdf::LiveWDFDocument doc;
        std::string error;
        if (!wdf::LiveWDFDocument::loadFromFile(argv[2], doc, error)) {
            std::printf("could not load '%s': %s\n", argv[2], error.c_str());
            return 3;
        }

        // **What the load had to say about the drawing.** Not a refusal -- the
        // sheet means what it draws -- but not silence either: these are the
        // facts (a part this build lacks, an audio entry with no drawn part)
        // that decide whether the circuit that runs is the one the user sees.
        for (const auto& warning : doc.loadWarnings)
            std::printf("  warning: %s\n", warning.c_str());

        if (burst) {
            for (int hostBlock : {64, 128, 256})
                runBurst(doc, 48000.0, hostBlock, 5.0);
            return 0;
        }

        if (soak) {
            runSoak(doc, 48000.0, 1.0, 20.0, driveVolts);
            return 0;
        }

        if (kPentodeModel == "compare") {
            const double hostBlockUs = 1024.0 / 48000.0 * 1e6;
            std::printf("LiveWDF Pentode Model Comparison\n");
            std::printf("  file: %s\n", argv[2]);
            std::printf("  model             rate   channels   form       ns/engine-sample   spread   it/sample   fb   fail  nonfinite   us/1024-host-block   %% of one core   peak\n");
            std::printf("  -------------------------------------------------------------------------------------------------------------------------------------------------------\n");
            const std::vector<std::string> models = {"koren", "reefman-d", "reefman-e", "reefman-se"};
            for (const auto& m : models) {
                kPentodeModel = m;
                for (double fs : {48000.0, 96000.0}) {
                    for (int channels : {1, 2}) {
                        const Result r = runDocument(doc, fs, channels, true, profile);
                        if (r.nsPerSample <= 0.0) continue;
                        const int engineBlock = static_cast<int>(1024.0 * fs / 48000.0);
                        const double usPerBlock = r.nsPerSample * engineBlock / 1000.0;
                        std::printf("  %-15s   %5.0f   %-8s   %-8s   %15.1f   %5.1f%%   %8.2f   %4lld   %4d  %9d   %18.1f   %13.2f%%   %10.6g\n",
                                    m.c_str(), fs, channels == 1 ? "mono" : "stereo",
                                    r.explicitActive ? "closed" : "newton", r.nsPerSample,
                                    spreadPercent(r), r.itersPerSample, r.jfetFallbacks,
                                    static_cast<int>(fs) - r.converged, r.nonFinite, usPerBlock,
                                    100.0 * usPerBlock / hostBlockUs, r.peak);
                    }
                }
            }
            std::printf("\n(us/1024-host-block uses 1024 input samples; 2x uses 2048 engine samples. %% of one core = that time / 21.33 ms.)\n");
            return 0;
        }

        const auto circuit = doc.toCircuit();
        const std::string inputId = doc.inputId();
        std::printf("LiveWDF engine-only file benchmark\n");
        if (kTolOverride)
            std::printf("  solve tol override: %.3e (engine default 1.000e-09; block path floors at 1e-9)\n", kBenchTol);
        if (kFineBlocks)
            std::printf("  fine block solver: on\n");
        if (!kPentodeModel.empty() && kPentodeModel != "koren")
            std::printf("  pentode model: %s\n", kPentodeModel.c_str());
        std::printf("  file: %s\n", argv[2]);
        std::printf("  circuit sources=%zu pots=%zu varres=%zu input='%s' output='%s'\n",
                    circuit.src.size(), circuit.pots.size(), circuit.varRes.size(),
                    inputId.c_str(), doc.outputNet().c_str());
        std::printf("  block   rate   channels   form       ns/engine-sample   spread   it/sample   fb   fail  nonfinite   us/1024-host-block   %% of one core   peak\n");
        std::printf("  -------------------------------------------------------------------------------------------------------------------------------------------------------\n");

        const double hostBlockUs = 1024.0 / 48000.0 * 1e6;
        for (double fs : {48000.0, 96000.0, 192000.0}) {
            for (int channels : {1, 2}) {
                const Result r = runDocument(doc, fs, channels, true, profile);
                if (r.nsPerSample <= 0.0)
                    continue;
                const int engineBlock = static_cast<int>(1024.0 * fs / 48000.0);
                const double usPerBlock = r.nsPerSample * engineBlock / 1000.0;
                std::printf("  %5d   %5.0f   %-8s   %-8s   %15.1f   %5.1f%%   %8.2f   %4lld   %4d  %9d   %18.1f   %13.2f%%   %10.6g\n",
                            1024, fs, channels == 1 ? "mono" : "stereo",
                            r.explicitActive ? "closed" : "newton", r.nsPerSample,
                            spreadPercent(r), r.itersPerSample, r.jfetFallbacks,
                            static_cast<int>(fs) - r.converged, r.nonFinite, usPerBlock,
                            100.0 * usPerBlock / hostBlockUs, r.peak);
                if (r.stateRejects > 0)
                    std::printf("           state rejects: %lld\n", r.stateRejects);
                if (kPrintBlocks) {
                    std::printf("  block fallbacks: %lld, last residual %.3e%s\n",
                                r.blockFallbacks, r.lastBlockResidual,
                                r.lastBlockFallback ? " (last sample fell back)" : "");
                }
                if (profile && fs == 48000.0 && channels == 1) {
                    std::printf("  paths: runtime-block %lld/%d, block-structured %lld/%d, state-rejects %lld, grid-limits %lld, line-trials %.2f, dev-evals %.2f\n",
                                r.runtimeBlockSamples, static_cast<int>(fs), r.blockStructureSamples,
                                static_cast<int>(fs), r.stateRejectSamples, r.gridLimitHitsTotal,
                                r.lineTrialsPerSample, r.deviceEvalsPerSample);
                    std::printf("  root ports %d, static blocks %d [", r.roots, r.blocks);
                    for (int s = 0; s < r.blocks && s < 16; ++s)
                        std::printf("%s%d", s == 0 ? "" : " ", r.blockPorts[s]);
                    std::printf("] ports\n");
                    printPhaseShares("48 kHz mono", r.roots, r.prof);
                }
                if (joint && fs == 48000.0 && channels == 1) {
                    const Result jr = runDocument(doc, fs, channels, false, false);
                    std::printf("  joint reference: %.1f ns/sample, it/sample %.2f, nonfinite %d, "
                                "max|block-joint| %.3e\n",
                                jr.nsPerSample, jr.itersPerSample, jr.nonFinite,
                                maxDiff(r.out, jr.out));
                }
            }
        }

        std::printf("\n(us/1024-host-block uses 1024 input samples; 2x uses 2048 engine samples. %% of one core = that time / 21.33 ms.)\n");
        return 0;
    }

    std::printf("LiveWDF multi-circuit instrument\n");
    std::printf("=====================================================================\n");
    std::printf("  circuit                     N    P   K  form      ns/sample   worst  spread  nt-ns   it/smp  conv      fb fail\n");
    std::printf("  ---------------------------------------------------------------------------------------------\n");

    std::vector<Result> fast;   // one row per fixture: the chosen formulation
    const auto& cases = extra ? lwdf_test::extendedCircuitCases() : lwdf_test::circuitCases();
    if (extra)
        std::printf("Extended permanent portfolio\n");

    if (forceJoint)
        std::printf("Forced joint Newton (reference path)\n");
    for (const auto& tc : cases) {
        const Result a = run(tc, !forceJoint, false);  // explicit root if eligible
        const Result b = run(tc, forceJoint ? true : false, false);  // opposite path
        const Result p = run(tc, true, true);    // profiled
        const double diff = maxDiff(a.out, b.out);

        // The forced-Newton cost is only news when it is a *different*
        // formulation -- otherwise the column would be the same number twice.
        char ntBuf[16];
        if (b.explicitActive != a.explicitActive)
            std::snprintf(ntBuf, sizeof ntBuf, "%8.1f", b.nsPerSample);
        else
            std::snprintf(ntBuf, sizeof ntBuf, "%8s", "-");

        std::printf("  %-26s %4d %4d %4d  %-8s %8.1f %8.1f %5.1f%% %s  %5.2f  %d/%d  %4lld %4d  |ex-nt| %.2e\n",
                    tc.name, a.nodes, a.elements, a.roots,
                    a.explicitActive ? "closed" : "newton", a.nsPerSample, a.nsMax, spreadPercent(a),
                    ntBuf, a.itersPerSample, a.converged, kSamples, a.jfetFallbacks,
                    kSamples - a.converged, diff);

        std::printf("    %-26s paths: runtime-block %lld/%d, block-structured %lld/%d, state-rejects %lld, grid-limits %lld\n",
                    tc.name, a.runtimeBlockSamples, kSamples, a.blockStructureSamples, kSamples,
                    a.stateRejectSamples, a.gridLimitHitsTotal);

        printPhaseShares(tc.name, p.roots, p.prof);

        fast.push_back(a);
    }

    std::printf("---------------------------------------------------------------------\n");
    std::printf("  timing: median of %d measured runs after %d untimed warm-up samples;\n",
                kRepeats, kWarmupSamples);
    std::printf("  spread = (max-min)/median; fb = JFET independent-fallback count; fail = non-converged samples.\n");
    std::printf("---------------------------------------------------------------------\n");
    std::printf("  which of Xe*w is provably zero (structure only)\n");
    for (size_t i = 0; i < fast.size(); ++i) {
        const Result& r = fast[i];
        std::printf("    %-26s %2d of %2d columns dead  (%.0f%%)\n",
                    cases[i].name, r.deadColumns, r.elements,
                    r.elements > 0 ? 100.0 * r.deadColumns / r.elements : 0.0);
    }
    std::printf("---------------------------------------------------------------------\n");
    std::printf("  which root-port groups are independent (structure only)\n");
    if (!extra) {  // rootGroupCoupling's display assumes the original 23-fixture device mix
        for (const auto& tc : cases)
            printCoupling(tc, lwdf_test::rootGroupCoupling((*tc.build)(), tc, kFs));
    }
    std::printf("---------------------------------------------------------------------\n");
    std::printf("  the engine only is timed here; `bench_stage.cpp` times its `std::sin`\n");
    std::printf("  input too, so its ns/sample sits about %.1f ns higher on the same stage.\n",
                sinBaselineNs());
    std::printf("  (real-time budget at %.0f Hz: %.0f ns of one core per sample)\n", kFs, 1e9 / kFs);
    return 0;
}
