#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "gui/VintageKnob.h"
#include "gui/DualOscilloscope.h"
#include "PresetLibraryPanel.h"
#include "SchematicEditor.h"
#include "UiLookAndFeel.h"

#include <vector>
#include <memory>

namespace livewdf {

class LiveWDFPluginEditor : public juce::AudioProcessorEditor,
                            public juce::Timer,
                            public juce::FileDragAndDropTarget {
public:
    explicit LiveWDFPluginEditor(LiveWDFPluginProcessor& p);
    ~LiveWDFPluginEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override;

    // File Drag and Drop
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

private:
    LiveWDFPluginProcessor& processor_;

    // Top Toolbar
    juce::Label titleLabel_;
    juce::TextButton factoryBtn_ { "FACTORY" };
    juce::TextButton loadFileBtn_ { "Load .livewdf..." };

    /// Re-read the file the circuit came from.
    ///
    /// **The "designing while listening" button.** The editor is where circuits
    /// are drawn and tested; the plugin is where one is played. When both are
    /// open on the same file the plugin is holding a *copy* from the moment it
    /// loaded, so an edit in the editor is invisible until the plugin is told
    /// to look again. This is that.
    ///
    /// Disabled when there is no file to reload -- a circuit restored from host
    /// state has none -- and it says so in its tooltip rather than being
    /// silently dead.
    juce::TextButton reloadBtn_ { "Reload" };
    juce::TextButton aboutBtn_ { "ABOUT" };

    juce::ComboBox oversamplingCombo_;

    /// Mono is the default. One engine instead of two is the single largest
    /// saving on the plugin path, and a mono amp chain does not need a second
    /// copy. The button is an APVTS parameter so the choice survives a session
    /// and can be automated by the host.
    juce::TextButton stereoBtn_ { "MONO" };

    /// What the plugin is playing, and what happened last: the file's name, or
    /// "Reloaded", or "Reload failed".
    ///
    /// It is the difference between "the plugin is playing something" and "the
    /// plugin is playing *the thing I just saved*", which is the whole question
    /// when the editor and the plugin are both open.
    juce::Label statusLabel_;
    editor::FactoryPresetStepper presetStepper_{*this, statusLabel_};

    // Central schematic. This is the editor's own canvas, embedded read-only:
    // the plugin plays a circuit, the editor draws it, and both use the same
    // renderer so a file looks identical in either window.
    editor::SchematicEditor schematicCanvas_;

    // Bottom Dual-Trace Oscilloscope
    DualOscilloscope oscilloscope_;

    // Knobs
    VintageKnob inTrimKnob_ { "TRIM", " dB" };
    VintageKnob outMasterKnob_ { "OUTPUT", " dB" };
    VintageKnob mixKnob_ { "MIX", " %" };

    // Dynamic circuit controls: one knob per pot/rheostat group and one button
    // per SPDT group. Both are grouped by label, so a ganged pair takes one
    // place in the strip rather than two. The pot ceiling is the processor's,
    // since it owns the `pot_*` parameters; asking for more than it has is a
    // document the strip cannot drive, not a second buffer to grow.
    static constexpr int kMaxDynamicPots = LiveWDFPluginProcessor::kMaxPotKnobs;
    static constexpr int kMaxDynamicSwitches = 8;
    // Below this width a knob's label stops being readable, so once a row
    // cannot hold every pot the strip wraps onto another row rather than
    // squeezing them (an amp with a graphical equalizer has twelve groups).
    static constexpr int kMinDynamicPotKnobWidth = 64;
    std::vector<std::unique_ptr<VintageKnob>> dynamicPotKnobs_;
    std::vector<std::unique_ptr<juce::TextButton>> dynamicSwitchButtons_;
    std::vector<juce::String> dynamicSwitchNames_;

    // APVTS Attachments
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    std::unique_ptr<SliderAttachment> inTrimAttach_;
    std::unique_ptr<SliderAttachment> outMasterAttach_;
    std::unique_ptr<SliderAttachment> mixAttach_;
    std::unique_ptr<ComboBoxAttachment> oversamplingAttach_;
    std::unique_ptr<ButtonAttachment> stereoAttach_;
    std::vector<std::unique_ptr<SliderAttachment>> dynamicPotAttachments_;

    std::unique_ptr<juce::FileChooser> fileChooser_;
    juce::Component::SafePointer<juce::CallOutBox> factoryBrowser_;
    editor::PresetLibraryPanel::View factoryView_;
    bool isDraggingFile_ = false;

    void rebuildDynamicControls();
    void updateSwitchButton(int index, juce::TextButton& button);
    void openCircuitFileChooser();
    void showFactoryBrowser();

    /// Load a file and update every part of the editor that depends on it.
    /// Returns false and puts the reason in the status line when the document
    /// is rejected.
    bool loadCircuitFile(const juce::File& file);

    /// Hand the shared canvas the live knob positions and switch throws.
    void syncSchematicLiveState();

    /// What the plugin is playing: the file's name, the restored document's
    /// name, or "No circuit loaded".
    juce::String currentCircuitDescription() const;

    editor::UiLookAndFeel lookAndFeel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LiveWDFPluginEditor)
};

} // namespace livewdf
