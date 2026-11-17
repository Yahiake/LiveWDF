// Manual micro-benchmark: timing wrightOmega in isolation across operating
// regimes and tracing its cost in running circuits (diode_single).
//
// NOT a correctness gate and NOT registered with CTest -- it is an instrument.
// Build target `livewdf_bench_omega`; run it by hand:
//
//     build\tests\Release\livewdf_bench_omega.exe
//
// The question this answers:
// Increment 14 landed the closed-form diode root at 59.8 ns/sample (vs 110.4 ns
// joint Newton, 1.88x ratio). The ratio is lower than the triode's (~4.5x)
// because the diode closed form costs one `wrightOmega` call per sample.
// Furthermore, Increment 15 (grid conduction) puts a Shockley diode on every
// triode. If `wrightOmega` is ~40 ns, grid conduction makes every tube stage
// pay it. This instrument prices `wrightOmega` directly:
//   1. isolated ns/call across 7 bias regimes (median of 9 interleaved passes);
//   2. Halley iteration count distribution in each regime;
//   3. the exact sequence of z values evaluated during a diode_single audio run,
//      answering exactly how much of diode_single's 59.8 ns is omega;
//   4. head-to-head comparison with a tightened initial guess and Taylor branches.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

#include "devices/WrightOmega.h"
#include "devices/Diode.h"
#include "support/CircuitCases.h"
#include "wdf/Engine.h"

