#pragma once

// Typed questions about a placed part, over the `.livewdf` record.
//
// **Why this file exists.** A `wdf::SchematicComponent` stores its pins, its
// numbers and its settings as three string-keyed maps (`pins`, `values`,
// `properties`). That is the *file format*, and it is the right shape for a
// format -- a key it does not recognise survives a load/save untouched. But it
// is the wrong shape for every *reader*: the canvas, the inspector and the
// symbol painter all end up asking "what is this part's resistance?" by first
// guessing the key ("value"? "totalR"? "maxR"? "initialOhms"?) and then reaching
// into a map. That guess is written down in four places and one of them is
// always the one that forgot a part type.
//
// So the format stays string-keyed; every read goes through here instead. The
// shape is deliberately Celine's: `ElementInfo`'s per-type table plus free
// functions over a placed `Element` (`Celine/source/Schematic/Element.h` --
// `getPinCount`, `hasNumericValue`, `hasModelChoice`, `getTurnsRatio`, ...). The
// difference is that Celine's element is a typed struct, so its functions read
// fields; ours reads maps, through the palette's `PartSpec` for the per-type
// facts. The *contract* is the same, which is the point: a caller asks the same
// question and does not care which side of the format it lands on.
//
// This is **not** a second model. It owns no state and is header-only-in-spirit
// (two tiny tables live in the `.cpp`); it is a reading of `PartLibrary` +
// `SchematicComponent` that the sources share.
//
// It is JUCE-free, like `PartLibrary`, so the model tests and any future
// non-GUI reader can use it.

#include "format/PartLibrary.h"
#include "format/LiveWDFFile.h"

#include <string>
#include <vector>

namespace livewdf::editor {

/// The one number a part carries, and the key it lives under.
///
/// A part has exactly one such number in practice -- a resistor's ohms, a pot's
/// total resistance -- but the *key* differs by type (`value`, `totalR`,
/// `maxR`, `initialOhms`, ...), which is the whole reason a caller must not
/// hard-code it. Returns `primaryValueKey`'s entry, or 0 if absent.
double valueOf(const wdf::SchematicComponent& part);

/// The key `valueOf` reads for this part's type (`"value"` for an unknown type).
const char* primaryValueKey(const std::string& type);

/// True for a part whose number the user types -- every palette part except the
/// pure terminals that stamp nothing. (Celine: `hasNumericValue`.) Modelled on
/// the palette's `defaultValue`/`valueKey`, so a part added to one is seen here.
bool hasNumericValue(const wdf::SchematicComponent& part);

/// The part's model name, e.g. "12AX7", or "" when it has no model field.
std::string modelOf(const wdf::SchematicComponent& part);

/// True for a part picked from a model list rather than by a typed value --
/// diode, triode, pentode, JFET, BJT, op-amp. (Celine: `hasModelChoice`.) These
/// read better on the sheet as their model name than as a number, which is what
/// `valueText` wants.
bool hasModelChoice(const wdf::SchematicComponent& part);

/// The part's pins, in the palette's order -- the order the net extractor walks,
/// so a caller that draws a pin's dot and a caller that reads its net agree.
/// Empty for an unknown type.
const std::vector<std::string>& pinNames(const std::string& type);

/// How many pins the part type has.
int pinCount(const std::string& type);

/// The net name this part's `pinName` sits on, or "" if the pin is absent.
/// (A pin missing from the map is not connected, which is not the same as
/// connected to ground -- so this does *not* substitute ground.)
std::string pinNet(const wdf::SchematicComponent& part, const std::string& pinName);

/// The net name of pin `index`, in `pinNames` order.
std::string pinNetAt(const wdf::SchematicComponent& part, int index);

/// True if `pinName` is one of this part type's pins. What a loader asks before
/// trusting a pin the file named.
bool hasPin(const std::string& type, const std::string& pinName);

/// Human label for the part ("Resistor"), falling back to the raw type.
std::string displayLabel(const wdf::SchematicComponent& part);

/// The label shown next to a placed part: its `properties["label"]` when the
/// user set one, else its `id`.
std::string partLabelText(const wdf::SchematicComponent& part);

/// The value as it should read on the sheet: the model name for a
/// model-bearing part, else the number compressed ("10k", "1.0M"). Celine's
/// `valueToLabel`, adapted: our parts have no unit column yet, so no suffix.
std::string valueText(const wdf::SchematicComponent& part);

/// The name a value key should be *shown* under, rather than the key itself.
///
/// The keys are the file format's words — `fullScaleVolts`, `dc`, `pos` — and
/// the file format is not a user interface. This maps the ones whose key is not
/// already a good label; anything else is shown as itself.
std::string valueKeyLabel(const std::string& key);

}  // namespace livewdf::editor
