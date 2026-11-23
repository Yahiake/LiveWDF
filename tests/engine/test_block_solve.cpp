#include <catch2.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
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

TEST_CASE("prepared affine regions preserve a coupled root through clipping and rebuilds",
          "[engine][blocksolve][affine]") {
    auto model = wdf::OpAmpModel::fromType(wdf::OpAmpType::Tl072);
    model.setVRails(7.0, 7.0);
    wdf::Circuit c;
    c.addSrc("in", "0", 0.0);
    c.addRes("in", "sum", 10e3, "Input");
    c.addRes("out", "sum", 100e3, "Feedback");
    c.addCap("out", "0", 100e-12);
    c.addOpAmp("out", "sum", "0", "0", "0", model, "U");
    c.addDiode("out", "sum", wdf::DiodeModel(1e-11, 2.0, 10.0), "D1");
    c.addDiode("sum", "out", wdf::DiodeModel(1e-11, 2.0, 10.0), "D2");
    c.addRes("out", "load", 1e3);
    c.addDiode("load", "0", wdf::DiodeModel(1e-11, 2.0, 10.0), "D3");

    wdf::Engine full(c, kFs);
    wdf::Engine reduced(c, kFs);
    REQUIRE(reduced.prepareMixedAffineReduction(0) > 0);
    const auto verify = [&](int first, int end) {
        std::vector<double> input(1);
        double peakDifference = 0.0;
        for (int n = first; n < end; ++n) {
            const double drive = n < 1000 ? 0.02 : (n < 2500 ? 2.0 : 0.35);
            input[0] = drive * sine(1.0, n);
            const auto& expected = full.step(input);
            const auto& actual = reduced.step(input);
            REQUIRE_FALSE(full.lastStateRejected());
            REQUIRE_FALSE(reduced.lastStateRejected());
            for (size_t node = 0; node < expected.size(); ++node)
                peakDifference = std::max(peakDifference, std::abs(expected[node] - actual[node]));
        }
        CHECK(peakDifference < 1e-7);
    };
    verify(0, 3000);
    REQUIRE(reduced.mixedAffineAccepted() > 0);
    CHECK(reduced.mixedAffineAccepted() <= reduced.mixedAffineAttempts());

    const auto attempts = reduced.mixedAffineAttempts();
    auto stale = reduced.captureMixedAffineSetup(0);
    REQUIRE(stale);
    stale->build();
    CHECK_FALSE(full.installMixedAffineSetup(*stale));
    REQUIRE(reduced.setResistance("Feedback", 47e3));
    REQUIRE(full.setResistance("Feedback", 47e3));
    CHECK_FALSE(reduced.installMixedAffineSetup(*stale));
    verify(3000, 3500);
    CHECK(reduced.mixedAffineAttempts() == attempts);

    auto current = reduced.captureMixedAffineSetup(0);
    REQUIRE(current);
    current->build();
    REQUIRE(reduced.installMixedAffineSetup(*current));
    reduced.useMixedAffineReduction(false);
    verify(3500, 3750);
    CHECK(reduced.mixedAffineAttempts() == attempts);

    reduced.useMixedAffineReduction(true);
    verify(3750, 4000);
    CHECK(reduced.mixedAffineAttempts() > attempts);
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

    REQUIRE(block.setExplicitRoots(false));
    REQUIRE_FALSE(block.blockRoots());
    block.step(sv);
    REQUIRE(block.blockFallbackCount() == 1);
    REQUIRE_FALSE(block.lastBlockFallback());
}

class CascadeAuditProbe : public wdf::Engine {
public:
    CascadeAuditProbe(const wdf::Circuit& c, double fs)
        : Engine(c, fs, true) {}