namespace {

using clk = std::chrono::steady_clock;

constexpr int kPasses = 9;
constexpr int kBatchSize = 100000;

struct Stat {
    double med = 0.0;
    double low = 0.0;
    double high = 0.0;
};

Stat summarize(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    Stat s;
    s.low = v.front();
    s.high = v.back();
    s.med = v[v.size() / 2];
    return s;
}

/// Tightened initial guess and branch structure for wrightOmega:
///  - z < -36: exp(z) asymptotic
///  - z < -18: 2-term Taylor ez*(1 - ez) (exact to 1e-16)
///  - z < -8:  4-term Taylor (exact to 6e-14, zero Halley iterations needed)
///  - z in [-8, -1): 4-term Taylor initialization for Halley (1-2 iters)
///  - z in [-1, 1.5): 4th-order polynomial around z=0 (w=Omega), eliminating log1p(exp)
///  - z >= 1.5: asymptotic z - ln(z) + ln(z)/z
inline double wrightOmegaProposed(double z) noexcept {
    if (z < -36.0)
        return std::exp(z);
    if (z < -18.0) {
        const double ez = std::exp(z);
        return ez * (1.0 - ez);
    }
    if (z < -8.0) {
        const double ez = std::exp(z);
        return ez * (1.0 - ez * (1.0 - ez * (1.5 - (8.0 / 3.0) * ez)));
    }
    double w;
    if (z < -1.0) {
        const double ez = std::exp(z);
        w = ez * (1.0 - ez * (1.0 - ez * (1.5 - (8.0 / 3.0) * ez)));
    } else if (z < 1.5) {
        constexpr double kOmega = 0.56714329040978387;
        constexpr double kC1 = 0.36189625663484550;
        constexpr double kC2 = 0.07358742847953280;
        constexpr double kC3 = -0.01217395759714850;
        constexpr double kC4 = 0.00223595562723650;
        w = kOmega + z * (kC1 + z * (kC2 + z * (kC3 + z * kC4)));
    } else {
        const double lz = std::log(z);
        w = z - lz + lz / z;
    }
    for (int i = 0; i < 3; ++i) {
        const double r = z - w - std::log(w);
        const double wp1 = 1.0 + w;
        const double t = wp1 * (wp1 + (2.0 / 3.0) * r);
        const double dw = w * (r / wp1) * (t - 0.5 * r) / (t - r);
        w += dw;
        if (std::fabs(dw) <= 1e-12 * w)
            break;
    }
    return w;
}

struct OmegaDetails {
    double w = 0.0;
    int branch = 0;
    int iters = 0;
    double relError = 0.0;
};

OmegaDetails evalOmegaDetailed(double z, bool proposed) noexcept {
    OmegaDetails d;
    if (!proposed) {
        if (z < -36.0) {
            d.w = std::exp(z);
            d.branch = 0;
            d.iters = 0;
            d.relError = d.w / std::max(1.0, std::abs(z));
            return d;
        }
        if (z < -16.0) {
            const double ez = std::exp(z);
            d.w = ez * (1.0 - ez);
            d.branch = 1;
            d.iters = 0;
            d.relError = std::abs(d.w + std::log(d.w) - z) / std::max(1.0, std::abs(z));
            return d;
        }
        double w;
        if (z < 1.0) {
            d.branch = 2;
            const double ez = std::exp(z);
            w = (z < -8.0) ? ez * (1.0 - 0.5 * ez) : std::log1p(ez);
        } else {
            d.branch = 3;
            const double lz = std::log(z);
            w = z - lz + lz / z;
        }
        int it = 0;
        for (; it < 4; ++it) {
            const double r = z - w - std::log(w);
            const double wp1 = 1.0 + w;
            const double t = wp1 * (wp1 + (2.0 / 3.0) * r);
            const double dw = w * (r / wp1) * (t - 0.5 * r) / (t - r);
            w += dw;
            if (std::fabs(dw) <= 1e-12 * w) {
                ++it;
                break;
            }
        }
        d.w = w;
        d.iters = it;
        d.relError = std::abs(w + std::log(w) - z) / std::max(1.0, std::abs(z));
        return d;
    }

    // Proposed
    if (z < -36.0) {
        d.w = std::exp(z);
        d.branch = 0;
        d.iters = 0;
        d.relError = d.w / std::max(1.0, std::abs(z));
        return d;
    }
    if (z < -18.0) {
        const double ez = std::exp(z);
        d.w = ez * (1.0 - ez);
        d.branch = 1;
        d.iters = 0;
        d.relError = std::abs(d.w + std::log(d.w) - z) / std::max(1.0, std::abs(z));
        return d;
    }
    if (z < -8.0) {
        const double ez = std::exp(z);
        d.w = ez * (1.0 - ez * (1.0 - ez * (1.5 - (8.0 / 3.0) * ez)));
        d.branch = 2;
        d.iters = 0;
        d.relError = std::abs(d.w + std::log(d.w) - z) / std::max(1.0, std::abs(z));
        return d;
    }
    double w;
    if (z < -1.0) {
        d.branch = 3;
        const double ez = std::exp(z);
        w = ez * (1.0 - ez * (1.0 - ez * (1.5 - (8.0 / 3.0) * ez)));
    } else if (z < 1.5) {
        d.branch = 4;
        constexpr double kOmega = 0.56714329040978387;
        constexpr double kC1 = 0.36189625663484550;
        constexpr double kC2 = 0.07358742847953280;
        constexpr double kC3 = -0.01217395759714850;
        constexpr double kC4 = 0.00223595562723650;
        w = kOmega + z * (kC1 + z * (kC2 + z * (kC3 + z * kC4)));
    } else {
        d.branch = 5;
        const double lz = std::log(z);
        w = z - lz + lz / z;
    }
    int it = 0;
    for (; it < 3; ++it) {
        const double r = z - w - std::log(w);
        const double wp1 = 1.0 + w;
        const double t = wp1 * (wp1 + (2.0 / 3.0) * r);
        const double dw = w * (r / wp1) * (t - 0.5 * r) / (t - r);
        w += dw;
        if (std::fabs(dw) <= 1e-12 * w) {
            ++it;
            break;
        }
    }
    d.w = w;
    d.iters = it;
    d.relError = std::abs(w + std::log(w) - z) / std::max(1.0, std::abs(z));
    return d;
}

struct Regime {
    std::string name;
    double zMin;
    double zMax;
    std::vector<double> zInputs;
};

} // namespace

