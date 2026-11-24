#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <span>
#include <vector>

namespace wdf::engine {

// Coherent tones separate harmonics and intermodulation. Transients exercise
// the reactive state and the transition back to the full circuit solver.
struct BipolarPreviewStimulus {
    static constexpr int overloadSection = 33;
    int period;
    int upperMidBin, trebleBin;
    std::vector<double> input;

    BipolarPreviewStimulus(double fs, double peak)
        : period(std::clamp(int(std::round(fs / 93.75)), 256, 2048)),
          upperMidBin(std::clamp(int(std::round(8000. * period / fs)), 1, period / 2 - 1)),
          trebleBin(std::clamp(int(std::round(10000. * period / fs)), 1, period / 2 - 1)),
          input((overloadSection + 2) * period) {
        for (int i = 0; i < int(input.size()); ++i) {
            const int section = i / period;
            const double phase = 2. * std::numbers::pi * (i % period) / period;
            if (section >= 1 && section <= 5)
                input[i] = peak * (section < 3 ? .1 : 1.) * std::sin(7. * phase);
            else if (section >= 6 && section <= 8)
                input[i] = peak * (.55 * std::sin(7. * phase + .2) + .45 * std::sin(53. * phase));
            else if (section == 9) {
                const double e = std::exp(-2. * (i % period) / period);
                input[i] = peak * e * (.51 * std::sin(phase + .8) + .31 * std::sin(9. * phase)
                                      + .18 * std::sin(71. * phase));
            } else if (section >= 14 && section <= 21) {
                // A boosted resonant band can distort on a sustained note while
                // passing a broadband mix at the same total peak voltage.
                const int bin = section < 17 ? 32 : section < 20 ? 67 : 2;
                input[i] = peak * std::sin(bin * phase);
            } else if (section >= 22 && section <= 27) {
                input[i] = peak * std::sin((section < 25 ? upperMidBin : trebleBin) * phase);
            } else if (section >= 28 && section <= 30) {
                // Keep the second harmonic just inside Nyquist at any sample rate.
                input[i] = peak * std::sin((period / 4 - 2) * phase);
            } else if (section == overloadSection) {
                input[i] = i % period == 0 ? 1.05 * peak : .04 * peak * std::sin(7. * phase);
            } else if (section == 10) {
                const int n = i % period;
                if (n < period / 8) input[i] = peak;
                else if (n < period / 4) input[i] = -peak;
                else if (n < period / 2) input[i] = peak * .36 * std::sin(23. * phase);
            }
        }
    }

    double line(std::span<const double> y, int section, int bin) const {
        const double omega = 2. * std::numbers::pi / period;
        std::complex<double> sum{};
        for (int n = 0; n < period; ++n) {
            const double hann = 1. - std::cos(omega * n);
            sum += std::polar(hann, -bin * omega * n) * y[section * period + n];
        }
        return 2. * std::abs(sum) / period;
    }

    bool matchesColor(const std::vector<double>& reference, const std::vector<double>& candidate) const {
        // The low/high fundamental ratio measures compression. Upper harmonics
        // and two-tone products catch lost coloration even when gain is unchanged.
        const auto gainRatio = [](double a, double b) { return a / std::max(1e-12, b); };
        const double refHigh = line(reference, 5, 7), trialHigh = line(candidate, 5, 7);
        const double refLow = line(reference, 2, 7), trialLow = line(candidate, 2, 7);
        const double compression = gainRatio(gainRatio(trialHigh, trialLow), gainRatio(refHigh, refLow));
        if (!(compression >= .99426 && compression <= 1.00578)) return false; // 0.05 dB
        const auto agrees = [&](int section, int bin, double carrier) {
            const double a = line(reference, section, bin), b = line(candidate, section, bin);
            if (std::max(a, b) <= std::max(1e-7, carrier * .0001)) return true; // −80 dBc
            const double ratio = gainRatio(b, a);
            return ratio >= .8414 && ratio <= 1.1886; // 1.5 dB
        };
        for (int harmonic = 2; harmonic <= 8; ++harmonic)
            if (!agrees(5, harmonic * 7, std::max(refHigh, trialHigh))) return false;
        for (const auto& [section, tone] : {std::pair{16, 32}, std::pair{19, 67}, std::pair{21, 2},
                                             std::pair{24, upperMidBin}, std::pair{27, trebleBin},
                                             std::pair{30, period / 4 - 2}}) {
            const double fundamental = std::max(line(reference, section, tone), line(candidate, section, tone));
            for (int h = 2; h <= 6 && h*tone < period/2; ++h)
                if (!agrees(section, h*tone, fundamental)) return false;
        }
        const double carrier = std::max({line(reference, 8, 7), line(reference, 8, 53),
                                         line(candidate, 8, 7), line(candidate, 8, 53)});
        for (int bin : {14, 21, 39, 46, 60, 67, 99, 106, 113})
            if (!agrees(8, bin, carrier)) return false;
        return true;
    }
};

inline bool bipolarPreviewWaveformMatches(std::span<const double> expected, std::span<const double> actual) {
    if (expected.size() != actual.size()) return false;
    double signalEnergy = 0., errorEnergy = 0.;
    for (size_t start = 0; start < expected.size(); start += 64) {
        const size_t count = std::min(size_t(64), expected.size() - start);
        double signal = 0., error = 0., peak = 0., peakError = 0.;
        for (size_t i = start; i < start + count; ++i) {
            const double e = actual[i] - expected[i];
            if (!std::isfinite(e)) return false;
            signal += expected[i] * expected[i];
            error += e * e;
            peak = std::max(peak, std::abs(expected[i]));
            peakError = std::max(peakError, std::abs(e));
        }
        if (std::sqrt(error / count) > 80e-6 + .0015 * std::sqrt(signal / count) ||
            peakError > .00035 + .003 * peak) return false;
        signalEnergy += signal;
        errorEnergy += error;
    }
    return signalEnergy > 1e-12 && errorEnergy < 1.e-6 * signalEnergy;
}

} // namespace wdf::engine
