// Manual micro-benchmark: the generic engine vs a hand-built closed-form stage.
//
// NOT a correctness gate and NOT registered with CTest -- it is an instrument.
// Build target `livewdf_bench`; run it by hand:
//
//     build\tests\Release\livewdf_bench.exe
//
// Why this exists (the question it answers): Cathodyne runs a common-cathode
// stage as a *fixed* WDF tree with a *closed-form* triode root -- no Newton, no
// dynamics, one sqrt per sample. LiveWDF's generic `Engine` compiles the same
// circuit to an R-type network and drives it with a multiport Newton. This
// program builds all three from the same topology and the same quadric device law:
//
//   closed  : hand-built single-stage WDF, closed-form triode root
//   generic : the Engine, multiport Newton            (`setExplicitRoots(false)`)
//   explicit: the Engine, exact closed-form root      (`setExplicitRoots(true)`)
//
// The gap between `closed` and `explicit` is what genericity costs once the root
// is closed-form; the gap between `explicit` and `generic` is what avoiding Newton
// buys. All three must agree to round-off.
//
// The circuit (paper Sec. 8, Table 4):
//
//     in --Ci-- i ; i --Ri-- gnd ; i --Rg-- g(tube plate=p, cathode=k) ; k --Rk-- gnd
//     k --Ck-- gnd ; p --Rp-- e(E=250) ; p --Co-- o ; o --Ro-- gnd

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "wdf/Engine.h"
#include "support/QuadricStage.h"
#include "devices/Triode.h"

namespace {

constexpr double kFs = 48000.0;
constexpr int kSamples = 48000 * 2;  // 2 s of signal
constexpr double kPi = 3.14159265358979323846;

double inputAt(int n) {
    return 0.2 * std::sin(2.0 * kPi * 220.0 * static_cast<double>(n) / kFs);
}

struct Run {
    double nsPerSample = 0.0;
    double seconds = 0.0;
    double checksum = 0.0;
    double peak = 0.0;
    std::vector<double> out;
    long long iters = 0;
    int converged = 0;
    double dcPlate = 0.0;
    bool explicitActive = false;
    /// Phase breakdown of `step()`. Zero unless the run asked for profiling;
    /// a profiled run pays a clock read per phase, so its `nsPerSample` is not
    /// comparable with an unprofiled one -- only its phase *shares* are.
    wdf::Engine::PhaseProfile prof;
};

wdf::Circuit stageCircuit() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);     // driven input source (src[0])
    c.addSrc("e", "0", 250.0);    // supply E (src[1])
    c.addCap("in", "i", 100e-9);  // Ci
    c.addRes("i", "0", 1e6);      // Ri
    c.addRes("i", "g", 20e3);     // Rg
    c.addRes("k", "0", 1e3);      // Rk
    c.addCap("k", "0", 10e-6);    // Ck
    c.addRes("e", "p", 100e3);    // Rp
    c.addCap("p", "o", 10e-9);    // Co
    c.addRes("o", "0", 1e6);      // Ro
    wdf::TriodeModel model = wdf::k12ax7Quadric();
    // The paper's model has no rgi/vct, so the internal grid *is* the external
    // grid here (addTube would insert the Tubes.lib grid network).
    c.addTri("g", "p", "k", "g", model);
    return c;
}

Run runGeneric(bool explicitRoots, bool profile = false) {
    Run r;
    wdf::Engine e(stageCircuit(), kFs);
    if (!explicitRoots)
        e.setExplicitRoots(false);
    r.explicitActive = e.explicitRoots();
    if (profile)
        e.setProfile(true);

    std::vector<double> node, rootV;
    if (!e.solveDc(node, rootV)) {
        std::printf("engine solveDc did not converge\n");
        return r;
    }
    const int pIdx = e.nodeIndex("p");
    r.dcPlate = pIdx >= 0 ? node[static_cast<size_t>(pIdx)] : 0.0;
    e.reset();
    e.resetPhaseProfile();

    r.out.assign(kSamples, 0.0);
    const int oIdx = e.nodeIndex("o");
    std::vector<double> sv(2, 0.0);
    sv[1] = 250.0;
    const auto t0 = std::chrono::steady_clock::now();
    double acc = 0.0;
    for (int n = 0; n < kSamples; ++n) {
        sv[0] = inputAt(n);
        const std::vector<double>& y = e.step(sv);
        const double v = y[static_cast<size_t>(oIdx)];
        r.out[static_cast<size_t>(n)] = v;
        acc += v;
        r.peak = std::max(r.peak, std::abs(v));
        r.iters += e.newtonIterations();
        if (e.lastConverged())
            ++r.converged;
    }
    const auto t1 = std::chrono::steady_clock::now();
    r.seconds = std::chrono::duration<double>(t1 - t0).count();
    r.nsPerSample = r.seconds / kSamples * 1e9;
    r.checksum = acc;
    r.prof = e.phaseProfile();
    return r;
}

