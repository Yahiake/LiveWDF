// Manual micro-benchmark: how much of `step()` is fixed cost, and what the R-type
// transfer costs on its own -- at the full port width and at the live-column width
// the engine now stores.
//
// NOT a correctness gate and NOT registered with CTest -- it is an instrument.
// Build target `livewdf_bench_floor`; run it by hand:
//
//     build\tests\Release\livewdf_bench_floor.exe
//
// The question this answers. Finding 1 in PROGRESS.md put "~14 ns fixed floor per
// `step()`" on a two-element circuit and called it the largest lever left on the
// closed-form path. The 14 was `rc_lowpass`'s *total* ns/sample, so it carried the
// resistor and the capacitor being simulated along with the engine's own overhead,
// and "largest" was never measured against anything. Both halves needed settling,
// and increment 9b made it urgent in a way nobody forecast: removing the root solve
// from the closed-form arm emptied the phase that used to own the budget, so
// whatever was second in line has since moved up. This prices the floor, and prices
// the one cost that scales with circuit size, so the two can be compared rather than
// asserted.
//
// Method. The profiler inside `step()` cannot answer this. It takes a clock read
// per phase -- ~16 of them per sample -- and on a circuit whose entire work is ~14 ns
// the instrumentation costs several times what is being measured. That is why
// `bench_circuits` prints "phase shares are clock reads" for the linear cases. So
// this measures the *unprofiled* path on a ladder of circuits differing by exactly
// one element at a time, and fits `ns/sample = a + b*P + c*N`:
//
//   * a -- the floor: the per-sample work no circuit can avoid
//   * b -- what an extra resistor costs
//   * c -- what an extra node costs
//
// Two ladders, because `P` and `N` move together in any single one:
//
//   parallel bank -- k resistors from `in` to `out`, one on to ground. P grows,
//                    N stays put, so the slope is `b` alone.
//   series chain  -- k resistors in a line from `in` to ground. P and N grow
//                    together, so the slope is `b + c`, and subtracting the
//                    parallel slope leaves `c`.
//
// Then `u = Xe*w` timed by itself, because it is the one term that scales as `N*P`
// and both ladders absorb it into `b` and `c`.
//
// **A single pass is not answerable.** The first draft timed each circuit once and
// printed it; re-running that on a quiet machine put the floor anywhere from 9 to
// 20 ns across five passes, and put the 5x8 transfer anywhere from 10 to 13.6. So
// every number below is the **median of nine passes**, and every row prints its low
// and high so an untrustworthy spread says so on the page instead of quietly
// feeding a fit. Passes are interleaved -- all of a ladder is timed once per pass --
// so a background hiccup lands on the whole ladder instead of on whichever circuit
// was unlucky enough to be running.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "wdf/Engine.h"

namespace {

constexpr double kFs = 48000.0;
constexpr int kSamples = 48000 * 4;  // 4 s of signal per circuit
constexpr int kPasses = 9;           // per-circuit timings, median reported
constexpr double kPi = 3.14159265358979323846;

double inputAt(int n) {
    return 0.2 * std::sin(2.0 * kPi * 220.0 * static_cast<double>(n) / kFs);
}

/// A median with its extremes. The extremes are not decoration: on this machine a
/// single pass of the floor has printed 9.21 and 20.14 ns, and a fit through a set
/// like that would be arithmetic about nothing.
struct Stat {
    double med = 0.0;
    double lo = 0.0;
    double hi = 0.0;
};

Stat medianOf(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return {v[v.size() / 2], v.front(), v.back()};
}

struct Row {
    int P = 0;
    int N = 0;
    Stat ns;
    double checksum = 0.0;
};

/// The smallest circuit the engine will compile: one source, one resistor. The
/// resistor has to leave a node free -- `in` to ground alone leaves nothing for
/// the R-type transfer to solve, and the build is refused as singular.
wdf::Circuit minimal() {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addRes("in", "out", 1.0e3);
    return c;
}

/// `k` resistors in parallel from `in` to `out`, then one on to ground. `P = k+1`
/// and `N` does not move, which is what makes this ladder price an element alone.
wdf::Circuit parallelBank(int k) {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    for (int i = 0; i < k; ++i)
        c.addRes("in", "out", 1.0e3);
    c.addRes("out", "0", 1.0e3);
    return c;
}

/// `k` resistors in a line from `in` to ground: one node per junction, so `P` and
/// `N` climb together.
wdf::Circuit seriesChain(int k) {
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    std::string from = "in";
    for (int i = 0; i < k; ++i) {
        const std::string to = (i + 1 == k) ? std::string("0") : ("n" + std::to_string(i));
        c.addRes(from, to, 1.0e3);
        from = to;
    }
    return c;
}

/// One timed pass over one circuit. The engine is rebuilt each pass and the build
/// cost lands outside the timed region; only `step()` is clocked.
double timeCircuit(const wdf::Circuit& c, int& P, int& N, double& checksum) {
    wdf::Engine e(c, kFs);
    P = e.elementCount();
    N = e.nodeCount();

    std::vector<double> node, rootV;
    if (!e.solveDc(node, rootV)) {
        std::printf("  solveDc did not converge (P=%d N=%d)\n", P, N);
        return 0.0;
    }
    e.reset();

    // The engine reads one slot per source, in order; a short vector would be an
    // out-of-range read rather than a default, so it is sized exactly.
    std::vector<double> sv(static_cast<size_t>(e.sourceCount()), 0.0);

    double acc = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int n = 0; n < kSamples; ++n) {
        if (!sv.empty())
            sv[0] = inputAt(n);
        acc += e.step(sv).front();
    }
    const auto t1 = std::chrono::steady_clock::now();
    checksum = acc;
    return std::chrono::duration<double>(t1 - t0).count() / kSamples * 1e9;
}

