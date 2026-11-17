#include <catch2.hpp>

#include <algorithm>
#include <cmath>
#include <string>

#include "wdf/Engine.h"
#include "spice/SpiceNet.h"

#include "oracle/rc_lowpass.h"  // wdf_compiler-generated oracle (see oracle/README.md)

// Phase 1 increment 2: sample-for-sample validation of LiveWDF's parser + Engine
// against wdf_compiler's generated code (prebuilt exe, MIT, Chowdhury-DSP) on the
// same netlist. Both sides start from zero state, same sample rate, same input.
//
// Note: the generated header declares its types in the global namespace, so this
// test lives in its own translation unit.

namespace {

// Byte-identical copy of wdf_compiler/tests/rc_lowpass/rc_lowpass.net (the input
// waveform tokens are ignored by both consumers here; we drive both engines).
constexpr const char* kRcLowpassNet = R"(* RC Lowpass Filter
V1 P001 0 PULSE(0 1 1m 1u 1u 10m 20m 0) ; input root
R1 P002 P001 1k
C1 P002 0 1u ; output:V
.tran 0 50m 0 10u
.backanno
.end
)";

constexpr double kFs = 48000.0;

}  // namespace

TEST_CASE("rc_lowpass: LiveWDF Engine matches wdf_compiler generated code", "[phase1][oracle]") {
    // --- Oracle side (wdf_compiler output, float math) ---
    Impedances imp{};
    calc_impedances(imp, static_cast<float>(kFs));
    State stOr{};

    // --- LiveWDF side (SpiceNet parser + Engine, double math) ---
    wdf::Circuit c = wdf::buildFromString(kRcLowpassNet);
    REQUIRE(c.res.size() == 1);
    REQUIRE(c.cap.size() == 1);
    REQUIRE(c.src.size() == 1);

    wdf::Engine e(c, kFs);  // fresh engine: capacitor states are zero, matching State{}
    const int outIdx = e.nodeIndex("P002");
    REQUIRE(outIdx >= 0);

    // --- Drive both with identical input, compare every sample ---
    const int nSamples = static_cast<int>(kFs * 0.5);  // 500 ms
    double maxErr = 0.0;
    bool allConverged = true;
    for (int n = 0; n < nSamples; ++n) {
        const double t = static_cast<double>(n) / kFs;
        const auto vin = static_cast<float>(std::sin(2.0 * M_PI * 100.0 * t));

        const float vOracle = process(stOr, imp, vin);
        const std::vector<double> vEng = e.step({static_cast<double>(vin)});
        const double vLive = vEng[static_cast<size_t>(outIdx)];

        REQUIRE(std::isfinite(vLive));
        allConverged = allConverged && e.lastConverged();
        // POLARITY FINDING (verified 2026-10-18): wdf_compiler's generated tree for
        // this netlist has no PolarityInverter, so its `V:C1` output equals **minus**
        // the SPICE node voltage. Verified empirically: wdf_compiler's own driver
        // (tests/rc_lowpass/rc_lowpass.cpp) against its chowdsp_wdf reference passes
        // at 1.8e-7 while both converge to -1.0 for input +1.0 (read from their
        // data.bin), whereas SPICE's v(P002) for `V1 P001 0 DC 1` is +1.0 (Engine
        // matches SPICE). Compare with sign flipped; see docs/research/03-wdf_compiler.md.
        maxErr = std::max(maxErr, std::abs(static_cast<double>(-vOracle) - vLive));
    }

    REQUIRE(allConverged);
    // float-vs-double, different operation order: sub-millivolt agreement expected
    // on a 1 V-scale signal; tolerance kept loose enough for 24k-sample accumulation.
    REQUIRE(maxErr < 1e-3);
}
