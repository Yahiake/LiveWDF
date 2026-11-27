#pragma once

// The palette of parts the editor can place, and the part facts every reader
// needs: pin names, the pin frame in grid units, and the default value.
//
// Why this is its own file: the *canvas* needs to know how many pins a part has
// (to draw them and to start a wire from one), and the *palette panel* needs to
// know what to offer and what to call it. If those two kept their own lists they
// would drift, and a part the palette offers that the canvas cannot pin out is a
// crash waiting to happen. One table, both readers.
//
// **Why it lives in `src/format/` rather than under `editor/`:** the pin frame
// is a fact about a *part*, not about the window that draws it. The document
// reader needs exactly the same answer -- where a part's terminals sit given
// its position, mirror and turns -- because that is what decides which pins
// share a net. A second table for the reader is the bug this project keeps
// being told off for: two accounts of one fact, which drift the first time one
// of them is edited. So there is one table here, and the editor reaches it
// through the `livewdf` library it links.
//
// The namespace stays `livewdf::editor` even though the file is in the core
// library. It is the palette's and the inspector's vocabulary -- `PartSpec`,
// `ChoiceKind`, a category tree, keyboard shortcuts -- and renaming it would
// touch four dozen call sites for no gain. `format/SchematicNets.cpp` says once
// that it asks the palette a question rather than owning it.
//
// A part is described purely in terms of the `.livewdf` document model -- pin
// names and default values -- so placing one is "append a SchematicComponent",
// with no editor-specific baggage.
//
// It is deliberately JUCE-free: the table is data, and the model tests and any
// non-GUI reader must be able to include it without the widget layer. The
// palette *panel* that displays it lives in `PartPalettePanel.h`.

#include "format/LiveWDFFile.h"

#include <string>
#include <vector>

namespace livewdf::editor {

/// A pin's position relative to a part's centre, in grid units. Its own small
/// struct rather than reusing the model's `PinOffset` because the palette is
/// deliberately JUCE-free and model-free -- see the header note -- and because a
/// table of these is what gets written out for a part whose frame is stated
/// rather than derived.
struct PinOffsetSpec {
    int x = 0, y = 0;
};

/// One placeable part type.
struct PartSpec {
    std::string type;        ///< `SchematicComponent::type`, e.g. "res"
    std::string label;       ///< what the palette shows, e.g. "Resistor"
    std::string idPrefix;    ///< default id stem, e.g. "R" -> R1, R2, ...
    std::vector<std::string> pins;  ///< pin names, in the order `makeComponent` fills them
    /// Per-pin canvas hint: which side of the body the pin sits on, index-parallel
    /// to `pins`. Keeps the drawing honest without the canvas re-deriving it.
    /// 0 = left, 1 = right, 2 = up, 3 = down (before rotation).
    std::vector<int> pinSides;
    /// **The pin frame, in grid units, when it is worth writing down.**
    ///
    /// Empty for most parts, and then `Schematic::pinOffsets` derives the offsets
    /// from `pinSides` (a pin on the left goes to `-halfWidth`, and pins sharing
    /// a side spread along it). Non-empty when Céline states the offsets
    /// explicitly -- which it does for every part it draws, and which is why its
    /// symbol geometry is a table rather than a rule.
    ///
    /// Why both exist rather than one replacing the other: the derived form is
    /// right for our own parts (`vcvs`, `scope`) that Céline has no counterpart
    /// for, and the explicit form is the only way to reproduce Céline's valves --
    /// a triode's plate, grid and cathode are not on a symmetric spread, and
    /// rounding them onto one is what made our valve artwork unusable. Written
    /// per pin, index-parallel to `pins`; empty means "derive it".
    ///
    /// **These are Céline's numbers, copied from its `Element.h` table**, and the
    /// parity test in `tests/editor/test_symbol_parity.cpp` holds us to them.
    std::vector<PinOffsetSpec> pinOffsets;
    double defaultValue = 0.0;      ///< the `values[valueKey]`
    std::string valueKey = "value"; ///< which key `defaultValue` belongs to
    /// The keyboard shortcut that arms this part for placing, or "" for none.
    ///
    /// **Why this is its own field rather than `idPrefix`'s first letter:** the
    /// two want different things. An `idPrefix` is the label a symbol wears
    /// ("POT", "XP", "SC"); a *shortcut* must be unique across the whole palette
    /// and must not collide with the tool letters (S/W/X/F).
    ///
    /// **Why a string and not a letter:** some shortcuts need a modifier. The
    /// Gain block is "Ctrl+G" (Cmd+G on a Mac), because the bare letters are
    /// spoken for -- the tools take S, W and X, and the verbs take F -- and
    /// "G" itself is wanted by the ground symbol.
    ///
    /// The name is the *canonical* one: "Ctrl" means "the command modifier",
    /// which is Cmd on a Mac. The editor renders it per platform; the model does
    /// not know what platform it is on.
    std::string key;

