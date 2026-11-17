#pragma once

// The compiler: netlist -> immutable `CompiledCircuit` + diagnostics.
//
// The compiler does not build a `Network`, does not solve DC, does not allocate
// solver state and does not know the sample rate. Those are all things whose
// answer depends on `fs`, and putting them here would make the compile cache
// rate-specific again.
//
// A compile failure the user caused is reported, not thrown: `CompileResult` is
// empty and its diagnostics say why. The netlist compiler itself still throws
// for structural problems, and this layer catches them.

#include "compiler/CompiledCircuit.h"
#include "compiler/Diagnostic.h"
#include "spice/Circuit.h"

namespace livewdf::compiler {

class Compiler {
public:
    CompileResult compile(const wdf::Circuit& netlist,
                          const CompileOptions& options = {}) const;
};

}  // namespace livewdf::compiler
