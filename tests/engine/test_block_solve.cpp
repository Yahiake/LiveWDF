#include <catch2.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "wdf/Engine.h"
#include "support/CircuitCases.h"

// Differential tests for the non-scalar block solve. These circuits have at
// least three Newton blocks and at least two scalar blocks, so they exercise
// `solveRootBlocks` rather than the all-scalar group path or the monolithic
// joint Newton. The joint Newton is the reference: the block pass is an
// optimization only if it solves the same equations, and the full-residual
// fallback counter proves it did not quietly hand the sample back to the
// reference solver.

namespace {

constexpr double kFs = 48000.0;

// A test-only view of the protected solver state. The fallback is not reachable
// through the public API on a valid circuit -- by construction the static graph
// is complete -- so the test has to make the graph stale deliberately and call
// the same routines `step()` would. No test hook is added to the engine.
class BlockProbe : public wdf::Engine {
public:
    using Engine::Engine;

    void setImpedance(int row, int col, double value) {
        Z_[static_cast<size_t>(row) * static_cast<size_t>(K_) +
           static_cast<size_t>(col)] = value;
        for (int j = 0; j < nRed_; ++j) {
            if (redCol_[static_cast<size_t>(j)] == col) {
                if (!Zred_.empty()) {
                    Zred_[static_cast<size_t>(row) * static_cast<size_t>(nRed_) + static_cast<size_t>(j)] = value;
                }
                break;
            }
        }
    }
    void solveBlocks(int maxIter, double tol) { solveRootBlocks(maxIter, tol); }
    void solveJointFromCurrent(int maxIter, double tol) {
        solveRootsNewtonSeeded(maxIter, tol);
    }
    const std::vector<double>& rootVoltages() const { return vv_; }
};

double sine(double amplitude, int n) {
    return amplitude * std::sin(2.0 * 3.14159265358979323846 * 220.0 *
                                static_cast<double>(n) / kFs);
}

void addDiodeBranch(wdf::Circuit& c, const std::string& suffix) {
    const std::string src = "d_in" + suffix;
    const std::string node = "d" + suffix;
    c.addSrc(src, "0", 0.0);
    c.addRes(src, node, 1e3);
    c.addCap(node, "0", 1e-6);
    c.addDiode(node, "0", wdf::DiodeModel(1e-9, 1.0, 0.0), "D" + suffix);
}

// A shared-cathode pair (one two-device block, not scalar) plus two
// independent diodes. This is the multi-device block Newton path, not just the
// two-port single-device path above.
wdf::Circuit sharedCathodePairAndDiodes() {
    wdf::Circuit c = lwdf_test::sharedCathodePair();
    addDiodeBranch(c, "a");
    addDiodeBranch(c, "b");
    return c;
}

// The same pair with one diode: two blocks, only one of them scalar-capable.
wdf::Circuit sharedCathodePairAndOneDiode() {
    wdf::Circuit c = lwdf_test::sharedCathodePair();
    addDiodeBranch(c, "a");
    return c;
}

void requireBlockParity(const char* label, wdf::Circuit (*build)()) {
    const wdf::Circuit fastCircuit = build();
    const wdf::Circuit slowCircuit = build();
    wdf::Engine fast(fastCircuit, kFs);
    wdf::Engine slow(slowCircuit, kFs);

    CAPTURE(label);
    REQUIRE(fast.blockRoots());
    REQUIRE(fast.rootBlockCount() >= 3);
    REQUIRE(slow.setExplicitRoots(false));
    REQUIRE_FALSE(slow.blockRoots());

    fast.reset();
    slow.reset();
    double worst = 0.0;
    for (int n = 0; n < 4800; ++n) {
        std::vector<double> sv(fastCircuit.src.size(), 0.0);
        for (size_t i = 0; i < fastCircuit.src.size(); ++i)
            sv[i] = fastCircuit.src[i].dc;
        // Every zero-DC source is a signal input in these fixtures; the only
        // non-zero source is the supply.
        for (size_t i = 0; i < fastCircuit.src.size(); ++i)
            if (fastCircuit.src[i].dc == 0.0)
                sv[i] = sine(1.0, n);

        const std::vector<double>& a = fast.step(sv);
        const std::vector<double>& b = slow.step(sv);
        REQUIRE(a.size() == b.size());
        for (size_t i = 0; i < a.size(); ++i)
            worst = std::max(worst, std::abs(a[i] - b[i]));
    }
    CAPTURE(worst, fast.blockFallbackCount(), fast.lastBlockResidual());
    WARN(label << ": max node diff=" << worst
               << " V, fallbacks=" << fast.blockFallbackCount()
               << ", last block residual=" << fast.lastBlockResidual());
    REQUIRE(worst < 1e-8);
    // The static partition is correct for these circuits, so the block pass
    // must not need the joint-Newton fallback on any sample. A fallback here
    // would hide a partition bug behind the reference solver.
    REQUIRE(fast.blockFallbackCount() == 0);
    REQUIRE(fast.lastBlockResidual() < 1e-9);
}

}  // namespace

TEST_CASE("block solve matches joint Newton: shared-cathode pair plus diodes",
          "[blocksolve]") {
    requireBlockParity("shared-cathode pair + two diodes",
                       &sharedCathodePairAndDiodes);
}

