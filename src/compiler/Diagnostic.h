#pragma once

// Diagnostics produced by the compiler, and the options that decide how hard it
// looks before handing back a circuit.
//
// A compile failure that the user caused (a zero-ohm resistor, a pot whose
// backing resistors are missing) is a *diagnostic*, not an exception: the editor
// needs a sentence and a part id, and the plugin's loader needs to refuse the
// file without unwinding through JUCE. Exceptions stay for structural failures
// inside the netlist compiler, which the compiler catches and turns into the
// same shape.

#include <string>

namespace livewdf::compiler {

enum class Severity {
    Info,
    Warning,
    Error,
};

struct Diagnostic {
    Severity severity = Severity::Error;
    /// Stable machine-readable code, e.g. "element.invalid-value".
    std::string code;
    /// Human-readable sentence, safe to show a user.
    std::string message;
    /// Optional component id the diagnostic is about.
    std::string part;
    /// Optional pin/net the diagnostic is about.
    std::string pin;
};

struct CompileOptions {
    /// Strict compile: a value the engine cannot represent is an error. A
    /// non-strict compile reports the same finding as a warning and lets the
    /// circuit through, which is useful for inspection tools that want to show
    /// a half-drawn sheet rather than refuse it.
    bool strict = true;

    /// Caller revision, normally a document id or hash. When empty the compiler
    /// derives a structural fingerprint from the netlist. A supplied revision is
    /// returned verbatim, so a caller that needs cache invalidation to cover
    /// device model-card parameters (which the fingerprint does not walk) can
    /// provide its own.
    std::string revision;
};

}  // namespace livewdf::compiler
