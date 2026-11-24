#pragma once

#include <iosfwd>

namespace wdf { class Engine; }

namespace livewdf::compiler {
// Append the optional split form after the CircuitStep emitted for the same
// engine. By default stage 1's influence on stage 0 is one internal sample old.
void emitDelayed(std::ostream& out, const wdf::Engine& engine, double sampleRate, bool reverse = false);
} // namespace livewdf::compiler
