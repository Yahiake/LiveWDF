#pragma once

// The editable schematic: parts, wires, and the connectivity between them.
//
// Why this file exists at all -- and why it is the fix for the first editor --
// is that the first editor had no model. It kept a `std::map<pin, netName>` on
// each component and derived the wires by joining every pin that shared a net,
// so "a wire" was not a thing that could be selected, deleted or dragged: it was
// a side effect of two pins happening to carry the same string. Wiring two pins
// together *renamed* one net onto the other, which meant a wire could not be
// rerouted without rewiring, could not be half-deleted, and drew an L through
// whatever pins it found rather than where the user put it. Nothing about that
// behaves like a drawing, because a drawing was not what was stored.
//
// This model stores what the drawing *is*:
//
//   * a `Part` is a component at a grid position, with an orientation, a
//     mirror flag, and the document's own values/properties;
//   * a `Wire` is a two-point axis-aligned segment, and it is selected, dragged
//     and deleted in its own right;
//   * pin positions come from one table (`PartLibrary`), so the symbol, the wire
//     and the hit test all agree on where a pin is;
//   * connectivity is *derived* -- `extractNets()` runs union-find over pins and
//     wire ends at solve time -- so editing the drawing cannot leave the netlist
//     stale.
//
// **Grid units, not world pixels -- the one coordinate decision.** The editor
// counts in grid squares; a square is `SheetView::gridSize` pixels, so that
// number *is* the zoom and there is no separate scale/offset pair. `toGrid` and
// `toPixel` are the only two conversions, and every coordinate in this file --
// `PinOffset`, `Wire` ends, a part's `visual.x/y` -- is in grid units. The first
// editor here mixed a world space with a scale and an offset, and every feature
// then had to remember which side of the divide a number was on; that is the bug
// class this removes. (Full text lives here; the headers that need it point
// back at this note rather than repeat it.)
//
// The port is Celine's `Schematic`/`Element` pair (`Celine/source/Schematic/`),
// which is the working reference for exactly this: a document an editor can
// edit, with net extraction kept out of the drawing. The ported decisions, and
// why each is kept, are called out at the definitions below.

#include "format/LiveWDFFile.h"

#include <juce_graphics/juce_graphics.h>

#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace livewdf::editor {

/// Most pins any part in the palette has. `netOfPart` rows are sized per part
/// rather than to this bound, so the number is only a safety margin for a caller
/// that indexes pins by a fixed limit -- the tests, and nothing in the app.
inline constexpr int kMaxPins = 8;

/// A pin's position relative to its part's centre, in grid units, before the
/// part's own rotation and mirror are applied.
struct PinOffset {
    int x = 0;
    int y = 0;
};

/// A drawn connection. Always axis-aligned: a diagonal has no meaningful
/// "does this pin touch it" test, and every schematic editor worth using draws
/// in right angles anyway. (Ported from Celine's `Wire`, for that reason.)
///
/// **Two id types, and the asymmetry is deliberate.** A wire is identified by an
/// `int`, with `-1` as the "no wire" sentinel -- wires are the editor's own
/// invention and never appear in the file, so an integer is enough and matches
/// Celine. A *part* is identified by its `std::string` id instead (see
/// `wdf::SchematicComponent`): that string is the document's identity for the
/// part -- `"R_plate"`, `"V1"` -- and it is what a `.livewdf` file stores, so
/// making it an `int` would break the format and the ability to reload a
/// document. The two are not "inconsistent"; they answer to different owners.
struct Wire {
    /// Identity, so a wire can be selected and a selection can survive its
    /// neighbours being deleted. An index into the vector would not: removing
    /// one wire renumbers every wire after it.
    int id = 0;

    int ax = 0, ay = 0;   ///< first end, grid units
    int bx = 0, by = 0;   ///< second end, grid units

    bool isVertical() const noexcept { return ax == bx; }
    bool isHorizontal() const noexcept { return ay == by; }

    /// True if `(px, py)` lies anywhere along this wire, ends included. This is
    /// what makes a T-junction work: a pin or a wire end landing partway along
    /// another wire is connected to it, with no explicit junction needed.
    bool contains(int px, int py) const noexcept {
        if (isVertical())
            return px == ax && py >= std::min(ay, by) && py <= std::max(ay, by);
        if (isHorizontal())
            return py == ay && px >= std::min(ax, bx) && px <= std::max(ax, bx);
        return false;
    }
};

/// What is connected to what, as produced by `Schematic::extractNets()` -- which
/// is `wdf::DerivedNets` in `format/SchematicNets.h`, in the shape this editor's
/// renderers want. The two structs having the same field names is the point:
/// there is one answer to "what is connected to what", one place that decides it,
/// and this is a copy of it. A second union-find here is the drift this project
/// keeps being told off for -- and it is silent, because a pin landing one grid
/// square off in one of the two is a part wired to nothing.
///
/// `netOfPart[i]` is one row per part, in `getParts()` order, sized to that part's
/// pin count (`format/PartLibrary.h` says how many a type has), and `netOfPart[i][p]`
/// is the net index of pin `p`. Pin `p` is the `p`-th pin of the part's *type*, so
/// the palette's pin names line up with these indices by construction.
///
/// `netNames[net]` is the node name handed to `Circuit`. **Two nets may share a
/// name, and that is not a bug** -- every ground symbol names its net "0", so
/// grounding a pin in two places connects them with no wire drawn across the
/// sheet, which is how ground is meant to work.
struct NetList {
    std::vector<std::vector<int>> netOfPart;
    std::vector<std::string> netNames;