    void setMode(int mode) {
        reset();
        if (mode == 1) {
            // Mode 1: Prediction-enabled block solving (relaxSweeps = 0, gridLag active)
            explicitRootsAllowed_ = true;
            explicitRootsActive_ = false;
            fineBlockSolver_ = false;
        } else if (mode == 2) {
            // Mode 2: Prediction-disabled block solving (gridLag cleared)
            explicitRootsAllowed_ = true;
            explicitRootsActive_ = false;
            fineBlockSolver_ = false;
            gridLag_.clear();
        } else if (mode == 3) {
            // Mode 3: Block solving with relaxation sweeps (6 sweeps)
            explicitRootsAllowed_ = true;
            explicitRootsActive_ = false;
            fineBlockSolver_ = true;
        } else if (mode == 4) {
            // Mode 4: Full joint Newton reference
            setExplicitRoots(false);
        } else if (mode == 5) {
            // Mode 5: The unmodified explicit closed-form path.
            explicitRootsAllowed_ = true;
            explicitRootsActive_ = true;
            fineBlockSolver_ = false;
        }
    }

    const std::vector<double>& rootVoltages() const { return vv_; }
    const std::vector<double>& openCircuitVoltages() const { return vOc_; }
    const std::vector<double>& rootCurrents() const { return ii_; }
    const std::vector<double>& rootZ() const { return Z_; }
    const std::vector<double>& capStates() const { return capState_; }

    /// True when `step()` would take the block-solve path for this fixture/mode
    /// rather than the joint Newton, so the later-block re-solve case that
    /// moves an earlier block's voltage is genuinely exercised.
    bool blockRootsEngaged() const { return blockRoots(); }

    double computePhysicalResidual(std::vector<double>& F_out, std::vector<double>& i_out) const {
        const int K = K_;
        i_out.assign(static_cast<size_t>(K), 0.0);
        F_out.assign(static_cast<size_t>(K), 0.0);

        for (const auto& dev : rootDevices_) {
            if (dev.kind == wdf::DeviceKind::Triode) {
                const int o = dev.portBase;
                const double vpk = vv_[static_cast<size_t>(o)];
                const double vgint = vv_[static_cast<size_t>(o + 2)];
                const auto& model = comp_.tri[dev.compIndex].model;

                auto cur = model.evaluateQuadric(vgint, vpk);
                double dummySlope = 0.0;
                double ig = model.gridCurrent(vgint, dummySlope);

                i_out[static_cast<size_t>(o)] = cur.ip;
                i_out[static_cast<size_t>(o + 1)] = 0.0;
                i_out[static_cast<size_t>(o + 2)] = ig;
            }
        }

        double maxResid = 0.0;
        for (int r = 0; r < K; ++r) {
            double sumZi = 0.0;
            for (int c = 0; c < K; ++c) {
                sumZi += Z_[static_cast<size_t>(r) * K + c] * i_out[static_cast<size_t>(c)];
            }
            double f = (vv_[static_cast<size_t>(r)] - vOc_[static_cast<size_t>(r)]) + sumZi;
            F_out[static_cast<size_t>(r)] = f;
            maxResid = std::max(maxResid, std::abs(f));
        }
        return maxResid;
    }

    // Re-evaluate every root device law at the final root voltages. This is
    // deliberately *not* a read of ii_: it is the fresh-current path, so a
    // stale current left behind by a block re-solve is visible here.
    double freshRootState(std::vector<double>& i_out, std::vector<double>& F_out) const {
        const int K = K_;
        i_out.assign(static_cast<size_t>(K), 0.0);
        F_out.assign(static_cast<size_t>(K), 0.0);
        std::vector<double> J(static_cast<size_t>(K) * static_cast<size_t>(K), 0.0);
        currents(vv_, i_out, J);

        double maxResid = 0.0;
        for (int r = 0; r < K; ++r) {
            double sumZi = 0.0;
            for (int c = 0; c < K; ++c)
                sumZi += Z_[static_cast<size_t>(r) * K + c] * i_out[static_cast<size_t>(c)];
            const double f = (vv_[static_cast<size_t>(r)] - vOc_[static_cast<size_t>(r)]) + sumZi;
            F_out[static_cast<size_t>(r)] = f;
            maxResid = std::max(maxResid, std::abs(f));
        }
        return maxResid;
    }
};