int main() {
    std::printf("\nLiveWDF WrightOmega micro-benchmark instrument\n");
    std::printf("=====================================================================\n");

    std::vector<Regime> regimes = {
        {"deep reverse   (z < -36)",  -100.0, -36.0, {}},
        {"mild reverse   [-36, -16)",  -36.0, -16.0, {}},
        {"neg transition [-16,  -8)",  -16.0,  -8.0, {}},
        {"subthreshold   [ -8,   0)",   -8.0,   0.0, {}},
        {"knee region    [  0,   1)",    0.0,   1.0, {}},
        {"conducting     [  1,  10)",    1.0,  10.0, {}},
        {"heavy conduct  [ 10,  50)",   10.0,  50.0, {}},
    };

    for (auto& reg : regimes) {
        reg.zInputs.resize(kBatchSize);
        const double step = (reg.zMax - reg.zMin) / static_cast<double>(kBatchSize);
        for (int i = 0; i < kBatchSize; ++i) {
            reg.zInputs[static_cast<size_t>(i)] = reg.zMin + (i + 0.5) * step;
        }
    }

    std::printf("  1. Analysis per regime (accuracy and iteration count)\n");
    std::printf("  -------------------------------------------------------------------\n");
    std::printf("  regime                         baseline (avg it)  proposed (avg it)  agreement\n");

    for (const auto& reg : regimes) {
        double avgItBase = 0.0, avgItProp = 0.0;
        double maxDiff = 0.0;
        for (double z : reg.zInputs) {
            OmegaDetails b = evalOmegaDetailed(z, false);
            OmegaDetails p = evalOmegaDetailed(z, true);
            avgItBase += b.iters;
            avgItProp += p.iters;
            const double diff = std::abs(p.w - b.w) / std::max(1e-15, std::abs(b.w));
            maxDiff = std::max(maxDiff, diff);
        }
        avgItBase /= kBatchSize;
        avgItProp /= kBatchSize;
        std::printf("  %-28s     %4.2f iters        %4.2f iters      %.2e\n",
                    reg.name.c_str(), avgItBase, avgItProp, maxDiff);
    }
    std::printf("  -------------------------------------------------------------------\n");

    std::printf("\n  2. Head-to-head timing per regime (median of %d passes, %d calls/pass)\n",
                kPasses, kBatchSize);
    std::printf("  -------------------------------------------------------------------\n");
    std::printf("  regime                         baseline        proposed       speedup\n");

    std::vector<std::vector<double>> baseTimes(regimes.size(), std::vector<double>(kPasses, 0.0));
    std::vector<std::vector<double>> propTimes(regimes.size(), std::vector<double>(kPasses, 0.0));

    for (int p = 0; p < kPasses; ++p) {
        for (size_t r = 0; r < regimes.size(); ++r) {
            const auto& zVec = regimes[r].zInputs;
            // Baseline
            double sumB = 0.0;
            const auto t0 = clk::now();
            for (int i = 0; i < kBatchSize; ++i) {
                sumB += wdf::wrightOmega(zVec[static_cast<size_t>(i)]);
            }
            const auto t1 = clk::now();
            baseTimes[r][static_cast<size_t>(p)] =
                std::chrono::duration<double, std::nano>(t1 - t0).count() / kBatchSize;

            // Proposed
            double sumP = 0.0;
            const auto t2 = clk::now();
            for (int i = 0; i < kBatchSize; ++i) {
                sumP += wrightOmegaProposed(zVec[static_cast<size_t>(i)]);
            }
            const auto t3 = clk::now();
            propTimes[r][static_cast<size_t>(p)] =
                std::chrono::duration<double, std::nano>(t3 - t2).count() / kBatchSize;

            // Keep dead-code eliminator away
            if (sumB == 0.123456 || sumP == 0.123456)
                std::printf("unreachable\n");
        }
    }

    for (size_t r = 0; r < regimes.size(); ++r) {
        Stat sB = summarize(baseTimes[r]);
        Stat sP = summarize(propTimes[r]);
        const double speedup = sB.med / sP.med;
        std::printf("  %-28s  %5.2f ns         %5.2f ns        %5.2fx\n",
                    regimes[r].name.c_str(), sB.med, sP.med, speedup);
    }
    std::printf("  -------------------------------------------------------------------\n");

    std::printf("\n  3. Real circuit trace: diode_single (48,000 samples @ 48 kHz, 1 V drive)\n");
    std::printf("  -------------------------------------------------------------------\n");

    wdf::Circuit c = lwdf_test::diodeSingle();
    wdf::Engine engine(c, 48000.0);
    engine.setExplicitRoots(true);

    const int kSamples = 48000;
    const double kPi = 3.14159265358979323846;

    const auto& dModel = engine.compiled().diode[0].second;
    const double Zee = engine.rootImpedance()[0];
    const double rTot = Zee + dModel.rs;
    const wdf::DiodeModel grpDiode(dModel.isat, dModel.n, rTot);

    std::vector<double> circuitZ(kSamples, 0.0);

    engine.reset();
    for (int n = 0; n < kSamples; ++n) {
        const double in = 1.0 * std::sin(2.0 * kPi * 100.0 * n / 48000.0);
        const auto& nodes = engine.step({in});
        const double vOut = nodes[0];
        double iD = 0.0, diD = 0.0;
        dModel.eval(vOut, iD, diD);
        const double vOc = vOut + Zee * iD;
        const double z = grpDiode.z0 + vOc / grpDiode.nvt;
        circuitZ[static_cast<size_t>(n)] = z;
    }

    std::vector<double> cBaseTimes(kPasses, 0.0);
    std::vector<double> cPropTimes(kPasses, 0.0);

    for (int p = 0; p < kPasses; ++p) {
        double sumB = 0.0;
        const auto t0 = clk::now();
        for (int i = 0; i < kSamples; ++i) {
            sumB += wdf::wrightOmega(circuitZ[static_cast<size_t>(i)]);
        }
        const auto t1 = clk::now();
        cBaseTimes[static_cast<size_t>(p)] =
            std::chrono::duration<double, std::nano>(t1 - t0).count() / kSamples;

        double sumP = 0.0;
        const auto t2 = clk::now();
        for (int i = 0; i < kSamples; ++i) {
            sumP += wrightOmegaProposed(circuitZ[static_cast<size_t>(i)]);
        }
        const auto t3 = clk::now();
        cPropTimes[static_cast<size_t>(p)] =
            std::chrono::duration<double, std::nano>(t3 - t2).count() / kSamples;
    }

    Stat sCB = summarize(cBaseTimes);
    Stat sCP = summarize(cPropTimes);

    std::printf("  omega cost on circuit trace   : baseline %5.2f ns  ->  proposed %5.2f ns  (%4.2fx)\n",
                sCB.med, sCP.med, sCB.med / sCP.med);
    std::printf("  projected diode_single sample : baseline ~59.8 ns  ->  projected ~%4.1f ns\n",
                59.8 - (sCB.med - sCP.med));

    std::printf("\n  4. Anti-parallel 1N4148 pair (Rs = 0.56, Rp = 1k, 1 kHz sine)\n");
    std::printf("  -------------------------------------------------------------------\n");
    wdf::DiodeModel dm(4.0e-9, 1.0, 0.56);
    wdf::AntiParallelDiodeModel pair(dm, dm);
    const int kPairN = 48000;
    const double Rp = 1000.0;
    const int kPairPasses = 5;
    for (double drive : {0.01, 0.1, 1.0}) {
        std::vector<double> vo(static_cast<size_t>(kPairN));
        for (int i = 0; i < kPairN; ++i)
            vo[static_cast<size_t>(i)] =
                drive * std::sin(2.0 * kPi * 1000.0 * i / 48000.0);

        std::vector<double> times;
        double maxResidual = 0.0;
        for (int p = 0; p < kPairPasses; ++p) {
            double sum = 0.0;
            const auto t0 = clk::now();
            for (int i = 0; i < kPairN; ++i) {
                double v = 0.0, i1 = 0.0, i2 = 0.0;
                pair.solvePort(vo[static_cast<size_t>(i)], Rp, v, i1, i2);
                sum += i1 + i2;
                const double f = v - (vo[static_cast<size_t>(i)] - Rp * (i1 - i2));
                maxResidual = std::max(maxResidual, std::abs(f));
            }
            const auto t1 = clk::now();
            times.push_back(
                std::chrono::duration<double, std::nano>(t1 - t0).count() / kPairN);
            if (sum == 0.123456)
                std::printf("unreachable\n");
        }
        Stat s = summarize(times);
        std::printf("  drive %4.2f V: %5.2f ns/call  max|port| %.2e\n",
                    drive, s.med, maxResidual);
    }
    std::printf("=====================================================================\n\n");

    return 0;
}
