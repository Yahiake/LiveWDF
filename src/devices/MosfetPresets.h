#pragma once

// The MOSFET preset cards the editor exposes and the document converter reads.
//
// **Why this is a shared table.** The preset numbers used to exist twice: once
// in the converter's `if (preset == ...)` chain and once in the inspector's
// "load the preset" chain. A card added to one and forgotten in the other made
// the inspector show one device and the engine build another. This is the one
// table both read.
//
// These are simplified square-law fits for the same paper model as
// `devices/Mosfet.h`, not complete SPICE Level-1 cards. `K` is the effective
// `mu*Cox*W/L`; `Vth` is a positive magnitude for both polarities. `pChannel`
// says which way the drain current and gate voltage are mirrored. The names are
// the part numbers the fits approximate; the values are deliberately rounded to
// the precision this model can distinguish.

#include <cstddef>
#include <string_view>

namespace wdf {

struct MosfetPreset {
    const char* name;
    double K;             // A/V^2
    double Vth;           // V, positive magnitude
    double lambda;        // 1/V
    bool pChannel;
};

inline constexpr MosfetPreset kMosfetPresets[] = {
    // N-channel
    {"2N7000",  20.0e-3, 2.1,  0.01,  false},
    {"BS170",   20.0e-3, 2.1,  0.01,  false},
    {"BSS138",   5.0e-3, 1.3,  0.02,  false},
    {"2N7002",  20.0e-3, 2.0,  0.01,  false},
    {"IRLZ44N", 150.0e-3, 1.5, 0.01,  false},
    {"IRF540N", 80.0e-3, 2.5,  0.01,  false},
    {"AO3400A", 120.0e-3, 1.2, 0.02,  false},
    {"IRF840",  30.0e-3, 3.0,  0.005, false},

    // P-channel. Selecting one is what makes a placed MOSFET P-channel; the
    // inspector also shows an explicit polarity row and changing it turns the
    // preset into "Custom" so an N-channel card name is never left on a
    // P-channel device.
    {"IRF9540N",  80.0e-3, 3.77, 0.01,  true},
    {"IRF9Z34N",  60.0e-3, 3.18, 0.01,  true},
    {"IRF4905",   90.0e-3, 3.54, 0.005, true},
    {"IRLML6402", 80.0e-3, 1.0,  0.02,  true},
    {"BSS84",     30.0e-3, 2.0,  0.01,  true},
};

inline constexpr int kNumMosfetPresets =
    static_cast<int>(sizeof(kMosfetPresets) / sizeof(kMosfetPresets[0]));

inline const MosfetPreset* findMosfetPreset(std::string_view name) noexcept {
    for (int i = 0; i < kNumMosfetPresets; ++i) {
        const std::string_view candidate(kMosfetPresets[i].name);
        if (candidate.size() != name.size())
            continue;
        bool equal = true;
        for (std::size_t c = 0; c < name.size(); ++c) {
            const char a = candidate[c];
            const char b = name[c];
            const char la = (a >= 'a' && a <= 'z') ? static_cast<char>(a - 'a' + 'A') : a;
            const char lb = (b >= 'a' && b <= 'z') ? static_cast<char>(b - 'a' + 'A') : b;
            if (la != lb) {
                equal = false;
                break;
            }
        }
        if (equal)
            return &kMosfetPresets[i];
    }
    return nullptr;
}

}  // namespace wdf
