# ==============================================================================
# Visual verification harness -- renders Céline's editor and ours to PNG.
#
# Why this exists: the build and the test suite cannot tell whether a symbol
# looks like a symbol. A green ctest says the code compiled and the invariants
# hold; it says nothing about whether a resistor reads as a resistor. This
# target produces the one artifact that *does* say that -- a picture -- so the
# appearance of the editor is verified by looking at it rather than by assuming
# a green build means it is right.
#
# It is under LiveWDF/ because it is our tooling. It reads Céline's sources but
# never modifies them: the Céline side is compiled from Celine/source and
# Celine/modules/CelineUI exactly as they are on disk.
#
# Three targets:
#   CelineReference  -- Céline's Schematic/*, ui/SchematicCanvas + SchematicSymbols
#                       + SymbolArtwork, and CelineUI's ThemePalette + Fonts,
#                       compiled against LiveWDF's JUCE.
#   celine_render    -- renders a fixed sheet through it to a PNG.
#   livewdf_render   -- renders the same sheet through our editor to a PNG.
# ==============================================================================

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../modules/JUCE/CMakeLists.txt")
    message(STATUS "render harness: JUCE not present, skipping")
    return()
endif()

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(CELINE_DIR "${REPO_ROOT}/Celine")
set(CELINE_UI_DIR "${CELINE_DIR}/modules/CelineUI")
set(LIVEWDF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")

if(NOT EXISTS "${CELINE_DIR}/source/ui/SchematicCanvas.cpp")
    message(STATUS "render harness: Celine source not present, skipping")
    return()
endif()

# --- Céline side: assets + sources -------------------------------------------
#
# The element SVGs and the two fonts the design uses, embedded exactly as Céline's
# own build embeds them (matched on original filename). The fonts are copied into
# the harness's build tree rather than referenced across repositories so the
# resource filenames are the ones Fonts.cpp asks for.
set(CELINE_ELEMENT_SVGS "")
file(GLOB CELINE_ELEMENT_SVGS CONFIGURE_DEPENDS
    "${CELINE_DIR}/assets/icons/elements/*.svg")

set(CELINE_FONTS "")
foreach(f Jura-Light.ttf Jura-Bold.ttf)
    if(EXISTS "${CELINE_DIR}/assets/Jura/${f}")
        list(APPEND CELINE_FONTS "${CELINE_DIR}/assets/Jura/${f}")
    endif()
endforeach()
if(EXISTS "${CELINE_DIR}/assets/JetBrainsMono/JetBrainsMono-Regular.ttf")
    list(APPEND CELINE_FONTS "${CELINE_DIR}/assets/JetBrainsMono/JetBrainsMono-Regular.ttf")
endif()

juce_add_binary_data(LiveWDF_CelineRefData
    SOURCES ${CELINE_ELEMENT_SVGS} ${CELINE_FONTS}
    NAMESPACE BinaryData
)

# The two headers a shim has to supply, because they are the plugin's rather than
# the kit's: ProductInfo.h (read by ThemePalette.cpp for the stored-file path, which
# a headless render never touches) and the generated BinaryData.h (read by Fonts.cpp
# for the fonts above). Both are written into the harness's own tree so nothing in
# Celine/ is edited.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/celine_shim/ProductInfo.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/shim/ProductInfo.h"
    @ONLY)

add_library(CelineReference STATIC
    "${CELINE_DIR}/source/Schematic/Schematic.cpp"
    "${CELINE_DIR}/source/ui/SchematicCanvas.cpp"
    "${CELINE_DIR}/source/ui/SchematicSymbols.cpp"
    "${CELINE_DIR}/source/ui/SymbolArtwork.cpp"
    "${CELINE_UI_DIR}/CelineUI/ThemePalette.cpp"
    "${CELINE_UI_DIR}/CelineUI/Fonts.cpp"
)

target_include_directories(CelineReference PUBLIC
    "${CELINE_DIR}/source"
    "${CELINE_DIR}/source/Schematic"
    "${CELINE_DIR}/source/ui"
    "${CELINE_UI_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/shim"
    "${CMAKE_CURRENT_BINARY_DIR}/juce_binarydata_LiveWDF_CelineRefData/JuceLibraryCode"
)

target_compile_definitions(CelineReference PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
    # Read by ThemePalette.cpp when it builds the stored-file path, and normally
    # supplied by Celine's own CMake. A headless render never takes that path, but
    # the file still has to compile.
    PRODUCT_NAME_WITHOUT_VERSION="Celine"
)

target_link_libraries(CelineReference PUBLIC
    LiveWDF_CelineRefData
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
)

