#pragma once

// Edits the values of the selected component.
//
// Why a separate panel: the canvas is about *where* things are; values are about
// *what* they are. Mixing an editable value field into the drawing surface means
// every mouse gesture has to know whether it landed on a widget, which is a
// class of bug this split removes. The canvas reports a selection; this reads
// and writes that one component.

#include "format/LiveWDFFile.h"

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

namespace livewdf::editor {

class InspectorPanel : public juce::Component, private juce::Timer {
public:
    InspectorPanel();
    ~InspectorPanel() override;

    /// Point the inspector at `id` inside `doc` ("" clears it). The document is
    /// held by pointer because the inspector edits in place -- it is a view onto
    /// the canvas's model, not a copy of it.
    void setTarget(wdf::LiveWDFDocument* doc, const std::string& id,
                   const std::vector<std::string>& selectedParts = {});

    /// What the inspector shows when a *wire* is selected, or none.
    ///
    /// Celine keeps parts and wires in two separate selections, and so do we. The
    /// inspector has no fields to edit on a wire -- a wire is two endpoints -- but
    /// saying nothing at all (the old "nothing selected") is wrong: the user
    /// clicked a wire and is owed an answer. This is that answer: what the wire
    /// is and where it runs, read-only.
    struct WireInfo {
        int id = -1;
        int ax = 0, ay = 0;   ///< first end, grid units
        int bx = 0, by = 0;   ///< second end, grid units
        /// The nets the two ends landed on, if the model could resolve them.
        juce::String netA, netB;
        bool valid() const noexcept { return id >= 0; }
    };

    /// Show a wire. Pass a default-constructed `WireInfo` (or call `setTarget`
    /// with "") to clear it. Calling this clears any part target and vice versa,
    /// because only one thing is selected at a time in the ordinary case.
    void setWireTarget(const WireInfo& wire);

    /// Called after the component was changed in place, so the host can
    /// recompile and redraw.
    std::function<void()> onChanged;

    /// How many times the panel has rebuilt.
    ///
    /// It should be a small number: one per selection change. It was in the
    /// thousands, because a picker sets its own opening position and JUCE posts
    /// a change for that, so the panel rebuilt itself in answer to a change the
    /// user had not made -- and the editor spun, building and destroying widgets
    /// as fast as the message loop would go. That is what took the editor down
    /// on a part with a picker.
    ///
    /// The count is here so that can be *checked* rather than believed.
    int rebuildCount() const noexcept { return rebuildCount_; }

    /// Type a number over the selected part on the canvas and commit it with
    /// Enter: routed here because the canvas owns the gesture (the part is the
    /// thing under the eye) and this panel owns the fields. Writes the part's
    /// *primary* value -- the one number the palette says its type carries --
    /// through the same parse the text box uses, so a refused parse leaves the
    /// part untouched exactly as it does in the box. A part whose number is not
    /// one field (model-bearing, or a terminal that stamps nothing) has nothing
    /// to answer, and nothing does.
    void quickValueEdit(const juce::String& text);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void rebuild();
    void startOutputCalibration();
    void cancelOutputCalibration();
    void refreshCalibrationControls();
    void timerCallback() override;
    struct CalibrationJob;
    std::unique_ptr<CalibrationJob> calibration_;
    juce::String calibrationMessage_;
    juce::Component::SafePointer<juce::TextButton> calibrationButton_;
    juce::Component::SafePointer<juce::Label> calibrationLabel_;
    juce::Component::SafePointer<juce::TextEditor> outputReferenceField_;

    /// Write `text` into the targeted component under `key`. Decides value vs
    /// property by which map already holds the key, so the callers stay free of
    /// the ordering bookkeeping `rebuild` does.
    void commitEdit(const std::string& key, const juce::String& text);
    void notifyChanged();

    wdf::LiveWDFDocument* doc_ = nullptr;
    std::string id_;
    std::vector<std::string> selectedParts_;
    WireInfo wire_;

    /// The rows live inside a viewport rather than directly in the panel.
    ///
    /// A JFET carries eight model parameters plus the model picker; an SPDT has
    /// its own controls; the inspector column is only as tall as the window's
    /// right-hand pane. Laying every row at `top + i * 44` meant the rows past
    /// the bottom were not merely off-screen -- they had no way of ever coming
    /// on-screen, because there was no scrolling. The model picker, built last,
    /// was the first thing lost.
    ///
    /// The header and Delete button stay outside the viewport: the inspector's
    /// title should not scroll away, and Delete must remain reachable no matter
    /// how long the parameter list is.
    juce::Viewport viewport_;
    juce::Component content_;

    juce::OwnedArray<juce::Label> labels_;

    /// The control for each row: a text box for a number or a free-text
    /// property, a combo box for a property with a fixed list of choices.
    ///
    /// One list rather than two, because the rows are laid out as pairs of
    /// (label, control) and a row whose control was in a different list from
    /// its label would need the two kept in step by hand.
    juce::OwnedArray<juce::Component> editors_;

    /// Write a picker's selected text into the targeted component under `key`.
    /// The text is read *before* the commit, so a host that rebuilds the panel
    /// in response cannot pull the widget out from under the callback.
    void commitPicker(const std::string& key, juce::ComboBox& box);

    /// The type of the part the inspector is showing, or "" when there is none.
    /// For the diagnostic log.
    std::string typeOfTarget() const;

    /// The rebuild counter's storage.
    int rebuildCount_ = 0;

    juce::TextButton removeButton_{"Delete"};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InspectorPanel)
};

}  // namespace livewdf::editor