/// Time a whole ladder, interleaved, and reduce each circuit to a median.
std::vector<Row> measureLadder(const std::vector<wdf::Circuit>& cs) {
    const size_t n = cs.size();
    std::vector<std::vector<double>> runs(n);
    std::vector<Row> out(n);
    for (int pass = 0; pass < kPasses; ++pass)
        for (size_t i = 0; i < n; ++i)
            runs[i].push_back(timeCircuit(cs[i], out[i].P, out[i].N, out[i].checksum));
    for (size_t i = 0; i < n; ++i)
        out[i].ns = medianOf(runs[i]);
    return out;
}

void printLadder(const char* title, const std::vector<Row>& rows) {
    std::printf("\n  %s\n", title);
    std::printf("    %-4s %-4s %12s %18s\n", "P", "N", "ns/sample (med)", "[low, high]");
    for (const Row& r : rows)
        std::printf("    %-4d %-4d %12.2f   [%5.2f, %5.2f]\n", r.P, r.N, r.ns.med, r.ns.lo,
                    r.ns.hi);
}

/// `y = a + b*x`, ordinary least squares. `useN` picks the regressor: `P` prices an
/// element, `N` prices a node. `rms` is the residual against the medians, and a
/// large one means the ladder is not linear and neither number should be trusted.
struct Fit {
    double a = 0.0;
    double b = 0.0;
    double rms = 0.0;
};

Fit lsq(const std::vector<Row>& rows, bool useN) {
    const int n = static_cast<int>(rows.size());
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    for (const Row& r : rows) {
        const double x = static_cast<double>(useN ? r.N : r.P);
        sx += x;
        sy += r.ns.med;
        sxx += x * x;
        sxy += x * r.ns.med;
    }
    Fit f;
    const double den = n * sxx - sx * sx;
    f.b = den != 0.0 ? (n * sxy - sx * sy) / den : 0.0;
    f.a = (sy - f.b * sx) / n;
    double se = 0.0;
    for (const Row& r : rows) {
        const double x = static_cast<double>(useN ? r.N : r.P);
        const double d = r.ns.med - (f.a + f.b * x);
        se += d * d;
    }
    f.rms = std::sqrt(se / n);
    return f;
}

/// `u = Xe * w` timed by itself: the same four-accomulator loop `step()` runs, on
/// buffers filled at run time so nothing is constant-folded.
///
/// This is the only term in `step()` that scales as `N*P`, and the ladders cannot
/// price it -- their `N` either sits still (parallel bank) or tracks `P` (series
/// chain), so its cost is absorbed into `b` and `c`. Timing it directly is what
/// decides whether the dense transfer is worth attacking: the `Xe` column skip was
/// prototyped and reverted in increment 7 because the solve owned the budget and
/// the skip's index indirection cost more than the multiplies it saved. Increment 9b
/// removed that solve from the closed-form arm, so the premise behind the revert has
/// moved. The compression that landed instead compacts the *storage* -- `rebuildLinear`
/// keeps only the live columns -- so the inner loop below stays contiguous and the
/// indirection sits once per sample in the `w` fill, not once per row here. The table
/// times both widths so the saving is a within-run number rather than a recollection.
///
/// One thing it surfaces without settling: `P` not a multiple of four leaves a **scalar
/// tail** in this loop, and a tail sits on one dependency chain where the four
/// accumulators give independence -- so it *should* cost more than the full iteration
/// it replaces. The direction agrees and the magnitude does not: five noisier
/// single-pass runs had 5x7 slower than 5x8 every time, but only two of four
/// median-of-nine runs do, and the other two read them equal. Treat it as unresolved
/// and re-measure rather than quoting either. The code fact is certain:
/// `cathode_follower` (P=5) and `common_cathode_unbypassed` (P=7) run scalar
/// multiply-adds per row that a multiple-of-four `P` would not.
struct MatVec {
    int N = 0;
    int P = 0;
    Stat ns;
    double sink = 0.0;  // summed and printed -- keeps every timed loop observable
};

