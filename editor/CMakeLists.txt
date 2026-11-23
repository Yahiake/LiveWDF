cmake_minimum_required(VERSION 3.22)

# ==============================================================================
# LiveWDF Standalone Editor
#
# A drawing surface for .livewdf circuits. It consumes the `livewdf` engine and
# JUCE. `LiveWDF_EditorCore` is the shared schematic GUI: the model, the canvas,
# the panels and the look. The editor app adds only the window shell and the
# widgets that need a running desktop; the plugin links the same core and shows
# its canvas read-only. The app, the plugin and `livewdf_editor_tests` all use
# one source list, so a file looks the same in both windows.
# ==============================================================================

set(LIVEWDF_EDITOR_CORE_SOURCES
    source/SchematicModel.cpp
    source/SchematicModel.h
    source/SchematicHistory.cpp
    source/SchematicHistory.h
    source/CircuitSession.cpp
    source/CircuitSession.h
    # What is *not* here, and why: the part-facts table (`PartLibrary`,
    # `PartModels`), the engineering-notation formatter (`ValueFormat`) and the
    # net extractor (`SchematicNets`) moved to `src/format/`. Each is a fact
    # about a part or about a drawing -- pin names, pin offsets, default values,
    # how a value is written, which pins share a net -- and the document reader
    # needs the same answers the canvas draws from, with no window in sight.
    # Two accounts of one fact is the bug this project keeps getting told off
    # for, and it is silent: a pin one grid off in one of the two is a part
    # wired to nothing. The editor reaches all of them through the `livewdf`
    # library it links below.
    source/ElementAccess.cpp
    source/ElementAccess.h
    source/SymbolArtwork.cpp
    source/SymbolArtwork.h
    source/Shortcut.cpp
    source/Shortcut.h
    source/Clipboard.cpp
    source/Clipboard.h
    source/SchematicEditor.cpp
    source/SchematicEditorEditing.cpp
    source/SchematicEditorGeometry.cpp
    source/SchematicEditorPaint.cpp
    source/SchematicEditorGestures.cpp
    source/SchematicEditorCommands.cpp
    source/SchematicEditorInternal.h
    source/SchematicEditor.h
    source/PartPalettePanel.cpp
    source/PartPalettePanel.h
    source/EditorLookAndFeel.cpp
    source/EditorLookAndFeel.h
    source/InspectorPanel.cpp
    source/InspectorPanel.h
    source/MessageConsole.cpp
    source/MessageConsole.h
    source/PresetLibraryPanel.cpp
    source/PresetLibraryPanel.h
    source/FactorySchematics.cpp
    source/FactorySchematics.h
    source/DiagLog.cpp
    source/DiagLog.h
    gui/Fonts.cpp
    gui/Fonts.h
    gui/UiLookAndFeel.cpp
    gui/UiLookAndFeel.h
    gui/AboutDialog.cpp
    gui/AboutDialog.h
    gui/PluginLayoutDialog.cpp
    gui/PluginLayoutDialog.h
    gui/glow/Glow.cpp
    gui/glow/Glow.h
    gui/glow/SchematicConstants.h
    gui/ScopeView.cpp
    gui/ScopeView.h
)

add_library(LiveWDF_FastLiveCache STATIC source/FastLiveCache.cpp)
set_target_properties(LiveWDF_FastLiveCache PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(LiveWDF_FastLiveCache PRIVATE JUCE_USE_CURL=0)
target_link_libraries(LiveWDF_FastLiveCache PRIVATE livewdf livewdf_options juce::juce_cryptography)

add_library(LiveWDF_EditorCore STATIC ${LIVEWDF_EDITOR_CORE_SOURCES})
set_target_properties(LiveWDF_EditorCore PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(LiveWDF_EditorCore PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)
target_include_directories(LiveWDF_EditorCore PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/source"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui/glow"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_BINARY_DIR}/../juce_binarydata_LiveWDF_EditorArtwork/JuceLibraryCode"
    "${CMAKE_CURRENT_BINARY_DIR}/../juce_binarydata_LiveWDF_EditorFonts/JuceLibraryCode"
)
target_link_libraries(LiveWDF_EditorCore PUBLIC
    livewdf
    livewdf_options
    LiveWDF_EditorArtwork
    LiveWDF_EditorFonts
    LiveWDF_FactorySchematics
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    PRIVATE LiveWDF_FastLiveCache
)

# The app is optional; the core above is not. The plugin links the core for its
# read-only schematic view, so `LIVEWDF_BUILD_EDITOR=OFF` turns off the editor
# window, not the canvas.
if(LIVEWDF_BUILD_EDITOR)
    juce_add_gui_app(LiveWDF_Editor
        PRODUCT_NAME "LiveWDF Editor"
        COMPANY_NAME "LiveWDF"
        VERSION "0.1.0"
    )

    target_sources(LiveWDF_Editor PRIVATE
        source/Main.cpp
        source/EditorWindow.cpp
        source/EditorWindowFiles.cpp
        source/EditorWindowEngine.cpp
        source/EditorWindowPanels.cpp
        source/EditorWindowUndo.cpp
        source/EditorWindowSelfTest.cpp
        source/EditorWindowInternal.h
        source/EditorWindow.h
        source/EditorApp.h
        source/Ruler.cpp
        source/Ruler.h
        source/TabBar.cpp
        source/TabBar.h
        source/NodeTablePanel.cpp
        source/NodeTablePanel.h
        gui/glow/SignalPath.cpp
        gui/glow/SignalPath.h
    )

    target_compile_definitions(LiveWDF_Editor PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:LiveWDF_Editor,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:LiveWDF_Editor,JUCE_VERSION>"
    )

    target_link_libraries(LiveWDF_Editor PRIVATE
        LiveWDF_EditorCore
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
    )
endif()