double maxDiff(const std::vector<double>& a, const std::vector<double>& b) {
    double m = 0.0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        m = std::max(m, std::abs(a[i] - b[i]));
    return m;
}

/// Size of the reduced Newton system. `EngineSolve::reducedAssemble` drops the
/// internal-grid port from each `(plate, external grid, internal grid)` triple,
/// so the step factors `2*nTri`, never `3*nTri` -- and this bench runs one
/// triode, so it is 2x2. (The labels here used to read "12x12", which is the
/// six-triode case; they were stale and made the profile look like a bigger
//  solve than it is.)
int reducedSystemSize() {
    wdf::Engine e(stageCircuit(), kFs);
    return 2 * static_cast<int>(e.compiled().tri.size());
}

/// One config's `step()` breakdown, in ns/sample and as a share of the phases
/// that were actually timed. `measuredNs` is the *unprofiled* ns/sample for the
/// same config: the difference between it and `accounted` is what the clock
/// reads inside `step()` cost, which is why the shares -- not the absolutes --
/// are the thing to read here.
void printPhases(const char* label, const wdf::Engine::PhaseProfile& p, double measuredNs, int nN) {
    if (p.samples <= 0)
        return;
    const double inv = 1e9 / static_cast<double>(p.samples);
    const double total = p.total();

    char jacName[64];
    char luName[64];
    std::snprintf(jacName, sizeof jacName, "jacobian   %dx%d assemble", nN, nN);
    std::snprintf(luName, sizeof luName, "lu         %dx%d solve + expand", nN, nN);

    struct Row {
        const char* name;
        double sec;
    };
    const Row rows[] = {
        {"offsets    eoff/roff/b        ", p.offsets},
        {"network    u=Xe*(b-eoff), vOc ", p.network},
        {"predict    warm start         ", p.predict},
        {"device     currents + residual", p.device},
        {jacName, p.jacobian},
        {luName, p.lu},
        {"linesearch halving trials      ", p.linesearch},
        {"other      solver work         ", p.otherSolveTime()},
        {"post       nodes + cap states  ", p.post},
    };

    std::printf("  %s\n", label);
    for (const auto& row : rows) {
        const double ns = row.sec * inv;
        if (ns < 0.05)  // a phase that never fires is noise in a table
            continue;
        std::printf("    %s : %6.2f ns  (%4.1f%%)\n", row.name, ns,
                    total > 0.0 ? 100.0 * row.sec / total : 0.0);
    }
    std::printf("    accounted %6.2f ns  vs measured %6.2f ns unprofiled\n", total * inv,
                measuredNs);
}

/// Which columns of `u = Xe*w` are *provably* zero, without running anything.
///
/// `w_j = b_j - eoff_j`, and both terms are fixed by structure, not by value:
/// `b_j` is the element's reactive state (nonzero only for a cap or inductor)
/// and `eoff_j` is the affine source map (nonzero only for an element tied to an
/// ideal source). So an element that is neither carries `w_j == 0` on every
/// sample forever, and `Xe*w` is spending a multiply on an exact zero.
///
/// This is the measurement the sparse-column skip has to justify itself against
/// -- if most columns survive here, the skip is not worth building.
void printColumns() {
    wdf::Engine e(stageCircuit(), kFs);
    const std::vector<int>& capOf = e.capOf();
    const std::vector<int>& indOf = e.indOf();
    const std::vector<wdf::AffineOffset>& off = e.elementOffsets();
    const int P = e.elementCount();

    // `buildNetwork` lays elements out as every resistor, then every capacitor,
    // then every inductor -- this table only reads for the stage as written.
    static const char* kNames[] = {"Ri", "Rg", "Rk", "Rp", "Ro", "Ci", "Ck", "Co"};
    static const int kNamesN = static_cast<int>(sizeof(kNames) / sizeof(kNames[0]));

    int dead = 0;
    std::printf("  columns of Xe*w, by whether w_j can ever be nonzero:\n");
    for (int j = 0; j < P; ++j) {
        const bool reactive = capOf[static_cast<size_t>(j)] >= 0 || indOf[static_cast<size_t>(j)] >= 0;
        const bool coupled =
            !off[static_cast<size_t>(j)].coef.empty() || off[static_cast<size_t>(j)].c0 != 0.0;
        const bool alive = reactive || coupled;
        if (!alive)
            ++dead;
        const char* nm = j < kNamesN ? kNames[j] : "?";
        std::printf("    j=%d  %-3s  %-9s %s\n", j, nm,
                    reactive ? (capOf[static_cast<size_t>(j)] >= 0 ? "cap" : "ind") : "resistor",
                    alive ? "state/source -> kept" : "b=0 and eoff=0 -> ALWAYS 0");
    }
    std::printf("    -> %d of %d columns are exactly zero on every sample\n", dead, P);
}

}  // namespace

