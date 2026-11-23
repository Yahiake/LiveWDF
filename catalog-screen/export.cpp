#include "format/Document.h"
#include "spice/SpiceExport.h"
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    wdf::LiveWDFDocument doc;
    std::string error;
    if (!doc.loadFromFile(argv[1], error)) {
        std::cerr << error << '\n';
        return 3;
    }
    const auto out = wdf::spiceExport(doc.toCircuit(), 48000, .05);
    if (!out.ok) return 4;
    std::ofstream target(argv[2]);
    target << out.netlist;
    return target.good() ? 0 : 5;
}
