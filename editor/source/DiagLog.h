#pragma once

// A tiny file log, for the times when "it crashed" is all anybody knows.
//
// **Why this exists.** A crash in the editor's inspector was reported as
// "clicking a triode takes the editor down", and every explanation offered for
// it was a theory: a use-after-free here, a popup there, a deleted member
// somewhere else. Theories were being tested by rebuilding and asking the user
// to click again, which is slow and tells you only whether a guess was right.
//
// A log turns the crash into a story: the last thing the panel did before it
// died, in order, on disk. The crash is still a crash -- the log cannot stop
// one -- but the log says what led up to it, and that is the difference
// between fixing it and guessing at it.
//
// **It is a diagnostic, not a feature.** It writes to one file, it is opened and
// closed per line, and it is meant to be removed once the bug it was added for is
// gone. Nothing in the product should depend on it.

#include <string>

namespace livewdf::editor {

/// Turn the diagnostic log on or off.
///
/// **Off by default.** The log is a tool for when something has gone wrong, not
/// something to leave running: it writes to a file in the working directory, and
/// a program that quietly leaves files behind is a nuisance. The editor turns it
/// on with `--diag` on the command line.
void setDiagLogEnabled(bool enabled);

/// Whether the log is on.
bool diagLogEnabled();

/// Append a line to the editor's diagnostic log.
///
/// The file is `editor_diag.log` in the working directory, and only when the log
/// is enabled. Writes are best effort: a log that cannot be written must never
/// take the editor down with it, so every failure here is swallowed.
void diagLog(const std::string& line);

}  // namespace livewdf::editor