MatVec timeMatvecOnce(int N, int P) {
    constexpr int kReps = 200000;
    MatVec mv;
    mv.N = N;
    mv.P = P;

    std::vector<double> Xe(static_cast<size_t>(N) * P);
    for (size_t i = 0; i < Xe.size(); ++i)
        Xe[i] = 0.001 * static_cast<double>(i % 7 + 1);
    std::vector<double> w(static_cast<size_t>(P), 0.5);
    std::vector<double> u(static_cast<size_t>(N), 0.0);

    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < kReps; ++r) {
        // Move the input every pass so the compiler cannot collapse the loop into
        // one multiply and a shift by the repetition count.
        w[0] = 0.5 + 1e-9 * static_cast<double>(r);
        const double* wData = w.data();
        for (int n = 0; n < N; ++n) {
            const double* row = &Xe[static_cast<size_t>(n) * P];
            double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
            int j = 0;
            for (; j + 4 <= P; j += 4) {
                s0 += row[j] * wData[j];
                s1 += row[j + 1] * wData[j + 1];
                s2 += row[j + 2] * wData[j + 2];
                s3 += row[j + 3] * wData[j + 3];
            }
            double s = (s0 + s1) + (s2 + s3);
            for (; j < P; ++j)
                s += row[j] * wData[j];
            u[static_cast<size_t>(n)] = s;
        }
        mv.sink += u[0];
    }
    const auto t1 = std::chrono::steady_clock::now();

    mv.ns.med = std::chrono::duration<double>(t1 - t0).count() / kReps * 1e9;
    return mv;
}

MatVec measureMatvec(int N, int P) {
    std::vector<double> runs;
    MatVec out;
    out.N = N;
    out.P = P;
    for (int pass = 0; pass < kPasses; ++pass) {
        const MatVec one = timeMatvecOnce(N, P);
        runs.push_back(one.ns.med);
        out.sink += one.sink;
    }
    out.ns = medianOf(runs);
    return out;
}