class StateGuardProbe : public wdf::Engine {
public:
    using Engine::Engine;

    bool accepts(bool explicitRoots, bool converged, double residual) {
        const bool savedExplicit = explicitRootsActive_;
        const bool savedConverged = lastConverged_;
        const double savedResidual = lastResidual_;
        explicitRootsActive_ = explicitRoots;
        lastConverged_ = converged;
        lastResidual_ = residual;
        const bool ok = stateAcceptable();
        explicitRootsActive_ = savedExplicit;
        lastConverged_ = savedConverged;
        lastResidual_ = savedResidual;
        return ok;
    }
};

TEST_CASE("phase2a finite unconverged solve cannot commit reactive state",
          "[phase2a][state]") {
    const wdf::Circuit circuit = lwdf_test::diodeClipper();
    StateGuardProbe engine(circuit, kFs);
    REQUIRE(engine.setExplicitRoots(false));
    engine.reset();

    std::vector<double> sv(circuit.src.size(), 0.0);
    for (size_t i = 0; i < circuit.src.size(); ++i)
        sv[i] = circuit.src[i].dc;
    for (int n = 0; n < 100; ++n)
        engine.step(sv);

    const auto capBefore = engine.capacitorStates();
    const auto indBefore = engine.inductorStates();
    sv[0] = 1.0;
    engine.step(sv, 0, 1e-9);

    REQUIRE_FALSE(engine.lastConverged());
    REQUIRE(engine.lastStateRejected());
    REQUIRE(engine.capacitorStates() == capBefore);
    REQUIRE(engine.inductorStates() == indBefore);
}

TEST_CASE("phase2a state acceptance requires a verified convergence result",
          "[phase2a][state]") {
    const wdf::Circuit circuit = lwdf_test::twoStageCascade();
    StateGuardProbe engine(circuit, kFs);

    // Ordinary nonlinear path: finite, converged only.
    REQUIRE_FALSE(engine.accepts(false, false, 1e-12));
    REQUIRE(engine.accepts(false, true, 1e-12));
    REQUIRE_FALSE(engine.accepts(false, true, std::numeric_limits<double>::quiet_NaN()));
    REQUIRE_FALSE(engine.accepts(false, true, std::numeric_limits<double>::infinity()));

    // Explicit closed form: residual is intentionally unevaluated (NaN), but
    // only a converged status may use that exception.
    REQUIRE(engine.accepts(true, true, std::numeric_limits<double>::quiet_NaN()));
    REQUIRE_FALSE(engine.accepts(true, false, std::numeric_limits<double>::quiet_NaN()));
    REQUIRE_FALSE(engine.accepts(true, true, std::numeric_limits<double>::infinity()));
}

