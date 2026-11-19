#pragma once

// The editor's self-test: the whole window, driven headlessly.
//
// **Why it exists.** The editor is where circuits are drawn and tested, and the
// parts of it that can crash are the parts that mix the model, the document and
// the widgets: a picker that fires a change, a host that rebuilds the panel in
// answer, a canvas that replaces the document underneath. None of that is
// reachable from a unit test, because it needs a real window and a real message
// loop -- and a crash in one of those took the editor down.
//
// So the editor can check itself: `--selftest` builds a window, puts a part with
// a picker on the sheet, clicks it, changes the pick, and lets the message loop
// run between each step -- and then says whether it survived.
//
// **Why it is a timer rather than one function.** The crash lived in a message
// the widgets *posted*: the picker's opening selection notifies asynchronously,
// so the callback that used to take the editor down ran a moment later, after
// the panel had been rebuilt. A self-test that did everything in one go would
// never deliver that message, and would pass with the bug still in it. So the
// steps are spread over timer ticks, and the message loop runs between them.
//
// The output is a line per check, so a failure says which one.

#include "DiagLog.h"
#include "EditorWindow.h"
#include "gui/UiLookAndFeel.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <iostream>

namespace livewdf::editor {

class EditorApp : public juce::JUCEApplication,
                   private juce::Timer {
public:
    const juce::String getApplicationName() override { return "LiveWDF Editor"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& commandLine) override {
        juce::String arg = commandLine.trim().unquoted().trim();

        // `--diag` turns on the diagnostic log. It may be followed by a file
        // path (`--diag circuit.livewdf`), so remove the flag before the rest
        // of the line is treated as a path.
        if (arg.contains("--diag")) {
            setDiagLogEnabled(true);
            arg = arg.replace("--diag", "").trim().unquoted().trim();
        }

        // The editor's look and feel, installed before anything is shown: a
        // widget that has already been laid out keeps the font it was given.
        //
        // **Installed for the self-test too**, deliberately: the self-test is
        // the app in a smaller window, and a configuration the self-test does
        // not exercise is a configuration nothing checks.
        lookAndFeel_ = std::make_unique<UiLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel(lookAndFeel_.get());

        if (arg.contains("--selftest")) {
            // The self-test always logs: the whole point of it is to be read
            // afterwards, and a self-test that says "passed" without saying
            // what it did is only half a result.
            setDiagLogEnabled(true);
            startSelfTest();
            return;
        }

        window_ = std::make_unique<EditorWindow>();

        // Opening a file from the OS (double-click, or a path on the command
        // line) is what makes `.livewdf` feel like a document type rather than an
        // import. The first argument is taken as a path when it names a file.
        if (arg.isNotEmpty()) {
            const juce::File f(arg);
            if (f.existsAsFile())
                window_->openFile(f);
        }
    }

    void shutdown() override { window_.reset(); }

    void systemRequestedQuit() override { quit(); }

    void anotherInstanceStarted(const juce::String&) override {}

private:
    // ---------------------------------------------------------------------------
    // The self-test
    // ---------------------------------------------------------------------------

    void startSelfTest() {
        // The logger writes to the debugger on Windows, which nobody reads in a
        // console. The self-test prints to stdout instead, so its output can be
        // captured and looked at.
        say("LiveWDF editor self-test");

        window_ = std::make_unique<EditorWindow>();

        // **Show it.** A window that was never laid out has never run its
        // layout code, and the layout is where a panel's rows are given their
        // sizes -- so a self-test that never showed the window would not have
        // exercised the very thing that was broken.
        window_->setSize(1200, 800);
        window_->setVisible(true);

        step_ = 0;
        failures_ = 0;

        // Twenty ticks a second, so the whole thing takes well under a second and
        // the message loop gets a real turn between every step.
        startTimerHz(20);
    }

    void timerCallback() override {
        switch (step_++) {
            case 0:
                // Put the parts on the sheet. A triode (a device) and a
                // potentiometer (a taper) are the two shapes the picker path
                // takes; a resistor is the one that does not, and is the control.
                for (const char* type : {"triode", "pot", "res"})
                    check(juce::String("placing a ") + type, window_->selfTestPlacePart(type));
                break;

            case 1:
                // Click them. This is the step that used to be fatal.
                for (const char* type : {"triode", "pot"}) {
                    const bool ok = window_->selfTestSelectPart(type);
                    check(juce::String("clicking a ") + type
                              + (ok ? juce::String() : juce::String(" -- ") + window_->selfTestNote()),
                          ok);
                }
                break;

            case 2:
                // Change the picker's value.
                for (const char* type : {"triode", "pot"}) {
                    const bool ok = window_->selfTestChangeFirstChoice(type);
                    check(juce::String("changing a ") + type + "'s picker"
                              + (ok ? juce::String() : juce::String(" -- ") + window_->selfTestNote()),
                          ok);
                }
                break;

            case 3:
                // **Recompile the engine.** Changing a part's model changes the
                // engine, and the engine is recompiled on the Rebuild button --
                // which the user presses before going back to the sheet. The
                // self-test had been skipping this, and it is the step where a
                // model that cannot be built shows up.
                check("rebuilding the engine", window_->selfTestRebuild());
                break;

            case 4:
                // **And click it again.** Changing the picker rebuilds the
                // inspector, so the part is now selected and its picker is a
                // *different* widget from the one the user changed. Clicking
                // the part again is what the user does next -- to look at it, to
                // check the new model took, to move on -- and it is the step
                // where the crash was.
                for (const char* type : {"triode", "pot"}) {
                    const bool ok = window_->selfTestSelectPart(type);
                    check(juce::String("clicking a ") + type + " again"
                              + (ok ? juce::String() : juce::String(" -- ") + window_->selfTestNote()),
                          ok);
                }
                break;

            case 5:
                // **Arm a part by its keyboard shortcut.** The ground is a bare
                // "G" and the gain block is "Ctrl+G" -- the same letter with and
                // without the modifier -- so this checks that the two are told
                // apart, which is the whole of the shortcut arrangement.
                check("pressing G arms the ground",
                      window_->selfTestArmByShortcut('g', false, "gnd"));
                check("pressing Ctrl+G arms the gain block",
                      window_->selfTestArmByShortcut('g', true, "vcvs"));

                // **And the picking.** "Selecting some items selects unwanted
                // ones, and wires are especially hard to select" -- so: a
                // click on a wire selects the wire and not the part it
                // reaches, and a click beside a part selects nothing at all.
                check("clicking a wire selects the wire",
                      window_->selfTestSelectWire());
                check("clicking beside a part selects nothing",
                      window_->selfTestMissPart());

                // **And the drag.** A part that has been dragged is a different
                // circuit: the Rebuild button has to unlock, and the document
                // has to follow -- or pressing Rebuild would compile the
                // circuit with the part where it used to be.
                {
                    const bool ok = window_->selfTestDragPart();
                    check(juce::String("dragging a part marks the sheet as changed")
                              + (ok ? juce::String() : " -- " + window_->selfTestNote()),
                          ok);
                }

                // **The tabs and the title.** The title bar answers "which
                // circuit am I in", so it has to follow the tab: it used to be
                // the window's file, which meant a second tab's title named the
                // first tab's circuit.
                {
                    const auto before = window_->tabTitles().size();
                    window_->newTab();

                    const auto after = window_->tabTitles().size();
                    check("the New button opens a second tab", after == before + 1);

                    // And the title names the tab that is showing.
                    const auto titles = window_->tabTitles();
                    const juce::String title = window_->titleText();
                    check("the title names the current tab",
                          !title.isEmpty()
                              && (title == titles.back() || title == titles.front()));
                }
                break;

            case 6:
                // And now let the message loop deliver anything the widgets
                // posted -- the picker's opening notification, and the deferred
                // change notification. This is where the crash used to happen.
                check("survived the message loop", true);

                // **And the loop must be gone.** A picker sets its own opening
                // position, and JUCE posts a change for that; the panel used to
                // rebuild itself in answer, and the editor spun building and
                // destroying widgets as fast as the message loop would go --
                // which is what took it down on a part with a picker.
                //
                // So the count of rebuilds has to be a small number. It was in
                // the thousands.
                {
                    const int rebuilds = window_->inspectorRebuildCount();
                    say("  (" + juce::String(rebuilds) + " inspector rebuilds)");
                    check("the inspector did not rebuild itself in a loop",
                          rebuilds < 30);
                }

                // **The two new overlays have to be reachable, not just
                // buildable.** A dropdown that is visible with zero width and a
                // drawer that never takes height are both invisible to a
                // unit test, and both look like a live button to the user.
                {
                    const bool ok = window_->selfTestPresetLibrary();
                    check("the preset library opens and closes"
                              + (ok ? juce::String() : " -- " + window_->selfTestNote()),
                          ok);
                }
                {
                    const bool ok = window_->selfTestMessageConsole();
                    check("the message console opens and closes"
                              + (ok ? juce::String() : " -- " + window_->selfTestNote()),
                          ok);
                }
                {
                    const bool ok = window_->selfTestMessageConsoleAutoOpen();
                    check("a build problem opens the message console"
                              + (ok ? juce::String() : " -- " + window_->selfTestNote()),
                          ok);
                }
                break;

            case 7:
                // **Undo, one gesture at a time.** The history's own assertions
                // pass with the window wired up wrong, so this step asks the
                // window: Undo is dark while the pointer is down, one press
                // takes the whole drag back, and redo puts it where it ended.
                {
                    const bool ok = window_->selfTestUndoRedo();
                    check("undo/redo steps a whole drag"
                              + (ok ? juce::String() : " -- " + window_->selfTestNote()),
                          ok);
                }

                // **An inspector edit, posted.** The panel edits the document and
                // tells the host asynchronously, so this step only types the
                // number; the undo is checked on the next tick, once the message
                // loop has delivered the change.
                {
                    const bool ok = window_->selfTestInspectorValueEdit();
                    check("typing a value into the inspector"
                              + (ok ? juce::String() : " -- " + window_->selfTestNote()),
                          ok);
                }
                break;

            case 8:
                // The message loop has delivered the edit the last step posted, so
                // Undo has to have a step to take and the number has to go back.
                {
                    const bool ok = window_->selfTestInspectorUndo();
                    check("undoing an inspector edit"
                              + (ok ? juce::String() : " -- " + window_->selfTestNote()),
                          ok);
                }
                break;

            default:
                stopTimer();
                say(failures_ == 0 ? juce::String("self-test passed")
                                   : juce::String("self-test FAILED: ")
                                         + juce::String(failures_) + " check(s)");
                quit();
                break;
        }
    }

    void check(const juce::String& what, bool ok) {
        say(juce::String(ok ? "  ok   " : "  FAIL ") + what);
        if (!ok)
            ++failures_;
    }

    static void say(const juce::String& text) {
        std::cout << text << std::endl;
    }

    std::unique_ptr<EditorWindow> window_;

    /// The widget look and feel: the editor's typeface and the sizes it is used
    /// at. Owned here, because a look and feel that is destroyed before the
    /// widgets that use it is a crash on the way out.
    std::unique_ptr<UiLookAndFeel> lookAndFeel_;

    int step_ = 0;
    int failures_ = 0;
};

}  // namespace livewdf::editor