    /// How a freshly placed part is turned, before the user touches it.
    ///
    /// Most parts start upright. The pot is the exception: its `low` terminal
    /// is the top one when placed, so the drawing reads `L` above `H` without
    /// the user having to rotate and flip it first. This is a placement
    /// transform, not a second pin frame -- `pinOffsets` stays Céline's, with
    /// `high` at (0,-2), so the parity test still holds.
    int defaultRotation = 0;
    bool defaultFlipped = false;

    /// Logical category for the palette hierarchy (e.g. "Passive/Resistors").
    /// Empty means uncategorised; the panel will place it at the top level.
    std::string category;

    /// **The device variant this row places.**
    ///
    /// A model-bearing part is listed once per model in the registry, so the
    /// palette shows `12AX7`, `12AT7`, ... rather than one anonymous "Triode".
    /// `modelProperty` is the document property that names the model (`"model"`
    /// for most devices, `"preset"` for a MOSFET); `model` is the value to
    /// write. Both empty for a part with no variants.
    std::string modelProperty;
    std::string model;

    /// What the palette row should say: the model for a variant, the family
    /// label otherwise. `label` itself stays the family name, because other
    /// callers (inspector titles, error sentences) want "Triode", not "12AX7".
    std::string paletteLabel() const { return model.empty() ? label : model; }
};

/// Category node for the hierarchical palette. This is JUCE-free data.
struct CategorySpec {
    std::string name;              ///< Display name (e.g. "Resistors")
    std::string path;              ///< Full path for nesting (e.g. "Passive/Resistors")
    /// The palette entries directly in this category, in palette order.
    ///
    /// **Pointers into the static palette, not type strings.** A model-bearing
    /// part has many palette entries that share one `type`; keying the tree by
    /// type would collapse them all to the first model. The palette vector is
    /// built once and never resized, so these pointers stay valid.
    std::vector<const PartSpec*> parts;
    std::vector<CategorySpec> children; ///< Nested sub-categories
    bool defaultExpanded = true;   ///< Whether this category starts expanded
};

/// The whole palette, in the order the panel lists it.
const std::vector<PartSpec>& partPalette();

/// The hierarchical category tree for the new palette view.
const std::vector<CategorySpec>& partCategories();

/// Look a part up by its `type`; returns nullptr if unknown.
const PartSpec* findPart(const std::string& type);

/// Human label for a component already in a document (falls back to the type).
std::string partLabel(const std::string& type);

/// A stable, unique id for `type` given the ids already in use.
std::string uniqueId(const std::string& type,
                     const std::vector<wdf::SchematicComponent>& existing);

/// Build a fresh component of `type`, with default pins/values and an id that
/// does not collide with any id in `existing`. `x`/`y` are canvas coordinates.
///
/// `modelProperty`/`model` override the model a model-bearing part starts with;
/// empty means "the palette's default variant" (the first entry for the type).
///
/// Returns a component whose `type` is empty when `type` is not in the palette,
/// so the caller can test that rather than guess.
wdf::SchematicComponent makeComponent(const std::string& type,
                                      const std::vector<wdf::SchematicComponent>& existing,
                                      float x, float y,
                                      const std::string& modelProperty = {},
                                      const std::string& model = {});

/// Net name for a fresh pin of `compId` when it is not yet wired to anything.
/// Unconnected pins get their own net so a half-drawn part is still a valid
/// document -- it just does not yet join anything.
///
/// Editor bookkeeping only. A `.livewdf` file never carries a net name, and the
/// one a pin ends up with is decided by the drawing -- see
/// `format/SchematicNets.h`.
std::string danglingNet(const std::string& compId, const std::string& pin);

/// Rotate a pin's canvas side by `quarterTurns` (0..3), the same way
/// `VisualPlacement::rotation` rotates the body.
int rotateSide(int side, int quarterTurns);

/// Where a part's pins sit, in grid units from its centre, before the part's own
/// mirror and turns are applied.
///
/// The stated frame wins when the part states one -- every part Celine draws
/// states its own, and that is the only way a valve's plate, grid and cathode
/// can be asymmetric. The side-and-spread rule is the fallback for our own
/// parts (`vcvs`, `scope`) that Celine has no counterpart for.
///
/// Index-parallel to `PartSpec::pins`, so `pinFrame(type)[i]` is pin `i`. It is
/// a reference into a table built once: the frame of a type does not depend on
/// where the part is placed, and nothing here may recompute it per call.
const std::vector<PinOffsetSpec>& pinFrame(const std::string& type);

const std::vector<std::string>& partPins(const wdf::SchematicComponent& part);
const std::vector<PinOffsetSpec>& pinFrame(const wdf::SchematicComponent& part);

/// The canvas point pin `pinIndex` of `part` is drawn at: the frame above,
/// mirrored in the part's own frame first and then turned, which is Celine's
/// order and the one that makes "flip" mean the same thing whichever way the
/// part is turned.
///
/// This is the answer the net extractor needs: two pins are on the same net
/// exactly when this says they are at the same point.
PinOffsetSpec pinAt(const wdf::SchematicComponent& part, int pinIndex);

}  // namespace livewdf::editor