TEST_CASE("block solve stands down where the joint Newton measured no win",
          "[blocksolve]") {
    // Two blocks, only one of them scalar-capable: the joint matrix is still
    // small and the measured K=2 case is neutral. The pass must not claim the
    // optimization.
    const wdf::Circuit two = sharedCathodePairAndOneDiode();
    wdf::Engine e2(two, kFs);
    REQUIRE(e2.rootBlockCount() == 2);
    REQUIRE_FALSE(e2.blockRoots());
    REQUIRE_FALSE(e2.explicitRoots());
}

TEST_CASE("runtime active-set block solve matches joint Newton",
          "[blocksolve][runtime]") {
    // The static partition does not pay here (one non-scalar block, one
    // scalar-capable block), so the runtime partition is the path that has to
    // reproduce the reference solve on its own.
    const wdf::Circuit circuit = sharedCathodePairAndOneDiode();
    wdf::Engine fast(circuit, kFs);
    wdf::Engine slow(circuit, kFs);
    REQUIRE(fast.setRuntimeBlockSolver(true));
    REQUIRE_FALSE(fast.blockRoots());
    REQUIRE_FALSE(slow.blockRoots());

    fast.reset();
    slow.reset();

    std::vector<double> sv(circuit.src.size(), 0.0);
    for (size_t i = 0; i < circuit.src.size(); ++i)
        sv[i] = circuit.src[i].dc;

    // Zero input: the runtime graph splits the pair from the diode, so it must
    // be the block pass that runs.
    fast.step(sv);
    slow.step(sv);
    REQUIRE(fast.lastRuntimeBlockUsed());
    REQUIRE(fast.runtimeBlockCount() == 2);

    double worst = 0.0;
    for (int n = 0; n < 4800; ++n) {
        for (size_t i = 0; i < circuit.src.size(); ++i)
            sv[i] = circuit.src[i].dc;
        for (size_t i = 0; i < circuit.src.size(); ++i)
            if (circuit.src[i].dc == 0.0)
                sv[i] = sine(0.25, n);

        const std::vector<double>& a = fast.step(sv);
        const std::vector<double>& b = slow.step(sv);
        REQUIRE(a.size() == b.size());
        for (size_t i = 0; i < a.size(); ++i)
            worst = std::max(worst, std::abs(a[i] - b[i]));
    }
    CAPTURE(worst, fast.runtimeBlockCount(), fast.runtimeBlockFallbackCount(),
            fast.lastBlockResidual());
    WARN("runtime block: max node diff="
         << worst << " V, fallbacks=" << fast.runtimeBlockFallbackCount()
         << ", last residual=" << fast.lastBlockResidual());
    REQUIRE(worst < 1e-8);
    REQUIRE(fast.lastConverged());
    REQUIRE(fast.lastResidual() < 1e-9);
}

TEST_CASE("block solver falls back when the static graph is stale",
          "[blocksolve]") {
    const wdf::Circuit circuit = sharedCathodePairAndDiodes();
    BlockProbe block(circuit, kFs);
    BlockProbe joint(circuit, kFs);
    REQUIRE(block.blockRoots());
    REQUIRE(block.blockFallbackCount() == 0);

    // Settle the diode branches so the poisoned cross-block current is
    // measurable. The DC sources make the diode current steady; 200 samples
    // pass the coupling capacitors.
    std::vector<double> sv(circuit.src.size(), 0.0);
    for (size_t i = 0; i < circuit.src.size(); ++i)
        sv[i] = circuit.src[i].dc;
    for (size_t i = 0; i < circuit.src.size(); ++i)
        if (circuit.src[i].dc == 0.0)
            sv[i] = 1.0;
    for (int n = 0; n < 200; ++n) {
        block.step(sv);
        joint.step(sv);
    }
    REQUIRE(block.blockFallbackCount() == 0);

    // Row 0 is the shared-cathode block's first triode plate; column K-1 is the
    // last diode. The real network has no such coupling, so the partition is
    // stale once this entry is nonzero. The block pass must notice the full
    // residual and hand the sample to the joint Newton.
    const int K = block.rootCount();
    REQUIRE(K >= 5);
    REQUIRE(std::abs(block.rootImpedance()[static_cast<size_t>(K - 1)]) < 1e-12);
    block.setImpedance(0, K - 1, 1.0);
    joint.setImpedance(0, K - 1, 1.0);

    block.solveBlocks(60, 1e-12);
    joint.solveJointFromCurrent(60, 1e-12);

    REQUIRE(block.lastBlockFallback());
    REQUIRE(block.blockFallbackCount() == 1);
    REQUIRE(block.lastBlockResidual() > 1e-9);
    REQUIRE(block.lastConverged());
    REQUIRE(joint.lastConverged());
    double worst = 0.0;
    for (int r = 0; r < K; ++r)
        worst = std::max(worst, std::abs(block.rootVoltages()[static_cast<size_t>(r)] -
                                         joint.rootVoltages()[static_cast<size_t>(r)]));
    CAPTURE(worst, block.lastResidual(), joint.lastResidual());
    REQUIRE(worst < 1e-8);
}
