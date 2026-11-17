// Scaling probe: how per-sample cost grows with coupled-block size. This is
// the measurement that decides whether a decomposition or a table-driven
// solve has any headroom to buy -- the gate the roadmap demands before any
// blockwise backend is even sketched.
//
// NOT registered with CTest; an instrument (see tests/CMakeLists.txt):
//
//     build/tests/Release/livewdf_bench_scaling.exe
//
// Three ladders, K = 1..8:
//   * cascade           K quadric triodes chained through interstage caps.
//     Each block is one device, one live port: the closed-form path.
//     Expected: linear in K, near-constant per stage.
//   * parallel stages   K independent triode stages on one ideal source: K
//     scalar blocks versus the one K-row joint Newton it is compared against.
//     Expected: the joint path superlinear -- folds and matrices both grow.
//   * cathode bundle    K triodes *sharing one cathode node*: one synthetic
//     multi-device block. No real amp shares K cathodes; this is an honest
//     probe of the one thing the coupled path scales on: joint-Newton
//     matrix size. If this row grows like K^3, the coupled path is exactly
//     where a decomposition would pay off.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "wdf/Engine.h"

namespace {

constexpr double kFs = 48000.0;
constexpr int kSamples = 24000;  // half a second per repeat
constexpr int kRepeats = 5;

double sineAt(int n) {
    return 3.0 * std::sin(2.0 * 3.14159265358979323846 * 220.0 * n / kFs);
}

// One quadric stage at nodes (g,p,k): external grid g wired off the interstage
// cap node `pin`, plate fed from `pin`, cathode at `k`, plate output dropped
// onto `pout`. Mirrors tests/engine/test_root_quadric.cpp's singleTriodeStage.
void appendStage(wdf::Circuit& c, const std::string& s) {
    const std::string g = "g" + s, p = "p" + s, k = "k" + s;
    c.addRes("pin" + s, p, 100e3);
    c.addCap(p, "pout" + s, 10e-9);
    c.addRes("pout" + s, "0", 1e6);
    c.addRes("pin" + s, g, 20e3);
    c.addRes(k, "0", 1e3);
    c.addCap(k, "0", 10e-6);
    wdf::TriodeModel model = wdf::k12ax7Quadric();
    c.addTri(g, p, k, g, model);
}

// Cascade of K stages: plate of stage i feeds the interstage pin of stage i+1.
wdf::Circuit cascade(int K) {
    wdf::Circuit c;
    c.addSrc("pin", "0", 0.0);
    for (int i = 0; i < K; ++i) {
        const std::string s = std::to_string(i);
        appendStage(c, s);
        if (i + 1 < K) {
            const std::string nxt = std::to_string(i + 1);
            c.addRes("p" + s + "out", "pin" + nxt, 1e3);
        }
    }
    return c;
}

// K triodes all sharing the cathode node "k". Synthetic scaling probe.
wdf::Circuit cathodeBundle(int K) {
    wdf::Circuit c;
    c.addSrc("pin0", "0", 0.0);
    for (int i = 0; i < K; ++i) {
        const std::string s = std::to_string(i);
        const std::string g = "g" + s, p = "p" + s;
        c.addRes("pin0", p, 100e3);
        c.addCap(p, "pout" + s, 10e-9);
        c.addRes("pout" + s, "0", 1e6);
        c.addRes("pin0", g, 20e3);
        wdf::TriodeModel model = wdf::k12ax7Quadric();
        // Shared cathode node "k" is the coupling under test.
        c.addTri(g, p, "k", g, model);
    }
    c.addRes("k", "0", 1e3);
    c.addCap("k", "0", 10e-6);
    return c;
}

// K independent triode stages driven from one ideal source. The roots are
// independent (the shared source and supply are ideal, so they add no
// impedance coupling), which is the shape that prices a block solve against a
// joint Newton whose matrix grows with every stage: K one-row blocks versus
// one K-row system.
wdf::Circuit parallelStages(int K) {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addSrc("e", "0", 250.0);
    for (int i = 0; i < K; ++i) {
        const std::string s = std::to_string(i);
        const std::string g = "g" + s, p = "p" + s, k = "k" + s;
        c.addRes("in", g, 20e3);
        c.addRes(g, "0", 1e6);
        c.addRes("e", p, 100e3);
        c.addRes(k, "0", 1e3);
        c.addCap(k, "0", 10e-6);
        wdf::TriodeModel model = wdf::k12ax7Quadric();
        c.addTri(g, p, k, g, model);
    }
    return c;
}

struct Row {
    double nsMin = 0.0;
    double nsMedian = 0.0;
    double nsMax = 0.0;
    double iters = 0.0;
    bool closedForm = false;
    bool blockPath = false;
    long long fallbacks = 0;
    int roots = 0;
    int blocks = 0;
    double setupUs = 0.0;
};

Row measure(wdf::Circuit c, bool forceJoint = false) {
    const auto setup0 = std::chrono::steady_clock::now();
    wdf::Engine e(c, kFs);
    const auto setup1 = std::chrono::steady_clock::now();
    if (forceJoint)
        e.setExplicitRoots(false);
    Row r;
    r.setupUs = std::chrono::duration<double, std::micro>(setup1 - setup0).count();
    r.roots = e.rootCount();
    r.closedForm = e.explicitRoots();
    r.blockPath = e.blockRoots();
    r.blocks = e.rootBlockCount();
    std::vector<std::vector<double>> runs;
    runs.reserve(kRepeats);
    for (int rep = 0; rep < kRepeats; ++rep) {
        e.reset();
        std::vector<double> sv(c.src.size(), 0.0);
        for (size_t s = 0; s < c.src.size(); ++s)
            sv[s] = c.src[s].dc;
        double iters = 0.0;
        // warm up untimed
        for (int n = 0; n < 2048; ++n) {
            sv[0] = sineAt(n);
            e.step(sv);
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (int n = 0; n < kSamples; ++n) {
            sv[0] = sineAt(n + 2048);
            const auto& y = e.step(sv, 60, 1e-9);
            (void)y;
            iters += e.newtonIterations();
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kSamples;
        runs.push_back({ns, iters / kSamples});
    }
    r.fallbacks = e.blockFallbackCount();
    std::sort(runs.begin(), runs.end(),
              [](const std::vector<double>& a, const std::vector<double>& b) { return a[0] < b[0]; });
    r.nsMedian = runs[kRepeats / 2][0];
    r.nsMin = runs.front()[0];
    r.nsMax = runs.back()[0];
    r.iters = runs[kRepeats / 2][1];
    return r;
}

void ladder(const char* label, wdf::Circuit (*build)(int)) {
    std::printf("%-18s K  roots  blocks  path        it/sample  fallbacks  ns/sample  setup us\n",
                label);
    for (int K = 1; K <= 8; ++K) {
        const Row r = measure(build(K));
        const char* path = r.closedForm ? "closed" : (r.blockPath ? "block" : "joint");
        std::printf("%-18s %d  %5d  %6d  %-10s %9.2f %10lld %10.1f %9.1f\n", label, K, r.roots,
                    r.blocks, path, r.iters, r.fallbacks, r.nsMedian, r.setupUs);
    }
}

void compareLadder(const char* label, wdf::Circuit (*build)(int)) {
    std::printf("%-18s K  roots  blocks  default ns min/med/max   joint ns min/med/max   speedup  def it  joint it  fallbacks  setup us\n",
                label);
    for (int K = 1; K <= 8; ++K) {
        const Row b = measure(build(K), false);
        const Row j = measure(build(K), true);
        std::printf("%-18s %d  %5d  %6d  %7.1f/%7.1f/%7.1f  %7.1f/%7.1f/%7.1f  %7.2fx  %6.2f  %8.2f  %9lld %9.1f\n",
                    label, K, b.roots, b.blocks, b.nsMin, b.nsMedian, b.nsMax,
                    j.nsMin, j.nsMedian, j.nsMax, j.nsMedian / b.nsMedian,
                    b.iters, j.iters, b.fallbacks, b.setupUs);
    }
}

}  // namespace

int main() {
    ladder("cascade", [](int K) { return cascade(K); });
    compareLadder("parallel stages", parallelStages);
    std::printf("\n");
    ladder("cathode bundle", cathodeBundle);
    return 0;
}
