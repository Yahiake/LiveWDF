// Manual micro-benchmark: Dense R-type Engine vs Tree-based WDF Executor
//
// NOT a correctness gate and NOT registered with CTest -- it is an instrument.
// Build target `livewdf_bench_tree`; run it by hand:
//
//     build\tests\Release\livewdf_bench_tree.exe

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "circuit/tree/TreeExecutor.h"
#include "spice/SpiceNet.h"
#include "support/CircuitCases.h"
#include "wdf/Engine.h"

namespace {

constexpr double kFs = 48000.0;
constexpr int kSamples = 48000 * 4;  // 4 s of audio
constexpr int kPasses = 9;           // median of 9 passes
constexpr double kPi = 3.14159265358979323846;

double inputAt(int n) {
    return 0.5 * std::sin(2.0 * kPi * 440.0 * static_cast<double>(n) / kFs);
}

struct Stat {
    double med = 0.0;
    double lo = 0.0;
    double hi = 0.0;
};

Stat medianOf(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return {v[v.size() / 2], v.front(), v.back()};
}

constexpr const char* kRcLowpassNet = R"(* RC Lowpass Filter
V1 P001 0 PULSE(0 1 1m 1u 1u 10m 20m 0) ; input root
R1 P002 P001 1k
C1 P002 0 1u ; output:V
.tran 0 50m 0 10u
.backanno
.end
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

// 4-stage RC ladder: 4 poles, 8 elements
wdf::Circuit makeRcLadder4() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    c.addRes("in", "n1", 1000.0, "R1");
    c.addCap("n1", "0", 100e-9, "C1");
    c.addRes("n1", "n2", 1000.0, "R2");
    c.addCap("n2", "0", 100e-9, "C2");
    c.addRes("n2", "n3", 1000.0, "R3");
    c.addCap("n3", "0", 100e-9, "C3");
    c.addRes("n3", "n4", 1000.0, "R4");
    c.addCap("n4", "0", 100e-9, "C4");
    return c;
}

wdf::Circuit makeGuitarPickup() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    c.addRes("in", "n1", 6000.0, "R_coil");
    c.addInd("n1", "jack", 3.0, "L_coil");
    c.addCap("jack", "0", 600e-12, "C_cable");
    c.addRes("jack", "0", 1.0e6, "R_load");
    return c;
}

wdf::Circuit makeLcCrossover3() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0, 0.0, {}, "Vin");
    c.addInd("in", "mid", 0.003, "L1");
    c.addCap("mid", "0", 10e-6, "C1");
    c.addInd("mid", "spk", 0.001, "L2");
    c.addRes("spk", "0", 8.0, "R_spk");
    return c;
}

void benchCircuit(const std::string& name, const wdf::Circuit& c, const std::string& outElem, const std::string& outNode) {
    wdf::TreeExecutor tree(c, kFs, outElem);
    wdf::Engine dense(c, kFs);
    const int outIdx = dense.nodeIndex(outNode);

    // Warm-up and parity check
    double maxErr = 0.0;
    for (int n = 0; n < 1000; ++n) {
        const double vin = inputAt(n);
        const double vt = tree.step(vin);
        const auto vd = dense.step({vin});
        maxErr = std::max(maxErr, std::abs(vt - vd[static_cast<size_t>(outIdx)]));
    }

    std::vector<double> treeTimes;
    std::vector<double> denseTimes;

    for (int pass = 0; pass < kPasses; ++pass) {
        // Benchmark Tree Executor
        tree.reset();
        auto t0 = std::chrono::steady_clock::now();
        double sumTree = 0.0;
        for (int n = 0; n < kSamples; ++n) {
            sumTree += tree.step(inputAt(n));
        }
        auto t1 = std::chrono::steady_clock::now();
        const double nsTree = std::chrono::duration<double, std::nano>(t1 - t0).count() / kSamples;
        treeTimes.push_back(nsTree);

        // Benchmark Dense Engine
        dense.reset();
        auto t2 = std::chrono::steady_clock::now();
        double sumDense = 0.0;
        for (int n = 0; n < kSamples; ++n) {
            sumDense += dense.step({inputAt(n)})[static_cast<size_t>(outIdx)];
        }
        auto t3 = std::chrono::steady_clock::now();
        const double nsDense = std::chrono::duration<double, std::nano>(t3 - t2).count() / kSamples;
        denseTimes.push_back(nsDense);

        // Volatile barrier to prevent loop optimization
        volatile double barrier = sumTree + sumDense;
        (void)barrier;
    }

    Stat sTree = medianOf(treeTimes);
    Stat sDense = medianOf(denseTimes);
    const double speedup = sDense.med / (sTree.med > 1e-6 ? sTree.med : 1e-6);

    std::printf("%-18s | Tree: %6.2f ns (lo %5.2f, hi %5.2f) | Dense: %6.2f ns (lo %5.2f, hi %5.2f) | %5.2fx speedup | err=%1.1e\n",
                name.c_str(), sTree.med, sTree.lo, sTree.hi, sDense.med, sDense.lo, sDense.hi, speedup, maxErr);
}

}  // namespace

int main() {
    std::printf("==================================================================================================\n");
    std::printf("LiveWDF Phase 1d: Tree-Based WDF Executor vs Dense R-Type Engine Benchmark\n");
    std::printf("Sampling Rate: %.0f Hz, Samples per pass: %d (4.0s), Passes: %d\n", kFs, kSamples, kPasses);
    std::printf("==================================================================================================\n\n");

    benchCircuit("RC Lowpass (1P)", wdf::buildFromString(kRcLowpassNet), "C1", "P002");
    benchCircuit("RC Bandpass (2P)", wdf::buildFromString(kRcBandpassNet), "C2", "3");
    benchCircuit("Pickup RLC (2P)", makeGuitarPickup(), "R_load", "jack");
    benchCircuit("LC Crossover (3P)", makeLcCrossover3(), "R_spk", "spk");
    benchCircuit("RC Ladder (4P)", makeRcLadder4(), "C4", "n4");
    benchCircuit("LC Ladder (3-section)", lwdf_test::lcLadder3(), "C2", "c");

    std::printf("\n==================================================================================================\n");
    return 0;
}
