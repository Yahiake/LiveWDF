#pragma once

#include <iosfwd>
#include <string>

namespace wdf {
class Engine;
}

namespace livewdf::compiler {
enum class RecurrenceTarget { Cxx, C99Float };

/// Emit a step at this engine's sample rate, control positions and state.
/// C99Float supports linear circuits with one audio input and fixed DC sources.
void emitRecurrence(std::ostream& out, const wdf::Engine& engine, int inputSource,
                    const std::string& outputNet, bool externalRoots = false,
                    RecurrenceTarget target = RecurrenceTarget::Cxx);
} // namespace livewdf::compiler
