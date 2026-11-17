#pragma once

// Engineering-notation value formatting and parsing, for the value editors and
// the labels on the sheet.
//
// **Ported from Celine** (`SymbolPainter::formatValue` / `parseValue` /
// `formatRatio`, `Celine/source/ui/SchematicSymbols.cpp`), because the rules are
// not obvious and Celine paid for each one:
//
//   * `formatValue` writes engineering notation the way a schematic writes it
//     ("4.7k", "2.2M", "100n", "1.5"), two significant decimals at most and no
//     trailing zeros.
//   * Upper-case **M is mega**, lower-case **m is milli**, and `parseValue` spells
//     that out *before* any case folding -- because `formatValue` writes an
//     upper-case M for mega, so a value read straight back off its own editor
//     would otherwise parse as millis. Celine's note: opening the inspector on a
//     2M2 grid leak and pressing return unchanged turned it into a short.
//   * `"meg"` keeps meaning mega in any case, since that is what a netlist writes.
//   * `"4k7"` means 4.7k -- the suffix standing in for the decimal point.
//
// One deliberate adaptation: Celine's pot functions take its `Taper` enum; our
// document stores the taper as a **string** (`properties["taper"]`, e.g.
// "Linear"/"Logarithmic"), so the pot helpers here speak that string. A Celine
// taper enum would be a second source of truth for something `.livewdf` already
// carries.

#include <string>

namespace livewdf::editor {

/// Engineering notation: `value` scaled to a T/G/M/k/""/m/u/n/p prefix and a
/// unit, trimmed to at most two decimals with no trailing zeros. `0` when zero.
std::string formatValue(double value, const std::string& unit);

/// A dimensionless pair written plainly, no prefixes: "1:10", not "100m:1".
std::string formatRatio(double primary, double secondary);

/// A pot's marking from its value and taper: "A250K" for a letter-coded taper,
/// "10A250K" for a numbered one (with the space the code earns), or "0" when the
/// value is unset.
std::string formatPotValue(double value, const std::string& taper);

/// Parses what `formatValue` produces, plus anything close enough: "4k7",
/// "2.2M", "10k ohm", "100nF", "9V", "40ms", "1e3".
/// Returns the number found (0 when there is none); `*ok`, when given, says
/// whether a number was found at all.
double parseValue(const std::string& text, bool* ok = nullptr);

/// Reads back what `formatPotValue` writes, and plain values too. On success,
/// `*taperGiven` says whether the text carried a taper code, and `*value` is the
/// number. Returns false when there is no number.
bool parsePotValue(const std::string& text, double& value, std::string& taper,
                   bool& taperGiven);

}  // namespace livewdf::editor
