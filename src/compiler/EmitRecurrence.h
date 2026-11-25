#pragma once

#include <iosfwd>
#include <string>

namespace wdf {
class Engine;
}

namespace livewdf::compiler {
/// Emit a C++20 step at this engine's sample rate, control positions and state.
/// Unsupported device groups require a caller-supplied nonlinear root solver.
void emitRecurrence(std::ostream& out, const wdf::Engine& engine, int inputSource,
                    const std::string& outputNet, bool externalRoots = false);
} // namespace livewdf::compiler
