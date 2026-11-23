#include "EditorWindow.h"
#include "EditorWindowInternal.h"
#include "DiagLog.h"
#include "EditorLayout.h"
#include "EditorLookAndFeel.h"
#include "gui/Fonts.h"

#include <algorithm>
#include <cmath>

namespace livewdf::editor {
void EditorWindow::Content::rebuildEngine() {
    const bool ok = session_.compile(editor_.getDocument(), 48000.0);
    syncScopeProbes();

    // **The build result belongs in the log.** The status bar says it while the
    // window is open; the log says it after the window has gone, which is what
    // makes "open this file and tell me why it will not build" answerable
    // without a screenshot.
    if (ok) {
        diagLog("build: ok");
        diagLog("dc: " + session_.dcStatus());
    } else {
        diagLog("build: rejected: " + session_.lastError());
    }

    if (!ok) {
        nodeTable_.clear();
        nodeTable_.setStatus(session_.lastError(), true);
        editor_.setNodeVoltages({});

        // **Point at the part.** The message says what is wrong; the ring round
        // the part says where, without taking the selection away.
        editor_.setProblemPart(session_.lastErrorPartId());

        const juce::String error = session_.lastError();
        statusLabel_.setText(error, juce::dontSendNotification);
        statusLabel_.setColour(juce::Label::textColourId, glow::getColourHotRed());

        std::vector<MessageConsole::Message> messages;
        messages.push_back({error, MessageConsole::Severity::Error,
                            juce::String(session_.lastErrorPartId())});
        presentMessages("Build failed", MessageConsole::Severity::Error,
                        std::move(messages));
        return;
    }

    editor_.setProblemPart({});

    refreshOperatingPoint();

    // **The status bar is the summary; the console is the list.** A successful
    // build with four unconnected parts used to grow the status line one clause
    // at a time until it was unreadable. Now the line says how many things need
    // attention and the drawer says which ones.
    std::vector<MessageConsole::Message> messages;

    if (!session_.dcConverged())
        messages.push_back({juce::String(session_.dcStatus()),
                            MessageConsole::Severity::Warning, {}});

    // **A part that is joined to nothing is worth saying, even when the build
    // worked.** It is nearly always a wire that stopped short, and the circuit
    // will stop working the moment that part is asked to do something.
    for (const auto& [part, pin] : session_.unconnectedParts()) {
        juce::String text(part);
        if (!pin.empty())
            text += "." + juce::String(pin);
        text += " is not connected to anything";
        messages.push_back({text, MessageConsole::Severity::Warning, juce::String(part)});
    }

    // **What the loader had to say about the sheet.** A geometry-only file
    // names nothing beside a pin, so the things a read can get wrong are
    // visible things: a part type the library does not have, or a sheet with no
    // `in`/`out` part, whose terminals then fall back to the stale names in its
    // io block. Those are not build failures -- the engine compiled what it
    // could -- so the build's own status would otherwise swallow them.
    for (const auto& note : editor_.getDocument().loadWarnings)
        messages.push_back({juce::String(note), MessageConsole::Severity::Warning, {}});

    const int problemCount = static_cast<int>(
        std::count_if(messages.begin(), messages.end(), [](const auto& message) {
            return message.severity == MessageConsole::Severity::Error ||
                   message.severity == MessageConsole::Severity::Warning;
        }));

    juce::String summary = juce::String(editor_.getDocument().components.size()) +
                           " parts  " + juce::String(session_.nodeNames().size()) +
                           " nodes  48 kHz  DC " + juce::String(session_.dcStatus());

    if (problemCount > 0)
        summary += "  --  " + juce::String(problemCount) +
                   (problemCount == 1 ? " issue" : " issues");

    statusLabel_.setText(summary, juce::dontSendNotification);
    statusLabel_.setColour(juce::Label::textColourId,
                           problemCount > 0 || !session_.dcConverged()
                               ? glow::getColourAmber()
                               : glow::getColourLaserGreen());

    presentMessages(summary,
                    problemCount > 0 ? MessageConsole::Severity::Warning
                                     : MessageConsole::Severity::Info,
                    std::move(messages));
}

void EditorWindow::Content::syncScopeProbes() {
    // **The scope's meters.** Each scope probe on the sheet gets a buffer and
    // its own colour, so the scope shows every probe's signal at once instead
    // of only the one that happens to be selected.
    //
    // The buffers are kept between rebuilds -- a probe that is still on the
    // sheet keeps its history, so the trace does not restart from nothing
    // every time the circuit is rebuilt.
    const auto& traces = session_.probeTraces();

    // The buffers are kept between rebuilds -- a probe that is still on the
    // sheet keeps its history, so the trace does not restart from nothing
    // every time the circuit is rebuilt.
    std::map<juce::String, std::unique_ptr<ScopeAudioBuffer>> kept;
    for (auto& entry : scopeProbes_)
        kept[entry.first] = std::move(entry.second);

    // Rebuilt in the session's order, so `pumpScope`'s index into the traces
    // and its index into the buffers are the same probe.
    scopeProbes_.clear();
    for (const auto& [id, idx] : traces) {
        const juce::String name(id);
        auto it = kept.find(name);
        if (it != kept.end()) {
            scopeProbes_.emplace_back(name, std::move(it->second));
            kept.erase(it);
        } else {
            scopeProbes_.emplace_back(name, std::make_unique<ScopeAudioBuffer>());
        }
    }

    std::vector<ScopeView::ProbeTrace> viewTraces;
    viewTraces.reserve(scopeProbes_.size());
    for (auto& entry : scopeProbes_) {
        ScopeView::ProbeTrace trace;
        trace.label = entry.first;
        trace.buffer = entry.second.get();
        viewTraces.push_back(std::move(trace));
    }
    scope_.setProbeTraces(std::move(viewTraces));
}

void EditorWindow::Content::clearScopeHistory() {
    scopeIn_.clear();
    scopeOut_.clear();
    for (auto& entry : scopeProbes_) {
        if (entry.second)
            entry.second->clear();
    }
    scopePhase_ = 0.0;
    scope_.clearTraces();
}

void EditorWindow::Content::refreshOperatingPoint() {
    nodeTable_.setOperatingPoint(session_.dcVoltages());
    nodeTable_.setDeviceCurrents(session_.dcCurrents());
    nodeTable_.setStatus(session_.dcStatus(), !session_.dcConverged());
    editor_.setNodeVoltages(session_.dcVoltages());
}

void EditorWindow::Content::pumpScope() {
    if (!session_.valid()) return;

    // One block per UI tick. 512 samples at 48 kHz is ~10 ms, so the 30 Hz tick
    // stays ahead of real time and the scope never starves.
    constexpr int kBlock = 512;
    constexpr double kFs = 48000.0;
    const double dPhase = 2.0 * juce::MathConstants<double>::pi * kTestFreq / kFs;

    // **The scope traces the probe when the sheet has one.** The scope probe
    // stamps nothing into the circuit -- it only names a net -- and it is the
    // editor's "look here": put it anywhere and the scope shows that point
    // instead of the output. That is the difference between a scope that shows
    // you the circuit's end and one that shows you the part you are working on.
    //
    // The label follows, so the trace says what it is: the probe's id, or "OUT"
    // when there is no probe.
    const int probeIdx = session_.probeIndex();
    const int outIdx = session_.outputIndex();
    const int traceIdx = probeIdx >= 0 ? probeIdx : outIdx;
    scope_.setOutputLabel(probeIdx >= 0 ? juce::String(session_.probeLabel()) : juce::String("OUT"));

    float peak = 0.0f;
    for (int i = 0; i < kBlock; ++i) {
        const double in = kTestAmplitude * std::sin(scopePhase_);
        scopePhase_ += dPhase;
        if (scopePhase_ > 2.0 * juce::MathConstants<double>::pi)
            scopePhase_ -= 2.0 * juce::MathConstants<double>::pi;

        const auto& nodes = session_.step(in);

        // The input trace is **what is actually going into the circuit** -- the
        // value on the input source. That is the editor's test signal when the
        // sheet has an Input terminal, and the source's own waveform when it
        // does not: a circuit excited by its own 1 Hz generator shows *that* on
        // the input trace, which is the whole point of the editor being a lab.
        const double circuitIn = session_.inputVolts();

        const double out = (traceIdx >= 0 && traceIdx < static_cast<int>(nodes.size()))
                               ? nodes[static_cast<size_t>(traceIdx)]
                               : 0.0;

        // A non-finite sample is a solver failure, not a signal. The scope
        // buffer sanitizes it so the readout cannot say "inf V"; the log keeps
        // the failure visible instead of hiding it.
        if (!std::isfinite(out) || !std::isfinite(circuitIn)) {
            static int nonFinite = 0;
            if (nonFinite++ < 8)
                diagLog("scope: non-finite sample in=" + std::to_string(circuitIn) +
                        " out=" + std::to_string(out) + " outIdx=" + std::to_string(outIdx) +
                        " traceIdx=" + std::to_string(traceIdx));
        }

        scopeIn_.pushSample(static_cast<float>(circuitIn));
        scopeOut_.pushSample(static_cast<float>(out));
        peak = std::max(peak, static_cast<float>(std::abs(out)));

        // **The probe traces.** Each scope probe on the sheet reads the node it
        // names, and the scope draws that signal on its own colour.
        const auto& traces = session_.probeTraces();
        for (size_t p = 0; p < traces.size() && p < scopeProbes_.size(); ++p) {
            const int idx = traces[p].second;
            const double v = (idx >= 0 && idx < static_cast<int>(nodes.size()))
                                 ? nodes[static_cast<size_t>(idx)]
                                 : 0.0;
            scopeProbes_[p].second->pushSample(static_cast<float>(v));
        }
    }

    // Drive the glowing signal-flow beads with the output level, so the drawing
    // lights up when the circuit is doing something.
    editor_.setSignalActivity(std::min(1.0f, peak * 4.0f));
}

void EditorWindow::Content::timerCallback() {
    startSavedCircuitPreparation();
    pumpScope();
}

// ----------------------------------------------------------------------------
// Layout
// ----------------------------------------------------------------------------

void EditorWindow::Content::paint(juce::Graphics& g) {
    g.fillAll(bg());

    // Header band — slightly lighter than the sheet, so the toolbar reads as
    // chrome. The colour is Celine's `headerBackground`.
    g.setColour(headerBand());
    g.fillRect(0, 0, getWidth(), kToolbarH);

    // The tab strip is painted here as well as by `TabBar` because its right
    // end holds the Presets and Edit actions rather than tabs. Without this the
    // actions would sit on a differently coloured patch.
    g.setColour(glow::getColourPanel());
    g.fillRect(0, kToolbarH, getWidth(), kTabBarH);

    // Toolbar group separators — faint vertical hairlines between logical
    // button groups. The positions are derived from the button bounds laid
    // out in `resized()`: each separator sits at the midpoint of the gap
    // between the last button of one group and the first button of the next.
    {
        g.setColour(panelEdge().withAlpha(0.4f));
        const float top    = 7.0f;
        const float bottom = static_cast<float>(kToolbarH) - 7.0f;

        // File | Build boundary
        const float sep1 = (saveAsBtn_.getRight() + rebuildBtn_.getX()) * 0.5f;
        // Build | Tools boundary
        const float sep2 = (rebuildBtn_.getRight() + selectBtn_.getX()) * 0.5f;
        // Tools | Edit boundary
        const float sep3 = (deleteBtn_.getRight() + undoBtn_.getX()) * 0.5f;
        // Edit | View boundary
        const float sep4 = (redoBtn_.getRight() + frameBtn_.getX()) * 0.5f;

        g.drawVerticalLine(juce::roundToInt(sep1), top, bottom);
        g.drawVerticalLine(juce::roundToInt(sep2), top, bottom);
        g.drawVerticalLine(juce::roundToInt(sep3), top, bottom);
        g.drawVerticalLine(juce::roundToInt(sep4), top, bottom);
    }

    // Status bar — a subtle darker strip along the bottom, like a hardware
    // status display. Quiet when things are good, loud when they are not.
    g.setColour(glow::getColourPanel().darker(0.03f));
    g.fillRect(0, getHeight() - kStatusH, getWidth(), kStatusH);

    // Structural hairlines. The columns stop at the message drawer when it is
    // open, so the drawer reads as one full-width band rather than as a piece of
    // the palette and a piece of the inspector.
    const float contentBottom =
        consoleOpen_ ? static_cast<float>(console_.getY())
                     : static_cast<float>(getHeight() - kStatusH);

    g.setColour(panelEdge());
    g.drawHorizontalLine(kToolbarH, 0.0f, static_cast<float>(getWidth()));
    g.drawHorizontalLine(getHeight() - kStatusH, 0.0f, static_cast<float>(getWidth()));
    if (consoleOpen_)
        g.drawHorizontalLine(juce::roundToInt(contentBottom), 0.0f,
                             static_cast<float>(getWidth()));
    g.drawVerticalLine(kPaletteW, static_cast<float>(kToolbarH), contentBottom);
    g.drawVerticalLine(getWidth() - kRightW, static_cast<float>(kToolbarH), contentBottom);
}

void EditorWindow::Content::resized() {
    auto area = getLocalBounds();

    auto toolbar = area.removeFromTop(kToolbarH).reduced(4, 3);

    // File group: New / Open / Save / Save As
    newBtn_.setBounds(toolbar.removeFromLeft(48));
    toolbar.removeFromLeft(4);
    openBtn_.setBounds(toolbar.removeFromLeft(52));
    toolbar.removeFromLeft(4);
    saveBtn_.setBounds(toolbar.removeFromLeft(48));
    toolbar.removeFromLeft(4);
    saveAsBtn_.setBounds(toolbar.removeFromLeft(64));

    toolbar.removeFromLeft(16);  // ── group gap ──

    // Build group: Rebuild
    rebuildBtn_.setBounds(toolbar.removeFromLeft(64));

    toolbar.removeFromLeft(16);  // ── group gap ──

    // Tools group: Select / Wire / Delete
    selectBtn_.setBounds(toolbar.removeFromLeft(58));
    toolbar.removeFromLeft(4);
    wireBtn_.setBounds(toolbar.removeFromLeft(52));
    toolbar.removeFromLeft(4);
    deleteBtn_.setBounds(toolbar.removeFromLeft(58));

    toolbar.removeFromLeft(16);  // ── group gap ──

    // Edit group: Undo / Redo
    undoBtn_.setBounds(toolbar.removeFromLeft(52));
    toolbar.removeFromLeft(4);
    redoBtn_.setBounds(toolbar.removeFromLeft(52));

    toolbar.removeFromLeft(16);  // ── group gap ──

    // View group: Fit / Layout / About / Keys
    frameBtn_.setBounds(toolbar.removeFromLeft(44));
    toolbar.removeFromLeft(4);
    layoutBtn_.setBounds(toolbar.removeFromLeft(64));
    toolbar.removeFromLeft(4);
    aboutBtn_.setBounds(toolbar.removeFromLeft(56));
    toolbar.removeFromLeft(4);
    keysBtn_.setBounds(toolbar.removeFromLeft(52));

    presetStepper_.setBounds(toolbar);

    auto status = area.removeFromBottom(kStatusH).reduced(6, 0);
    messagesBtn_.setBounds(status.removeFromRight(96).reduced(0, 2));
    statusLabel_.setBounds(status);

    // The drawer sits above the status bar and takes height from every column,
    // not just the sheet: a message about a part and the node table that shows
    // what that part is doing should be readable together.
    if (consoleOpen_) {
        console_.setBounds(area.removeFromBottom(kConsoleH));
        console_.setVisible(true);
    } else {
        console_.setBounds({});
        console_.setVisible(false);
    }

    // **The tab strip**, between the toolbar and the sheet -- where a browser
    // puts it, and where the eye expects to find "which circuit am I in". The
    // two strip actions take their width from the right, so tabs never run
    // underneath them.
    auto tabStrip = area.removeFromTop(kTabBarH);
    auto stripActions = tabStrip.removeFromRight(132);
    tabBar_.setBounds(tabStrip);

    presetsBtn_.setBounds(stripActions.removeFromRight(78).reduced(2, 3));
    stripActions.removeFromRight(4);
    editMenuBtn_.setBounds(stripActions.removeFromRight(48).reduced(2, 3));

    if (presetLibrary_.isVisible()) {
        placePresetLibrary();
        presetDismisser_.setBounds(getLocalBounds());
    }

    palette_.setBounds(area.removeFromLeft(kPaletteW));

    auto right = area.removeFromRight(kRightW);
    nodeTable_.setBounds(right.removeFromTop(right.getHeight() / 2));
    inspector_.setBounds(right);

    scope_.setBounds(area.removeFromBottom(kScopeH));
    auto editorArea = area;
    // Feed the rulers before the sheet lays the rest out: the strips take their
    // own thickness off the top and left, and the canvas gets what is left. The
    // canvas is not inset by a guessed number -- the strip measures itself, so a
    // change to `Ruler::thickness` moves both in step.
    topRuler_.setBounds(editorArea.removeFromTop(topRuler_.thickness()));
    leftRuler_.setBounds(editorArea.removeFromLeft(leftRuler_.thickness()));
    editor_.setBounds(editorArea);
}

} // namespace livewdf::editor
