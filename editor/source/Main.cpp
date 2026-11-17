// The standalone editor's entry point.
//
// `JUCE_MAIN_FUNCTION` / `START_JUCE_APPLICATION` is JUCE's platform-correct
// main: it picks WinMain / main as the platform needs and keeps this file free of
// `#ifdef _WIN32`. Nothing else belongs here.

#include "EditorApp.h"

START_JUCE_APPLICATION(livewdf::editor::EditorApp)
