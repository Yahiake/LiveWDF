#pragma once

// Nets come from the **drawing**, never from a name written beside a pin.
//
// A `.livewdf` file stores parts and wires and nothing else about connectivity.
// What a pin is connected to is answered by where it is drawn: two pins at one
// point, or joined by a wire, or landing along a wire, are one net. This is what
// both reference implementations do -- Celine's `Schematic::extractNets()` runs
// union-find over pin points and wire ends, and LiveSPICE rebuilds nodes from
// wire-connected geometry (`Schematic.RebuildNodes`) and attaches a symbol's
// terminals by `NodeAt(point)` -- and it is the only storage rule that cannot
// lie about the drawing it came with.
//
// The alternative it replaces is the first editor's `map<pin, netName>` per
// component: connectivity as strings, so "a wire" was a side effect of two pins
// happening to carry the same name. That is what made floating nodes possible
// (a pin *named* a net it was drawn nowhere near) and what made a rail able to
// claim two different nets for one value, or one net for two conflicting ones.
// Those could not be reported, only silently reconciled -- which is the "bad
// engineering" this file removes.
//
// It asks the palette (`format/PartLibrary.h`) where a part's pins sit rather
// than owning a second pin table: two accounts of one fact is the bug class this
// project keeps being told off for.
//
// Names are assigned the way Celine assigns them: a net's name comes from the
// *terminals drawn on it* (ground/in/out or a net label), and everything else is auto-numbered.
// A supply -- a rail or a source -- names nothing; it feeds whatever net it is
// drawn onto. LiveSPICE's `Rail` and Celine's PSU both do this: their part name
// goes into the equations ("i"+Name, an unknown current), never into the net.
// A part's *id* labels the part, for the inspector and the plugin; a net's name
// labels the net.

#include <string>
#include <vector>

#include "format/Document.h"

namespace wdf {

/// What the drawing says is connected to what.
///
/// `netOfPin[i][p]` is the net index of pin `p` of `components[i]`, in the
/// order the caller passed -- the same order a document stores its parts. Pin
/// `p` is the `p`-th pin of the part's type (`format/PartLibrary.h`), so the
/// palette's pin names line up with these indices by construction. A part whose
/// type the palette does not know gets an empty row: there are no pins to place,
/// and inventing a position is what a reader used to do by defaulting to ground.
///
/// `netNames[net]` is the name to hand `Circuit`. **Two nets may share a name,
/// and that is not a bug** -- every ground symbol names its net by the
/// document's ground name, so grounding a pin in two places connects them
/// without a wire drawn across the sheet, which is how ground is supposed to
/// work. `Circuit` maps nodes by name, so identically named nets become one
/// node when the circuit is built.
///
/// `netOfWire[i]` is the net index of wire `i` (its first drawn point's net),
/// parallel to the `wires` argument. It exists so a reader can name a wire's
/// net without running a second union-find that would number the nets
/// differently.
///
/// `danglingNets` are the nets with exactly one pin on them. Almost always a
/// mistake -- a part left dangling, or a wire that stops one grid square short
/// -- so the editor can point at the part instead of the solver refusing over a
/// singular matrix. A net with a pin and a wire that goes nowhere is dangling
/// too: the wire is drawn, it connects nothing.
struct DerivedNets {
    std::vector<std::vector<int>> netOfPin;
    std::vector<std::string> netNames;
    std::vector<int> netOfWire;
    std::vector<int> danglingNets;
};

/// Derive the nets of one drawing.
///
/// `components` and `wires` are the drawing; the three names are what the
/// document calls its ground, input and output nets, so a sheet drawn against
/// other names still reads the way it was drawn rather than being refused.
DerivedNets deriveNets(const std::vector<SchematicComponent>& components,
                       const std::vector<SchematicWire>& wires,
                       const std::string& groundName = "0",
                       const std::string& inputName = "in",
                       const std::string& outputName = "out");

/// The same, asked of a whole document -- the parts, the wires and the
/// document's own terminal names.
DerivedNets deriveNets(const LiveWDFDocument& doc);

} // namespace wdf
