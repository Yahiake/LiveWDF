#pragma once

// A group of parts and wires, ready to be pasted somewhere.
//
// **What this is.** The clipboard's payload: the selected parts, their values
// and their settings, and the wires among them. It is *not* a document -- a
// pasted group is not a circuit, it is a piece of one, and it is pasted into
// whatever circuit the user is in.
//
// **The nets are deliberately not carried.** A pasted part gets **fresh** nets,
// the way a newly placed part does. Two reasons:
//
//   * a pasted group must not join the circuit it is pasted into by accident,
//     through a net name that happens to be the same on both sides; and
//   * the group's own wiring is preserved by its *geometry* -- the wires are
//     pasted with it, and the net extractor joins the pins by position, so the
//     group arrives as it left.
//
// A group pasted on top of an existing circuit does connect to it, where the
// pins land on the same points. That is the point of pasting a filter into a
// circuit, and it is what every schematic editor does.
//
// **The format is text**, so it can go through the *system* clipboard and
// therefore between two editor processes -- which is the case that matters
// when the answer to "how do I get this sub-circuit from that window" is
// "copy it".

#include <map>
#include <string>
#include <vector>

namespace livewdf::editor {

/// One part in a clipboard group.
struct ClipboardPart {
    std::string type;
    float x = 0.0f;
    float y = 0.0f;
    int rotation = 0;
    bool flipped = false;

    std::map<std::string, double> values;
    std::map<std::string, std::string> properties;

    /// The part's pin *roles* -- "a", "b", "cathode" -- and not their nets.
    /// The nets are what the paste makes fresh.
    std::vector<std::string> pins;
};

/// A wire in a clipboard group: its two ends, in grid squares.
struct ClipboardWire {
    int ax = 0, ay = 0, bx = 0, by = 0;
};

/// A group of parts and the wires among them.
struct ClipboardGroup {
    std::vector<ClipboardPart> parts;
    std::vector<ClipboardWire> wires;

    bool isEmpty() const noexcept { return parts.empty(); }

    /// The group's own extent, so a paste can put it where the pointer is.
    void bounds(float& minX, float& minY, float& maxX, float& maxY) const;

    /// The text form, for the clipboard.
    std::string toText() const;

    /// The text form, read back. Empty when the text is not ours.
    static ClipboardGroup fromText(const std::string& text);
};

}  // namespace livewdf::editor