TEST_CASE("phase2a stored root currents match fresh device evaluation",
          "[phase2a][consistency]") {
    const wdf::Circuit circuit = lwdf_test::twoStageCascade();
    constexpr int warm = 200;
    constexpr int samples = 2400;

    for (int mode = 1; mode <= 5; ++mode) {
        CascadeAuditProbe probe(circuit, kFs);
        probe.setMode(mode);

        std::vector<double> sv(circuit.src.size(), 0.0);
        for (size_t i = 0; i < circuit.src.size(); ++i)
            sv[i] = circuit.src[i].dc;
        for (int n = 0; n < warm; ++n)
            probe.step(sv);

        double maxCurrentDiff = 0.0;
        double maxFreshResid = 0.0;
        double maxReportedVsFresh = 0.0;
        double maxPhysicalResid = 0.0;
        long long rejected = 0;
        long long unconverged = 0;
        std::vector<double> iFresh, Ffresh, Fphysical, iPhysical;
        for (int n = 0; n < samples; ++n) {
            const double drive = -1.2 - 1.2 * std::sin(
                2.0 * 3.14159265358979323846 * 100.0 * static_cast<double>(n) / kFs);
            sv[0] = drive;
            probe.step(sv);

            const double freshResid = probe.freshRootState(iFresh, Ffresh);
            const double physicalResid = probe.computePhysicalResidual(Fphysical, iPhysical);
            const auto& stored = probe.rootCurrents();
            double currentDiff = 0.0;
            for (size_t k = 0; k < stored.size(); ++k)
                currentDiff = std::max(currentDiff, std::abs(stored[k] - iFresh[k]));
            maxCurrentDiff = std::max(maxCurrentDiff, currentDiff);
            maxFreshResid = std::max(maxFreshResid, freshResid);
            maxPhysicalResid = std::max(maxPhysicalResid, physicalResid);
            maxReportedVsFresh = std::max(maxReportedVsFresh,
                                          std::abs(probe.lastResidual() - freshResid));
            if (probe.lastStateRejected()) ++rejected;
            if (!probe.lastConverged()) ++unconverged;
        }

        WARN("mode " << mode << ": max |stored-fresh current|=" << maxCurrentDiff
             << " A, max fresh residual=" << maxFreshResid
             << " V, max |reported-fresh residual|=" << maxReportedVsFresh
             << " V, max independent triode residual=" << maxPhysicalResid
             << " V, rejected=" << rejected << ", unconverged=" << unconverged);

        REQUIRE(rejected == 0);
        REQUIRE(unconverged == 0);
        REQUIRE(maxFreshResid <= 1e-9);
        REQUIRE(maxPhysicalResid <= 1e-9);
    }
}

