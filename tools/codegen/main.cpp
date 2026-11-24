#include "compiler/EmitRecurrence.h"
#include "format/LiveWDFFile.h"
#include "wdf/Engine.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "Usage: livewdf_codegen circuit.livewdf sample-rate [--external-roots]\n";
        return 2;
    }
    try {
        const std::string_view rateArg = argv[2];
        double rate = 0;
        auto [end, ec] = std::from_chars(rateArg.data(), rateArg.data() + rateArg.size(), rate);
        if (ec != std::errc{} || end != rateArg.data() + rateArg.size() || !std::isfinite(rate) || rate <= 0)
            throw std::invalid_argument("expected a positive finite sample rate");
        const bool external = argc == 4;
        if (external && std::string_view(argv[3]) != "--external-roots")
            throw std::invalid_argument("unknown option");
        wdf::LiveWDFDocument doc;
        std::string error;
        if (!wdf::LiveWDFDocument::loadFromFile(argv[1], doc, error))
            throw std::runtime_error(error);
        const auto circuit = doc.toCircuit();
        auto source = std::find_if(circuit.src.begin(), circuit.src.end(),
                                  [&](const auto& s) { return s.name == doc.inputId(); });
        if (source == circuit.src.end()) throw std::invalid_argument("missing audio input");
        wdf::Engine engine(circuit, rate);
        livewdf::compiler::emitRecurrence(std::cout, engine, static_cast<int>(source - circuit.src.begin()),
                                      doc.outputNet(), external);
        if (!std::cout) throw std::runtime_error("failed to write generated header");
    } catch (const std::exception& error) {
        std::cerr << "livewdf_codegen: " << error.what() << '\n';
        return 1;
    }
}