# --- Renderers ----------------------------------------------------------------
# `juce_add_console_app` rather than a bare `add_executable`: it supplies the
# platform entry point and the JUCE application glue that `START_JUCE_APPLICATION`
# needs, and keeps stdout attached so the tool can print where it wrote the PNG.
juce_add_console_app(celine_render PRODUCT_NAME "celine_render")
target_sources(celine_render PRIVATE render_main.cpp)
target_include_directories(celine_render PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/celine_shim"
)
target_compile_definitions(celine_render PRIVATE LIVEWDF_RENDER_CELINE=1)
target_link_libraries(celine_render PRIVATE
    CelineReference
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
)

# --- Our editor's renderer ----------------------------------------------------
# Compiles the editor's drawing sources (not its window/panel widgets) so the same
# tool can render our canvas to a PNG and dump our geometry. It is a second copy
# of the editor target's source list on purpose -- the harness must not drag in
# the whole window just to draw a sheet.
juce_add_console_app(livewdf_render PRODUCT_NAME "livewdf_render")
target_sources(livewdf_render PRIVATE
    render_main.cpp
    "${LIVEWDF_DIR}/editor/source/SchematicEditor.cpp"
    "${LIVEWDF_DIR}/editor/source/SchematicEditorEditing.cpp"
    "${LIVEWDF_DIR}/editor/source/SchematicEditorGeometry.cpp"
    "${LIVEWDF_DIR}/editor/source/SchematicEditorPaint.cpp"
    "${LIVEWDF_DIR}/editor/source/SchematicEditorGestures.cpp"
    "${LIVEWDF_DIR}/editor/source/SchematicEditorCommands.cpp"
    "${LIVEWDF_DIR}/editor/source/SchematicEditorInternal.h"
    "${LIVEWDF_DIR}/editor/source/SchematicEditor.h"
    "${LIVEWDF_DIR}/editor/source/SchematicModel.cpp"
    "${LIVEWDF_DIR}/editor/source/SchematicModel.h"
    "${LIVEWDF_DIR}/editor/source/SchematicHistory.cpp"
    "${LIVEWDF_DIR}/editor/source/SchematicHistory.h"
    # No part-facts table (`PartLibrary`, `PartModels`) and no value formatter
    # here: both live under `src/format/` now, so the `livewdf` library this
    # target links already carries them, and a second copy of the translation
    # unit would collide at link time.
    "${LIVEWDF_DIR}/editor/source/ElementAccess.cpp"
    "${LIVEWDF_DIR}/editor/source/ElementAccess.h"
    "${LIVEWDF_DIR}/editor/source/SymbolArtwork.cpp"
    "${LIVEWDF_DIR}/editor/source/SymbolArtwork.h"
    "${LIVEWDF_DIR}/editor/source/Ruler.cpp"
    "${LIVEWDF_DIR}/editor/source/Ruler.h"
    "${LIVEWDF_DIR}/editor/source/EditorLookAndFeel.cpp"
    "${LIVEWDF_DIR}/editor/source/EditorLookAndFeel.h"
    "${LIVEWDF_DIR}/editor/source/Shortcut.cpp"
    "${LIVEWDF_DIR}/editor/source/Shortcut.h"
    "${LIVEWDF_DIR}/editor/source/Clipboard.cpp"
    "${LIVEWDF_DIR}/editor/source/Clipboard.h"
    "${LIVEWDF_DIR}/editor/gui/glow/Glow.cpp"
    "${LIVEWDF_DIR}/editor/gui/glow/Glow.h"
    # The typeface, because the drawing code asks for it by name.
    "${LIVEWDF_DIR}/editor/gui/Fonts.cpp"
    "${LIVEWDF_DIR}/editor/gui/Fonts.h"
)
target_include_directories(livewdf_render PRIVATE
    "${LIVEWDF_DIR}/editor"
    "${LIVEWDF_DIR}/editor/source"
    "${LIVEWDF_DIR}/editor/gui"
    "${LIVEWDF_DIR}/editor/gui/glow"
    "${LIVEWDF_DIR}/src"
    "${CMAKE_BINARY_DIR}/juce_binarydata_LiveWDF_EditorArtwork/JuceLibraryCode"
    "${CMAKE_BINARY_DIR}/juce_binarydata_LiveWDF_EditorFonts/JuceLibraryCode"
)
target_compile_definitions(livewdf_render PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)
target_link_libraries(livewdf_render PRIVATE
    livewdf
    LiveWDF_EditorArtwork
    LiveWDF_EditorFonts
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
)
