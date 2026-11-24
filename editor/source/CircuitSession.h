#pragma once

// Owns the live engine for the document being edited.
//
// Why this is its own class: the editor has three things that all need "the
// current circuit, solved" -- the DC table, the probe badges, and the scope --
// and none of them should own an engine or know how one is built. This is the
// one place that compiles a document, solves it, and steps it, so the three
// readers stay dumb and the engine-lifetime rules live in a single spot.
//
// It is deliberately *not* threaded the way the plugin's processor is: the
// editor is not real-time, so the audio for the scope is generated on demand by
// stepping a test signal, rather than being pushed in by a host. That difference
// is the reason the editor gets its own copy instead of sharing the plugin's
// processor.

#include "format/AcSource.h"
#include "format/LiveWDFFile.h"
#include "dsp/HalfBandOversampler.h"
#include "wdf/Engine.h"

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace livewdf::editor {

/// A compiled circuit, ready to solve and step.
///
/// Rebuild it whenever the topology changes (`compile`) and re-drive it whenever
/// only values changed (`refresh`). A failed compile leaves the session empty and
/// `lastError()` explains why, so a half-drawn circuit is a message, not a crash
/// or a silent zero.
class CircuitSession {
public:
    CircuitSession() = default;

    /// Compile `doc` at `fs`. Returns false and sets `lastError()` on failure.
    /// A successful compile also solves DC once, so the tables are populated
    /// immediately after an edit rather than on the next audio callback.
    bool compile(const wdf::LiveWDFDocument& doc, double fs);

    struct OutputCalibration {
        double voltsPerFullScale = 0.;
        std::string error;
    };
    static OutputCalibration measureOutputReference(const wdf::LiveWDFDocument& doc,
                                                    const std::atomic_bool& cancel);

    /// Drop the engine (no circuit / failed circuit).
    void clear();

    bool valid() const noexcept { return engine_ != nullptr; }
    const std::string& lastError() const noexcept { return error_; }

    /// The part the last failure was about, or "" -- so the editor can take
    /// the user to it rather than leaving them to find it.
    ///
    /// **Why this is a part and not a sentence.** "The circuit cannot be
    /// solved" tells the user nothing they can act on; "the capacitor C1 is
    /// not connected to anything" tells them where to look. The part is what
    /// the editor can select, and selecting it is what takes the user there.
    const std::string& lastErrorPartId() const noexcept { return errorPartId_; }

    /// The pin the last failure was about, or "". With the part, it names the
    /// terminal: "C1.b".
    const std::string& lastErrorPin() const noexcept { return errorPin_; }

    /// The parts on the sheet that are joined to nothing, as (partId, pin)
    /// pairs. A warning, not an error: a part can be left open on purpose (a
    /// test point), and the engine may well accept the circuit.
    ///
    /// **It is the same finding the refusal is explained by**, so the two
    /// cannot disagree: a build that fails says which of these it was.
    const std::vector<std::pair<std::string, std::string>>& unconnectedParts() const noexcept {
        return unconnected_;
    }

    /// Node voltages from the most recent DC solve, keyed by node name.
    const std::map<std::string, double>& dcVoltages() const noexcept { return dcVolts_; }

    /// Per-device currents from the most recent DC solve, keyed by a readable
    /// device label (e.g. "V1 plate"). Empty when there is nothing to report.
    const std::map<std::string, double>& dcCurrents() const noexcept { return dcAmps_; }

    /// The DC solve status line: how many Newton iterations it took, or why it
    /// did not converge. Empty when there is no engine.
    const std::string& dcStatus() const noexcept { return dcStatus_; }
    bool dcConverged() const noexcept { return dcConverged_; }

    /// The output node's index into `step()`'s vector, or -1 when the document's
    /// `outputNode` is not a node of the compiled circuit.
    int outputIndex() const noexcept { return outIdx_; }

    /// The scope probe's node index, or -1 when the sheet has no probe.
    ///
    /// The probe is the editor's "look here": it stamps nothing into the
    /// circuit, and the scope traces *this* node instead of the output when the
    /// sheet has one. That is what makes it a place to watch rather than a
    /// badge.
    int probeIndex() const noexcept { return probeIdx_; }

    /// The node the circuit's signal comes in on, or -1.
    ///
    /// The Input terminal's net, or -- when the sheet has no Input terminal --
    /// the first source's positive pin. The editor's scope shows this as the
    /// *input* trace: what is actually going into the circuit, rather than what
    /// the editor wished were going in.
    int inputNodeIndex() const noexcept { return inputNodeIdx_; }

    /// **What is going into the circuit**, in volts.
    ///
    /// The value on the input source: the editor's test signal when the editor
    /// is driving one, and the source's own value when it is not -- a DC supply,
    /// an AC generator.
    ///
    /// It is a *value* rather than a node's voltage on purpose: a net held by a
    /// voltage source is not a free node, so the engine has no voltage to read
    /// for it. The source's value is the voltage there.
    double inputVolts() const noexcept { return lastInputValue_; }

    /// What to call the probe in the scope's label: its id, so the user can tell
    /// *which* probe they are looking at when there is more than one. Empty when
    /// the sheet has no probe.
    const std::string& probeLabel() const noexcept { return probeLabel_; }

    /// **Every** scope probe on the sheet, as (id, node index).
    ///
    /// The scope traces all of them at once, each on its own colour: a sheet
    /// with three meters shows three signals, not one. A probe whose net is
    /// not a node of the compiled circuit is left out -- there is no signal to
    /// trace there.
    const std::vector<std::pair<std::string, int>>& probeTraces() const noexcept {
        return probeTraces_;
    }

    /// Watch a particular scope probe, by its id. "" puts the scope back on the
    /// first probe on the sheet.
    ///
    /// **This is how "click the probe you want to watch" works.** The probe
    /// stamps nothing into the circuit; it only names a net, and the scope
    /// traces whichever probe is named here. The editor sets it when the
    /// selection changes.
    void watchProbe(const std::string& componentId, const std::string& net);

    /// The audio-input source's index into the source-value vector, or -1 when
    /// the document has no source to drive.
    int inputSourceIndex() const noexcept { return inSrcIdx_; }

    /// The name of the audio input source, for the scope's input trace. Empty
    /// when there is none.
    const std::string& inputSourceName() const noexcept { return inSrcName_; }

    /// One sample of the circuit driven by `inputVolts` on the input source.
    /// Returns the vector of node voltages (indexed like `nodeNames()`). This is
    /// how the editor makes scope traces without a host: the window feeds a test
    /// signal in and reads the output out.
    const std::vector<double>& step(double inputVolts);

    /// The node names, indexed exactly like `step()`'s return. Empty when there
    /// is no engine.
    const std::vector<std::string>& nodeNames() const noexcept { return nodeNames_; }

    /// Oversampling setting for real-time auditioning / stepping.
    void setOversampling(wdf::OversamplingFactor factor);
    wdf::OversamplingFactor oversampling() const noexcept { return osFactor_; }

    /// Reset reactive state to the DC operating point, so a fresh test signal
    /// starts from a settled circuit.
    void reset();

    /// The sample counter the AC sources' phase is counted from. It restarts at
    /// zero on `reset`, so a fresh test signal starts at a zero crossing rather
    /// than halfway through a cycle.
    long long sampleIndex() const noexcept { return sampleIndex_; }

private:
    void solveDcNow();
    void refreshNodeNames();

    /// Point the scope at a probe, by id, and work out its node. Called by
    /// `watchProbe` and by `compile`.
    void resolveWatchedProbe(const wdf::LiveWDFDocument& doc);

    std::unique_ptr<wdf::Engine> engine_;
    wdf::Circuit circuit_;
    std::vector<std::string> nodeNames_;
    std::vector<double> baseSources_;

    /// The sources' AC settings -- amplitude and frequency -- in the same order
    /// as `baseSources_` for the voltage sources and after them for the current
    /// ones. Read from the document, because the frequency is the signal's and
    /// not the network's (see `format/AcSource.h`).
    std::vector<wdf::LiveWDFDocument::SourceAc> acSettings_;

    std::vector<double> scratch_;

    /// How many samples have been stepped since the last reset. The AC sources'
    /// phase is a function of it.
    long long sampleIndex_ = 0;

    std::map<std::string, double> dcVolts_;
    std::map<std::string, double> dcAmps_;
    std::string dcStatus_;
    bool dcConverged_ = false;

    int outIdx_ = -1;
    int inSrcIdx_ = -1;
    std::string inSrcName_;

    /// The scope probe's node, or -1. Watched by the editor's scope.
    int probeIdx_ = -1;

    /// **Every** scope probe on the sheet, as (id, node index).
    std::vector<std::pair<std::string, int>> probeTraces_;

    /// The node the circuit's signal comes in on, or -1.
    int inputNodeIdx_ = -1;

    /// Which source's value is "what is going into the circuit". The Input
    /// terminal's, or the first source when the sheet has no terminal.
    int inputSrcIdx_ = -1;

    /// The value that was on the input source at the last step.
    double lastInputValue_ = 0.0;

    /// The probe's name for the scope's label.
    std::string probeLabel_;

    /// The id of the probe the scope is watching. Empty means "the first probe
    /// on the sheet", which is what a sheet with one probe wants.
    std::string watchedProbeId_;

    /// The net that probe watches.
    std::string watchedProbeNet_;

    std::string error_;
    std::string errorPartId_;
    std::string errorPin_;

    /// The parts joined to nothing, as (partId, pin) pairs.
    std::vector<std::pair<std::string, std::string>> unconnected_;

    double fs_ = 48000.0;
    wdf::OversamplingFactor osFactor_ = wdf::OversamplingFactor::X1;
    wdf::HalfBandOversampler oversampler_{wdf::OversamplingFactor::X1};
    std::vector<double> lastStepNodes_;
    const std::vector<double>& stepInternal(double inputVolts);
};

}  // namespace livewdf::editor
