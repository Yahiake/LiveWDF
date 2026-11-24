#pragma once

#include <vector>

namespace wdf {

// One scalar law describes a junction's current in milliamps or a hard-rail
// op-amp's internal source in volts. The same-sample driving-point impedance
// is absorbed into this law before solving the remaining mutual coupling.
struct LoadedJunctionPlan {
    struct Term {
        int port = -1, negativePort = -1;
        double gain = 1.0;
    };
    struct Law {
        Term sense;
        int pole = -1, bjt = -1, fet = -1, photo = -1;
        bool forward = false;
        int diode = -1, opposite = -1, oppositePort = -1;
        double resistance = 0.0, inversePole = 0.0;
        std::vector<double> drive, current, lookup;
    };
    struct LinearPole {
        int device, port;
        double sourceGain, historyGain, currentGain;
    };
    struct Buffer {
        int device, port;
        double vbe;
    };
    struct FetStamp {
        int device, port;
        double conductance, transconductance, offset, load;
    };
    struct Entry {
        int row, col;
        double value;
    };
    std::vector<Law> laws;
    std::vector<FetStamp> fets;
    std::vector<LinearPole> linearPoles;
    std::vector<Buffer> buffers;
    std::vector<double> poleMap;
    std::vector<Entry> linear, injection;
    std::vector<double> constant, offset, openMap, voltageMap, mutual;
    std::vector<double> open, drive, raw, q, correction, matrix, low, high;
    std::vector<int> pivots;
    std::vector<double> impedance;
    int misses = 0, cooldown = 0;
    bool seed = true;
    long long revision = -1;
};
} // namespace wdf