TEST_CASE("phase2a stored root currents match fresh device evaluation on every root type",
          "[phase2a][consistency][roots]") {
    // The consistency invariant above only covered the triode cascade. Sweep it
    // over the coupled / non-triode fixtures too: the shared-cathode diode pair,
    // the anti-parallel diode clipper, the long-tailed diode pair, the EL34
    // push/pull pentode stage, and the general JFET stage. `freshRootState`
    // re-evaluates every root law through the general currents path (no triode
    // special-casing), so it covers diode, JFET and pentode roots. The block
    // modes are the ones where a later block re-solve can move an earlier
    // block's voltage -- the stale-current case the fix targets.
    const std::vector<std::string> targets = {
        "shared_cathode_pair", "push_pull_power_stage", "diode_clipper",
        "long_tailed_pair", "jfet_common_source"};
    constexpr int warm = 200;
    constexpr int samples = 1200;
    int blockEngagements = 0;

    for (const auto& tc : lwdf_test::circuitCases()) {
        bool wanted = false;
        for (const auto& t : targets)
            if (tc.name == t)
                wanted = true;
        if (!wanted)
            continue;

        const wdf::Circuit circuit = (*tc.build)();

        for (int mode = 1; mode <= 4; ++mode) {
            CascadeAuditProbe probe(circuit, kFs);
            probe.setMode(mode);
            const bool engaged = probe.blockRootsEngaged();
            if (engaged)
                ++blockEngagements;

            std::vector<double> sv = lwdf_test::sampleSources(circuit, tc, 0);
            for (int n = 0; n < warm; ++n)
                probe.step(sv);

            double maxCurrentDiff = 0.0, maxFreshResid = 0.0;
            long long rejected = 0, unconverged = 0;
            std::vector<double> iFresh, Ffresh;
            for (int n = 0; n < samples; ++n) {
                sv = lwdf_test::sampleSources(circuit, tc, n);
                probe.step(sv);

                const double freshResid = probe.freshRootState(iFresh, Ffresh);
                const auto& stored = probe.rootCurrents();
                double currentDiff = 0.0;
                for (size_t k = 0; k < stored.size(); ++k)
                    currentDiff = std::max(currentDiff, std::abs(stored[k] - iFresh[k]));
                maxCurrentDiff = std::max(maxCurrentDiff, currentDiff);
                maxFreshResid = std::max(maxFreshResid, freshResid);
                if (probe.lastStateRejected())
                    ++rejected;
                if (!probe.lastConverged())
                    ++unconverged;
            }

            WARN("roots " << tc.name << " mode " << mode
                 << ": max|stored-fresh current|=" << maxCurrentDiff
                 << " A, max fresh residual=" << maxFreshResid
                 << " V, blockRoots=" << (engaged ? 1 : 0)
                 << ", rejected=" << rejected << ", unconverged=" << unconverged);

            // The stored current vector must equal a fresh evaluation at the
            // returned voltages, and the fresh residual must meet tolerance.
            REQUIRE(maxCurrentDiff <= 1e-12);
            REQUIRE(maxFreshResid <= 1e-9);
            // State-commit acceptance invariant: an unconverged sample may not
            // commit state, so every non-converged sample is rejected.
            REQUIRE(rejected == unconverged);
        }
    }
    // The anti-parallel diode pair engages the block path on modes 1-3; the
    // dedicated multi-block fixture below adds the shared-cathode-pair re-solve.
    REQUIRE(blockEngagements >= 3);

    // The genuine multi-device, multi-block re-solve: a shared-cathode pair
    // (one two-device block, non-scalar) plus two independent diodes gives
    // >= 3 blocks, so a later diode block re-solve can move the pair's voltage.
    // This is the case the fresh-current re-evaluation before residual
    // certification exists to protect; the CircuitCases fixtures above never
    // have this shape.
    {
        const wdf::Circuit multi = sharedCathodePairAndDiodes();
        // Modes 1-3 are the block-solve modes; mode 4 is the joint-Newton
        // reference, which deliberately does not engage the block path.
        for (int mode = 1; mode <= 3; ++mode) {
            CascadeAuditProbe probe(multi, kFs);
            probe.setMode(mode);
            // Force the multi-block path: assert the fixture actually engages
            // it, so the check is not quietly measuring the joint Newton.
            REQUIRE(probe.blockRootsEngaged());

            std::vector<double> sv(multi.src.size(), 0.0);
            for (size_t i = 0; i < multi.src.size(); ++i)
                sv[i] = multi.src[i].dc;
            for (int n = 0; n < warm; ++n)
                probe.step(sv);

            double maxCurrentDiff = 0.0, maxFreshResid = 0.0;
            long long rejected = 0, unconverged = 0;
            std::vector<double> iFresh, Ffresh;
            for (int n = 0; n < samples; ++n) {
                for (size_t i = 0; i < multi.src.size(); ++i)
                    if (multi.src[i].dc == 0.0)
                        sv[i] = sine(1.0, n);
                probe.step(sv);

                const double freshResid = probe.freshRootState(iFresh, Ffresh);
                const auto& stored = probe.rootCurrents();
                double currentDiff = 0.0;
                for (size_t k = 0; k < stored.size(); ++k)
                    currentDiff = std::max(currentDiff, std::abs(stored[k] - iFresh[k]));
                maxCurrentDiff = std::max(maxCurrentDiff, currentDiff);
                maxFreshResid = std::max(maxFreshResid, freshResid);
                if (probe.lastStateRejected())
                    ++rejected;
                if (!probe.lastConverged())
                    ++unconverged;
            }
            WARN("roots shared-cathode-pair+2 diodes mode " << mode
                 << ": max|stored-fresh current|=" << maxCurrentDiff
                 << " A, max fresh residual=" << maxFreshResid
                 << " V, blockRoots=1, rejected=" << rejected
                 << ", unconverged=" << unconverged);
            REQUIRE(maxCurrentDiff <= 1e-12);
            REQUIRE(maxFreshResid <= 1e-9);
            REQUIRE(rejected == unconverged);
        }
    }
}