/// Everything the ladders and the transfer measurement produce. Kept out of `main`
/// so `main` can be a one-line try/catch around it.
int run() {
    std::printf("\nLiveWDF fixed-cost instrument  fs=%.0f Hz  %d samples x %d passes\n", kFs,
                kSamples, kPasses);
    std::printf("=====================================================================\n");

    // The floor itself: one source, one resistor, nothing else.
    const std::vector<Row> tinyRows = measureLadder({minimal()});
    const Row& tiny = tinyRows.front();
    std::printf("\n  smallest circuit the engine compiles (one source, one resistor)\n");
    std::printf("    P=%d N=%d  ->  %.2f ns/sample  [%.2f, %.2f]   (checksum %+.4f)\n", tiny.P, tiny.N,
                tiny.ns.med, tiny.ns.lo, tiny.ns.hi, tiny.checksum);

    // Ladder 1: P grows, N does not.
    std::vector<wdf::Circuit> bank;
    for (int k = 1; k <= 9; k += 2)
        bank.push_back(parallelBank(k));
    const std::vector<Row> parallel = measureLadder(bank);
    printLadder("parallel bank: P grows, N stays put -- the slope is the cost of an element",
                parallel);

    // Ladder 2: P and N grow together.
    std::vector<wdf::Circuit> chain;
    for (int k = 2; k <= 10; k += 2)
        chain.push_back(seriesChain(k));
    const std::vector<Row> series = measureLadder(chain);
    printLadder("series chain: P and N grow together -- the slope is element + node", series);

    const Fit byElement = lsq(parallel, false);
    const Fit byNode = lsq(series, true);

    std::printf("---------------------------------------------------------------------\n");
    std::printf("  fit  ns/sample = a + b*P + c*N   (on the medians)\n");
    std::printf("    a  (the fixed floor)        : %6.2f ns\n", byElement.a);
    std::printf("    b  (one more resistor)      : %6.2f ns\n", byElement.b);
    std::printf("    c  (one more node)          : %6.2f ns\n", byNode.b - byElement.b);
    std::printf("    residual, parallel ladder   : %6.2f ns rms\n", byElement.rms);
    std::printf("    residual, series ladder     : %6.2f ns rms\n", byNode.rms);

    // The one cost that scales as N*P, timed on the shapes `bench_circuits` compiles.
    //
    // Each real circuit appears twice: once at its full port width and once at the
    // live-column width the engine now stores, because `rebuildLinear` keeps only the
    // columns whose `w` entry can be nonzero. Same run, same buffers, same loop -- so
    // the gap between a pair is what the compaction is worth, measured rather than
    // inferred. (Increment 7's reverted skip was the *dense* row walking an index
    // list inside the inner loop; this changes the storage instead, so the inner loop
    // is contiguous and the indirection happens once per sample, in the `w` fill.)
    struct Shape {
        int N;
        int P;
        const char* what;
    };
    const Shape shapes[] = {
        {1, 1, "minimal"},
        {1, 8, "parallel bank"},
        {4, 5, "cathode_follower, dense    "},
        {4, 3, "cathode_follower, compacted"},
        {5, 7, "unbypassed, dense          "},
        {5, 3, "unbypassed, compacted      "},
        {5, 8, "paper stage, dense         "},
        {5, 4, "paper stage, compacted     "},
        {7, 12, "cascade, dense             "},
        {7, 7, "cascade, compacted         "},
    };
    std::printf("\n  the transfer `u = Xe * w`, on its own (the only N*P term)\n");
    std::printf("    %-28s %-4s %-4s %8s %14s %16s %14s\n", "shape", "N", "P", "N*P",
                "ns/call (med)", "[low, high]", "ns/mult-add");
    double sinkSum = 0.0;
    double stageNs = 0.0;
    std::vector<double> medians;
    medians.reserve(sizeof(shapes) / sizeof(shapes[0]));
    for (const auto& s : shapes) {
        const MatVec mv = measureMatvec(s.N, s.P);
        sinkSum += mv.sink;
        medians.push_back(mv.ns.med);
        if (mv.N == 5 && mv.P == 4)
            stageNs = mv.ns.med;
        std::printf("    %-28s %-4d %-4d %8d %14.2f   [%5.2f, %5.2f]   %9.3f\n", s.what, mv.N,
                    mv.P, mv.N * mv.P, mv.ns.med, mv.ns.lo, mv.ns.hi,
                    mv.ns.med / (mv.N * mv.P));
    }
    // Printed so every timed loop above has an observable result and none of them
    // can be folded away. The value itself means nothing.
    std::printf("    (checksum %+.4f)\n", sinkSum);

    // Deltas are only meaningful within this process -- the machine's two regimes sit
    // ~12-16% apart, so nothing here may be compared with a run from yesterday.
    std::printf("\n    what the compaction buys (same run, so within-regime):\n");
    const int pairs[][2] = {{2, 3}, {4, 5}, {6, 7}, {8, 9}};  // dense/compacted row indices
    for (const auto& p : pairs) {
        const Shape& d = shapes[static_cast<size_t>(p[0])];
        const Shape& c = shapes[static_cast<size_t>(p[1])];
        const double dense = medians[static_cast<size_t>(p[0])];
        const double comp = medians[static_cast<size_t>(p[1])];
        std::printf("      %dx%d -> %dx%d   %5.2f -> %5.2f   %+.2f ns\n", d.N, d.P, c.N, c.P,
                    dense, comp, comp - dense);
    }

    std::printf("---------------------------------------------------------------------\n");
    std::printf("  reading this\n");
    std::printf("    floor  : %.1f ns, paid before any element is looked at -- about a fifth\n",
                byElement.a);
    std::printf("             of the paper stage's sample, and most of a bare linear\n");
    std::printf("             filter's. See `livewdf_bench` for the stage itself.\n");
    std::printf("    transfer: %.1f ns at the paper stage's live-window 5x4 (the full 5x8 is\n",
                stageNs);
    std::printf("             priced in the table above) -- see PROGRESS.md, Findings 4 and 5.\n");
    std::printf("---------------------------------------------------------------------\n");
    std::printf("\n(real-time budget at %.0f Hz: %.0f ns of one core per sample)\n", kFs, 1e9 / kFs);
    return 0;
}

}  // namespace

int main() {
    // Every circuit build sits inside a `try` so a refusal -- a singular transfer, a
    // short source vector -- prints as itself rather than as a silent exit.
    try {
        return run();
    } catch (const std::exception& e) {
        std::printf("\n  failed: %s\n", e.what());
        return 1;
    }
}