    /// Parallel to `Schematic::getWires()`: `netOfWire[i]` is the net index of
    /// wire `i`'s endpoints, using the same numbering as `netNames`. This is what
    /// lets `toDocument()` name a wire's net without running a second union-find
    /// that would number the nets differently.
    std::vector<int> netOfWire;

    /// Nets with only one thing attached. Almost always a mistake -- a part left
    /// dangling, or a wire that stops one grid square short -- so the editor can
    /// point at them.
    std::vector<int> danglingNets;
};

/// Where a scope probe reads: two node names, or the same name against ground.
/// Empty for "not attached".
struct ProbeReadout {
    std::string node;       ///< node measured, or "" when dangling
    std::string reference;  ///< the other pin's node, or "" when it is ground
};

// ============================================================================
// The document
// ============================================================================

/// A schematic: some parts, some wires, and the ability to say which nets they
/// form. Deliberately just a document -- it never builds a `Circuit`, never
/// touches audio, and has no idea one exists.
class Schematic {
public:
    // ------------------------------------------------------------------
    // Parts
    // ------------------------------------------------------------------

    /// Adds a part of `type` at a grid position and returns its id. The part is
    /// made from `PartLibrary`, so its pins and defaults always match the
    /// palette. Returns "" if `type` is not placeable.
    std::string addPart(const std::string& type, int x, int y);

    /// Adds a part constructed elsewhere (a clone, or a load). The id is
    /// reassigned if it is blank or already in use. Returns the id.
    std::string addPart(wdf::SchematicComponent part);

    /// Removes a part, and every wire end that no longer touches anything.
    /// Returns how many wires went with it.
    int removePart(const std::string& id);

    wdf::SchematicComponent* findPart(const std::string& id) noexcept;
    const wdf::SchematicComponent* findPart(const std::string& id) const noexcept;

    /// Index of a part in `getParts()`, or -1.
    int indexOfPart(const std::string& id) const noexcept;

    /// The parts, in creation order.
    std::vector<wdf::SchematicComponent>& getParts() noexcept { return parts_; }
    const std::vector<wdf::SchematicComponent>& getParts() const noexcept { return parts_; }

    /// Centre pin offsets of a part's type, in the order its pins are listed.
    /// Empty for a type the palette does not know.
    static std::vector<PinOffset> pinOffsets(const std::string& type);

    /// Where pin `pinIndex` of `part` sits, in grid units, with the part's
    /// rotation and mirror applied. This is the one function the symbol, the
    /// wire and the hit test all call, so they cannot disagree about a pin.
    static PinOffset pinPosition(const wdf::SchematicComponent& part, int pinIndex);

    /// The box a part occupies, in grid units: **its symbol's own artwork, plus
    /// its pins**.
    ///
    /// **Ported from Céline's `SymbolPainter::elementBounds`.** This is the box
    /// used for framing and for a *left-to-right* enclosing selection, where
    /// "is the whole symbol inside this box" is the question. It is deliberately
    /// not the click target any more: using a bounding box there made a valve or
    /// transformer selectable from its empty corners, and the transformer from
    /// the gap between its windings. Clicks and crossing selections use the drawn
    /// paths instead (`hitTestPart`, `partTouchesBox`).
    ///
    /// The artwork is transformed the way the drawing transforms it -- mirror,
    /// then rotation -- so the box and the symbol cannot disagree, and the pins
    /// are included so a part is grabbable by its terminals even when its
    /// artwork is missing.
    static juce::Rectangle<float> partBounds(const wdf::SchematicComponent& part);

    /// Whether a click on this grid point lands on the part.
    ///
    /// **Tests the drawn symbol, not `partBounds`.** Closed paths are picked
    /// throughout their interior (a resistor's body, a valve's envelope); open
    /// paths and filled shapes are picked on their ink with `reach` grid units of
    /// pointer tolerance. Pin dots count as visible ink. A type with no artwork
    /// falls back to its box, since there is no shape to follow.
    bool hitTestPart(const wdf::SchematicComponent& part, float px, float py,
                     float reach = 0.22f) const;

    /// The part whose symbol covers this grid point, or "". Searched newest
    /// first, so a part placed on top of another is the one you grab.
    std::string partAt(float px, float py, float reach = 0.22f) const;

    /// Whether any drawn ink of the part meets `box`, in grid units. For a
    /// right-to-left crossing selection: the box does not have to enclose the
    /// symbol, it only has to touch something visible.
    bool partTouchesBox(const wdf::SchematicComponent& part, juce::Rectangle<float> box) const;

