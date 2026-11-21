cmake_minimum_required(VERSION 3.22)
set(LIVEWDF_VERSION 0.1.0 CACHE STRING "Version stamped into the binaries")
project(LiveWDF VERSION ${LIVEWDF_VERSION} LANGUAGES C CXX)

# --- Language / toolchain -----------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LIVEWDF_BUILD_TESTS "Build LiveWDF tests" ON)

# Shared warning options for LiveWDF targets (third_party is included as SYSTEM).
add_library(livewdf_options INTERFACE)
if(MSVC)
    target_compile_options(livewdf_options INTERFACE /W4 /permissive- /utf-8 /D_USE_MATH_DEFINES /D_CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(livewdf_options INTERFACE -Wall -Wextra -Wpedantic)
endif()
# The compiler's target, because CMAKE_SYSTEM_PROCESSOR is the host for a Visual Studio -A build.
set(LIVEWDF_ARCH "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
if(NOT LIVEWDF_ARCH)
    set(LIVEWDF_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(LIVEWDF_ARCH MATCHES "^(x86_64|x64|X86|AMD64|i[3-6]86)$")
    target_compile_options(livewdf_options INTERFACE "$<IF:$<BOOL:${MSVC}>,/arch:AVX2,-mavx2;-mfma>")
endif()

# --- Core library -------------------------------------------------------------
# Layout (see docs/ARCHITECTURE.md):
#   src/spice/    SPICE front end: parser -> netlist IR
#   src/circuit/  SPQR contraction, R-type scattering, tree compilation
#   src/devices/  nonlinear device models (triode, pentode, diode)
#   src/wdf/      engine: dense step, root solve, compile, buffers
#   src/format/   native .livewdf document/JSON format
#
# The core is deliberately JUCE-free. Anything that includes a JUCE header
# lives under `plugin/` or `editor/`, whose targets own the JUCE include paths.
file(GLOB_RECURSE LIVEWDF_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(livewdf STATIC ${LIVEWDF_SOURCES})
target_include_directories(livewdf PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(livewdf PRIVATE livewdf_options)
target_compile_definitions(livewdf PRIVATE LIVEWDF_VERSION_STRING="${PROJECT_VERSION}")
set_target_properties(livewdf PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- Symbol artwork -----------------------------------------------------------
# The parts' shapes live as SVG files and are embedded into the binary, exactly as
# Celine does it -- so redrawing a resistor is editing a file, not a C++ edit,
# which is the whole point of porting the loader. Defined here, above both the
# tests and the editor, because both link it: the editor draws with it, the editor
# tests walk its type list to catch a missing file.
#
# JUCE has to be added first: `juce_add_binary_data` is a JUCE CMake function, so
# it is not defined until the JUCE subdirectory has been processed.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/modules/JUCE/CMakeLists.txt")
    add_subdirectory(modules/JUCE EXCLUDE_FROM_ALL)
endif()

# The glob is CONFIGURE_DEPENDS: dropping a new file in comes back on the next
# build without re-running CMake by hand. It is recursive because the
# transistor symbols live in `elements/transistors/`; a non-recursive glob
# silently skipped them, which is exactly the "BJT is missing" symptom.
file(GLOB_RECURSE LIVEWDF_ELEMENT_ASSETS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/editor/assets/elements/*.svg")

if(LIVEWDF_ELEMENT_ASSETS AND COMMAND juce_add_binary_data)
    juce_add_binary_data(LiveWDF_EditorArtwork
        SOURCES ${LIVEWDF_ELEMENT_ASSETS}
        NAMESPACE ArtworkData
    )
endif()

# The editor's typeface. Jura, bundled rather than asked for by name: the
# editor cannot assume anything about the machine it runs on, and
# `juce::Font("Jura", ...)` on a machine without it silently falls back to the
# platform sans -- which looks like a layout bug rather than a missing font,
# because every label is suddenly a different width.
#
# Jura is under the SIL Open Font License 1.1, whose text is bundled alongside it
# in `editor/assets/fonts/`.
file(GLOB LIVEWDF_FONT_ASSETS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/editor/assets/fonts/*.ttf")

if(LIVEWDF_FONT_ASSETS AND COMMAND juce_add_binary_data)
    # `HEADER_NAME` because every binary-data target generates a header called
    # `BinaryData.h`, and two of them on one include path means the compiler
    # picks whichever it likes -- silently, and the symptom is a namespace that
    # is not there.
    juce_add_binary_data(LiveWDF_EditorFonts
        SOURCES ${LIVEWDF_FONT_ASSETS}
        NAMESPACE FontData
        HEADER_NAME FontData.h
    )
endif()

# The factory circuits shipped with the editor and plugin. They live as real
# files in `schematics/` so they can be edited and reviewed in the repository,
# but an installed binary cannot see the checkout. Pack the folder into one
# archive, embed that archive, and let the shared factory helper unpack it into
# the user's Documents folder on first run (or when the factory contents
# change). A zip keeps the `amps/` and `pedals_fx/` paths; individual binary
# resources would flatten them and need a second manifest.
file(GLOB_RECURSE LIVEWDF_FACTORY_SCHEMATICS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/schematics/*.livewdf")
list(SORT LIVEWDF_FACTORY_SCHEMATICS)

if(LIVEWDF_FACTORY_SCHEMATICS AND COMMAND juce_add_binary_data)
    # Hash the source paths and bytes, not the generated zip: the archive's
    # timestamps may change when it is rebuilt, while the circuits a user sees
    # have not. The hash is the version marker the runtime compares.
    set(LIVEWDF_FACTORY_SCHEMATICS_HASH "")
    foreach(schematic IN LISTS LIVEWDF_FACTORY_SCHEMATICS)
        file(SHA256 "${schematic}" schematic_hash)
        file(RELATIVE_PATH schematic_relative
            "${CMAKE_CURRENT_SOURCE_DIR}/schematics" "${schematic}")
        string(APPEND LIVEWDF_FACTORY_SCHEMATICS_HASH
            "${schematic_relative}:${schematic_hash}\n")
    endforeach()
    string(SHA256 LIVEWDF_FACTORY_SCHEMATICS_VERSION
        "${LIVEWDF_FACTORY_SCHEMATICS_HASH}")

    set(LIVEWDF_FACTORY_SCHEMATICS_ZIP
        "${CMAKE_CURRENT_BINARY_DIR}/LiveWDF_FactorySchematics.zip")
    add_custom_command(
        OUTPUT "${LIVEWDF_FACTORY_SCHEMATICS_ZIP}"
        COMMAND "${CMAKE_COMMAND}" -E tar cf
            "${LIVEWDF_FACTORY_SCHEMATICS_ZIP}" --format=zip -- .
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/schematics"
        DEPENDS ${LIVEWDF_FACTORY_SCHEMATICS}
        COMMENT "Packing factory schematics"
        VERBATIM
    )
    add_custom_target(LiveWDF_FactorySchematicsZip
        DEPENDS "${LIVEWDF_FACTORY_SCHEMATICS_ZIP}")

    juce_add_binary_data(LiveWDF_FactorySchematics
        SOURCES "${LIVEWDF_FACTORY_SCHEMATICS_ZIP}"
        NAMESPACE FactoryData
        HEADER_NAME FactoryData.h
    )
    add_dependencies(LiveWDF_FactorySchematics LiveWDF_FactorySchematicsZip)
    target_compile_definitions(LiveWDF_FactorySchematics INTERFACE
        LIVEWDF_FACTORY_SCHEMATICS_VERSION="${LIVEWDF_FACTORY_SCHEMATICS_VERSION}")
endif()

# --- Tests --------------------------------------------------------------------
if(LIVEWDF_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# --- Shared Schematic GUI / Standalone Editor ---------------------------------
# The drawing surface for .livewdf circuits owns the schematic GUI: the model,
# the canvas and the look. The plugin embeds that canvas read-only, so a file
# renders identically in both windows.
#
# `LiveWDF_EditorCore` is shared, so the editor subdirectory is added whenever
# either target is enabled. The editor app itself is guarded by
# `LIVEWDF_BUILD_EDITOR` inside editor/CMakeLists.txt.
option(LIVEWDF_BUILD_EDITOR "Build LiveWDF Standalone Editor" ON)
option(LIVEWDF_BUILD_PLUGIN "Build LiveWDF Audio Plugin" ON)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/modules/JUCE/CMakeLists.txt"
   AND (LIVEWDF_BUILD_EDITOR OR LIVEWDF_BUILD_PLUGIN))
    add_subdirectory(editor)

    # The editor tests link `LiveWDF_EditorCore`, so they are added after the
    # editor target that defines it. They still hang off the tests option.
    if(LIVEWDF_BUILD_TESTS)
        add_subdirectory(tests/editor)
    endif()
endif()

# --- Audio Plugin (VST3 / Standalone) -----------------------------------------
# The plugin plays a circuit; its schematic view is the editor's canvas from
# `LiveWDF_EditorCore`, linked read-only.
if(LIVEWDF_BUILD_PLUGIN AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/modules/JUCE/CMakeLists.txt")
    add_subdirectory(plugin)
endif()

# --- Visual verification harness ----------------------------------------------
# Renders Celine's editor and ours to PNG, so the appearance of the drawing can
# be checked by looking at it rather than by trusting a green build. Off by
# default: it is a development tool, not part of the product, and it compiles
# Celine's sources when they are present. See docs/FINDINGS_CELINE_EDITOR.md.
option(LIVEWDF_BUILD_RENDER "Build the PNG render harness" OFF)
if(LIVEWDF_BUILD_RENDER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/modules/JUCE/CMakeLists.txt")
    add_subdirectory(tools/render)
endif()

