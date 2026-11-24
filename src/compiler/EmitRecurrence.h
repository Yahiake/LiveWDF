#pragma once

#include <iosfwd>
#include <string>

namespace wdf {
class Engine;
}

namespace livewdf::compiler {
/// Emit a C++20 step at this engine's sample rate, control positions and state.
/// For multiple coupled ports the caller must opt into providing their laws.
void emitRecurrence(std::ostream& out, const wdf::Engine& engine, int inputSource,
                    const std::string& outputNet, bool externalRoots = false);
} // namespace livewdf::compiler