TEST_CASE("two-stage cascade grid conduction convergence investigation", "[reproduce][grid]") {
    const wdf::Circuit circuit = lwdf_test::twoStageCascade();

    std::printf("\n=== Two-Stage Cascade Grid Conduction Investigation ===\n");
    std::printf("%-5s %-5s %-9s %-10s %-12s %-14s %-14s %-5s %-5s\n",
                "Mode", "Smp", "Vin(V)", "Vgk2(V)", "Ig2(A)", "EngineResid", "TrueResid(V)", "Conv", "Cmt");

    for (int mode = 1; mode <= 4; ++mode) {
        CascadeAuditProbe probe(circuit, kFs);
        probe.setMode(mode);

        std::vector<double> sv(circuit.src.size(), 0.0);
        for (size_t i = 0; i < circuit.src.size(); ++i)
            sv[i] = circuit.src[i].dc;

        // Warm up at DC
        for (int n = 0; n < 50; ++n) {
            sv[0] = 0.0;
            probe.step(sv);
        }

        std::vector<double> F(6), iAct(6);
        for (int n = 50; n <= 65; ++n) {
            // Ramp input negative to drive stage 1 plate high and stage 2 grid positive
            sv[0] = -0.15 * static_cast<double>(n - 49);

            const auto capBefore = probe.capStates();
            probe.step(sv);
            const auto capAfter = probe.capStates();
            bool stateCommitted = (capBefore != capAfter);

            const double trueResid = probe.computePhysicalResidual(F, iAct);
            const double engResid = probe.lastResidual();
            const bool engConv = probe.lastConverged();
            const double vgk2 = probe.rootVoltages()[5];
            const double ig2 = iAct[5];

            std::printf("%-5d %-5d %-9.2f %-10.4f %-12.4e %-14.4e %-14.4e %-5s %-5s\n",
                        mode, n, sv[0], vgk2, ig2, engResid, trueResid,
                        engConv ? "T" : "F", stateCommitted ? "Y" : "N");

            // Integrity assertions:
            // 1. Residual must never be NaN
            REQUIRE_FALSE(std::isnan(engResid));
            // 2. Both reported and independently computed residual must satisfy tolerance
            REQUIRE(engResid <= 1e-9);
            REQUIRE(trueResid <= 1e-9);
            // 3. Convergence must be genuine
            REQUIRE(engConv);
            // 4. Sample accepted and state committed
            REQUIRE(stateCommitted);
        }
    }
}


TEST_CASE("nodes with common root currents retain the scalar transfer", "[engine][transfer]") {
    struct Probe : wdf::Engine {
        using Engine::Engine;
        bool hasSharedRows() const { return !rootPanels_.empty(); }
        double scalarDifference() const {
            double error = 0.0;
            for (int n = 0; n < N_; ++n) {
                double drop = 0.0;
                for (int j = xrRowOffsets_[n]; j < xrRowOffsets_[n + 1]; ++j)
                    drop += xrVals_[j] * ii_[xrCols_[j]];
                error = std::max(error, std::abs(node_[n] - (u_[n] - drop)));
            }
            return error;
        }
    };
    wdf::Circuit circuit;
    circuit.addSrc("in", "0", 0.0);
    std::string previous = "in";
    for (int i = 0; i < 24; ++i) {
        const std::string node = "n" + std::to_string(i);
        circuit.addRes(previous, node, 1000.0);
        circuit.addRes(node, "0", 20000.0);
        if (i % 6 == 0)
            circuit.addDiode(node, "0", wdf::DiodeModel(1e-9, 1.0, 0.0), "D" + node);
        previous = node;
    }
    Probe engine(circuit, kFs);
    REQUIRE(engine.hasSharedRows());
    for (int n = 0; n < 48; ++n) {
        engine.step({0.6 * std::sin(n * 0.2)});
        REQUIRE(engine.lastConverged());
        CHECK(engine.scalarDifference() < 1e-12);
    }
}
