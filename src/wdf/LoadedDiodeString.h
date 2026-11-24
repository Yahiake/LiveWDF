#pragma once

#include <vector>

namespace wdf {
struct Compiled;
struct RootLayout;

struct LoadedDiodeString {
    int first, second, firstPort, secondPort;
    double firstShunt, secondShunt;
    std::vector<double> firstDrop;

    // With no other connection to the midpoint, the current through both
    // legs is identical, including the current through each shunt.
    double tabulate(const Compiled& circuit, double voltage, double& drop) const;
};

// Recognize a string opposite the single diode d. One fixed shunt must
// anchor the otherwise floating midpoint in the compiled linear network.
bool findLoadedDiodeString(const Compiled& c, const RootLayout& layout,
                                  int d, const std::vector<bool>& used, LoadedDiodeString& result);
} // namespace wdf
