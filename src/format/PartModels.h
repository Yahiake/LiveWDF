#pragma once

// The variants a part can be, and the lists they come from.
//
// **Why this file exists.** A diode, a triode, a pentode, a JFET, a MOSFET, a
// BJT or an op-amp is not one part each: the engine carries a *registry* of real
// devices -- six diodes, fourteen triodes, twelve pentodes, the N-channel and
// P-channel JFET cards, the NPN and PNP BJT cards, and the op-amp model cards --
// and a placed part names one of them.
//
// The editor had the registry and no way to show it. The inspector offered a
// free-text box and expected the user to already know that "12AX7" was a thing
// they could type. So the user could not see what was available, and a typo
// became a part with no model at all -- which the converter silently resolved to
// a default, so the circuit silently was not the circuit they drew.
//
// The lists here are the registry, read from the same tables the engine
// compiles from. There is no second copy to drift: a device added to
// `kDiodeTable` appears in the picker with no other edit.
//
// This is JUCE-free, like `PartLibrary` and `ElementAccess`, so the model tests
// can use it.

#include "format/LiveWDFFile.h"

#include <string>
#include <vector>

namespace livewdf::editor {

/// What kind of choice a property offers.
enum class ChoiceKind {
    None,        ///< no fixed list: the property is free text
    DiodeModel,
    TriodeModel,
    PentodeModel,
    JfetModel,
    BjtModel,
    MosfetPreset,
    OpAmpModel,
    Taper,
    SourceMode,  ///< DC or AC -- a source is one or the other, not both
    Polarity,    ///< N-channel or P-channel for a JFET/MOSFET
    TransformerModel, ///< Ideal, Auto, or Manual for a transformer model card
};

/// The choices for a kind, in the order they should be offered.
///
/// Empty for `ChoiceKind::None`.
std::vector<std::string> modelChoices(ChoiceKind kind);

/// The kind of choice `key` is, for a part of `type`.
///
/// `None` means the property is free text -- which is the answer for every
/// property that is not a device name or a taper, and for a part type the
/// editor does not know about.
ChoiceKind choiceKindFor(const std::string& type, const std::string& key);

/// The choices for one property of one part type. Empty when the property is
/// free text.
std::vector<std::string> propertyChoices(const std::string& type, const std::string& key);

/// The text the inspector shows for a stored choice value.
///
/// Most choices are stored exactly as they are shown. Grid conduction is the
/// exception: the file format stores `"true"`/`"false"`, while the picker says
/// `"On"`/`"Off"`. Keeping the translation here means the inspector never has
/// to know which property is special.
std::string choiceDisplayText(const std::string& key, const std::string& stored);

/// The canonical value to store for the text chosen in an inspector picker.
///
/// The inverse of `choiceDisplayText`; identity for every key except grid
/// conduction.
std::string choiceStorageText(const std::string& key, const std::string& displayed);

/// True when a transformer's `model` picker says the low-level card is the
/// user's -- `Real — Manual`, or the bare `Manual` an older file writes.
///
/// This is the one question both halves of a transformer ask: the inspector asks
/// it before offering `Lm`/`leakage`/`dcrPrimary`/`dcrSecondary`, and the picker
/// asks it before writing them. Asked once, here, the rows shown and the numbers
/// stored cannot disagree about which card the picker says is in use. False for
/// every part type that is not a transformer, so a caller has no type check of
/// its own to remember.
bool transformerCardIsManual(const wdf::SchematicComponent& part);

/// Make a transformer's stored numbers mean the card its picker says.
///
/// A transformer is the inverse of the other card parts: `Ideal` and `Real —
/// Auto` *derive* the card from the turns ratio, so there is nothing to load --
/// the numbers the user would edit do not exist for those cards, by design.
/// `Real — Manual` is where those numbers become the user's, and they start from
/// the card `Real — Auto` would have derived for the turns ratio that is set, so
/// picking Manual is a starting point rather than a blank page: a transformer
/// placed from the palette can be tuned without hand-typing a file.
///
/// Unlike the readings above, this writes to the record. It belongs here because
/// this file owns the card names the picker offers, and the seed comes from the
/// same `resolveTransformerSpec` the converter runs -- there is no second copy of
/// the derivation to drift. The inspector calls it once, after the picker
/// changes; nothing else may, because it would overwrite a Manual card the user
/// had already tuned.
void applyTransformerCard(wdf::SchematicComponent& part);

/// What a transistor part's model says it is, for the inspector's read-only
/// identity rows.
///
/// **Why this is not in the picker.** A picker entry has to remain the exact
/// model name the converter understands; decorating it with "NPN" would turn
/// the selected text into a name `findBjtByName` does not resolve. This is the
/// separate line that says the polarity plainly, and for BJTs the model-card
/// numbers that change when a different card is chosen.
struct TransistorFacts {
    /// "NPN", "PNP", "N-channel", "P-channel", "Unknown", or "" for a
    /// non-transistor.
    std::string type;
    /// BJT model-card summary (Is, Bf, Vaf); empty for other part types.
    std::string details;
};

/// Read the facts for `part`. For a BJT the card is looked up in the engine's
/// registry; for a JFET/MOSFET the card or preset is authoritative, and a
/// custom part falls back to its explicit `polarity` property. A name the
/// registry does not know still gets an "Unknown" type rather than pretending
/// to be N-type.
TransistorFacts transistorFacts(const wdf::SchematicComponent& part);

}  // namespace livewdf::editor