    /// The pin nearest to `(px, py)` within `reach` grid squares, as
    /// (partId, pinIndex), or ("", -1). Used to start and finish wires.
    std::pair<std::string, int> pinNear(int px, int py, int reach) const;

    // ------------------------------------------------------------------
    // Wires
    // ------------------------------------------------------------------

    /// Adds a wire, splitting a diagonal into an L of two segments (a diagonal
    /// segment would conduct nothing -- `contains()` has no answer for one).
    /// Zero-length drags add nothing. Returns the ids added.
    std::vector<int> addWire(int ax, int ay, int bx, int by);

    const Wire* findWire(int id) const noexcept;

    /// The wire within `reach` grid squares of the point, or -1.
    ///
    /// **The distance to the wire's *line*, not to its bounding box** (Céline's
    /// `wireAt`): a wire is picked by how close the pointer is to it, whichever
    /// way it runs. Ours compared the point with the wire's axis-aligned band,
    /// so a wire that ran diagonally could not be picked at all, and a wire
    /// that ran behind a part was picked by the part's box first.
    ///
    /// Where two wires are equally close, the later one wins -- the one drawn
    /// on top, which is the one that was being aimed at.
    int wireNear(float px, float py, float reach) const;

    /// Removes it. True if anything went.
    bool removeWire(int id);

    /// Moves both ends by the same offset.
    void moveWire(int id, int dx, int dy);

    /// Drags one end along the wire's own axis, leaving the other where it is.
    /// `end` is 0 for `a`, 1 for `b`. Refuses to land exactly on the other end,
    /// which would make a wire that is invisible and unclickable. True if moved.
    bool resizeWireEnd(int id, int end, int px, int py);

    /// Removes any wire passing through this grid point. Returns how many went.
    int removeWiresAt(int px, int py);

    const std::vector<Wire>& getWires() const noexcept { return wires_; }

    /// Points where three or more wire ends meet, or a wire ends partway along
    /// another, so the renderer can draw a junction dot.
    std::vector<std::pair<int, int>> findJunctions() const;

    /// Joins wires that lie on the same line and touch, so two segments drawn
    /// end to end become the one wire they look like. `preferredId` keeps that
    /// wire's identity when it is one of a merged pair, so the wire being
    /// dragged is still selected when the drag ends. Returns how many went.
    int mergeCollinearWires(int preferredId = -1);

    // ------------------------------------------------------------------
    // Connectivity
    // ------------------------------------------------------------------

    /// Work out the nets. O(points x wires), which at schematic sizes is
    /// nothing, and avoids any need for the drawing to maintain connectivity as
    /// it is edited.
    NetList extractNets() const;

    /// What each scope probe would read -- the node on each of its two pins.
    ProbeReadout probeReadout(const std::string& partId) const;

    // ------------------------------------------------------------------
    // Whole-sheet operations
    // ------------------------------------------------------------------

    void clear();
    bool isEmpty() const noexcept { return parts_.empty() && wires_.empty(); }

    /// Every wire end, as a grid point, for the net extractor and the view.
    std::vector<std::pair<int, int>> wireEnds() const;

    /// The grid rectangle everything drawn fits inside, or an empty one.
    juce::Rectangle<float> contentBounds() const noexcept;

    // ------------------------------------------------------------------
    // Persistence
    // ------------------------------------------------------------------

    /// Build from a `.livewdf` document. Part positions come from each
    /// component's `visual`; wires are reconstructed from the pins they join
    /// (see the definition -- the format stores routed polylines, the editor
    /// stores segments, and the pins are the ground truth both agree on).
    void fromDocument(const wdf::LiveWDFDocument& doc);

    /// Write back to a `.livewdf` document: components keep their ids, pins,
    /// values and properties, `visual` carries the placement, and `wires` is the
    /// routed geometry rebuilt from the segments.
    wdf::LiveWDFDocument toDocument() const;

    const std::vector<std::string>& potOrder() const { return potOrder_; }
    void setPotOrder(std::vector<std::string> order) { potOrder_ = std::move(order); }

    const std::vector<std::string>& switchOrder() const { return switchOrder_; }
    void setSwitchOrder(std::vector<std::string> order) { switchOrder_ = std::move(order); }

private:
    int nextPartId_ = 1;
    int nextWireId_ = 1;

    std::vector<wdf::SchematicComponent> parts_;
    std::vector<Wire> wires_;
    std::string name_ = "Untitled Circuit";
    std::string category_ = "Custom";
    std::string author_ = "LiveWDF User";
    std::string description_ = "";
    bool allowPreviewInputPad_ = false;
    double previewPadDb_ = 0.;
    std::string inputNode_ = "in";
    std::string outputNode_ = "out";
    std::string groundNode_ = "0";
    std::vector<std::string> potOrder_;
    std::vector<std::string> switchOrder_;

    friend class SchematicEditor;
};

}  // namespace livewdf::editor