int main() {
    // (1) Hand-built closed-form stage (the Cathodyne-style path).
    Run closed;
    {
        wdf::QuadricCommonCathode stage;
        stage.params = wdf::kQuadric12AX7;
        if (!stage.prepare(kFs)) {
            std::printf("quadric stage prepare() failed\n");
            return 1;
        }
        closed.out.assign(kSamples, 0.0);
        const auto t0 = std::chrono::steady_clock::now();
        double acc = 0.0;
        for (int n = 0; n < kSamples; ++n) {
            const double v = stage.process(inputAt(n));
            closed.out[static_cast<size_t>(n)] = v;
            acc += v;
            closed.peak = std::max(closed.peak, std::abs(v));
        }
        const auto t1 = std::chrono::steady_clock::now();
        closed.seconds = std::chrono::duration<double>(t1 - t0).count();
        closed.nsPerSample = closed.seconds / kSamples * 1e9;
        closed.checksum = acc;
    }

    const Run generic = runGeneric(false);   // multiport Newton
    const Run explicit_ = runGeneric(true);  // exact closed-form root

    // Two more passes with `setProfile(true)` -- one clock read per phase, so
    // these answer "where does the time go" and nothing else. Kept separate so
    // the headline ns/sample figures above stay free of instrumentation.
    const Run genericProf = runGeneric(false, true);
    const Run explicitProf = runGeneric(true, true);

    std::printf("\nLiveWDF stage benchmark  fs=%.0f Hz  %d samples (%.1f s)\n", kFs, kSamples,
                kSamples / kFs);
    std::printf("---------------------------------------------------------------------\n");
    std::printf("  hand-built closed form    : %8.1f ns/sample   (checksum %+.4f)\n",
                closed.nsPerSample, closed.checksum);
    std::printf("  Engine, Newton            : %8.1f ns/sample   (checksum %+.4f, %.2f it/sample)\n",
                generic.nsPerSample, generic.checksum, static_cast<double>(generic.iters) / kSamples);
    std::printf("  Engine, closed-form root  : %8.1f ns/sample   (checksum %+.4f, %.2f it/sample)\n",
                explicit_.nsPerSample, explicit_.checksum,
                static_cast<double>(explicit_.iters) / kSamples);
    std::printf("---------------------------------------------------------------------\n");
    std::printf("  speedup (Newton / closed-form root) : %6.2fx\n",
                generic.nsPerSample / explicit_.nsPerSample);
    std::printf("  closed-form root active             : %s\n", explicit_.explicitActive ? "yes" : "NO");
    std::printf("  Newton converged                    : %d / %d\n", generic.converged, kSamples);
    std::printf("  closed-form root converged          : %d / %d\n", explicit_.converged, kSamples);
    std::printf("  DC plate voltage                    : Newton %.3f V, closed-form %.3f V\n",
                generic.dcPlate, explicit_.dcPlate);
    std::printf("  max |Engine closed - Engine Newton| : %.3e V\n", maxDiff(explicit_.out, generic.out));
    std::printf("  max |Engine closed - hand-built|    : %.3e V  (peak %.3f V)\n",
                maxDiff(explicit_.out, closed.out), explicit_.peak);

    std::printf("---------------------------------------------------------------------\n");
    std::printf("  where step() spends its time (profiled pass, shares of timed phases)\n");
    const int nN = reducedSystemSize();
    printPhases("Engine, Newton", genericProf.prof, generic.nsPerSample, nN);
    printPhases("Engine, closed-form root", explicitProf.prof, explicit_.nsPerSample, nN);
    std::printf("---------------------------------------------------------------------\n");
    std::printf("  which of that work is avoidable (static, from structure alone)\n");
    printColumns();

    std::printf("\n(real-time budget at %.0f Hz: %.0f ns of one core per sample)\n", kFs, 1e9 / kFs);
    return 0;
}
