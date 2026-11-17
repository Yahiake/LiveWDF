cmake_minimum_required(VERSION 3.22)

# ==============================================================================
# Editor logic tests
#
# The editor's testable surface is `LiveWDF_EditorCore` -- the model, canvas,
# panels and look. The app target adds only the desktop shell, so these tests
# link the same sources the app does rather than a second copy of the list.
#
# The tests are grouped by the part of the editor they exercise:
#
#   model/      Schematic, history, clipboard, element access
#   canvas/     drawing, hit testing, symbols, palette layout
#   session/    compile/solve/reload of a document
#   inspector/  pickers, part models, SPDT controls
#   util/       shortcuts and value formatting
#   support/    Céline reference geometry/theme headers
# ==============================================================================

add_executable(livewdf_editor_tests
    main.cpp

    model/test_element_access.cpp
    model/test_schematic_history.cpp
    model/test_schematic_model.cpp

    canvas/test_frame_all.cpp
    canvas/test_palette_layout.cpp
    canvas/test_selection.cpp
    canvas/test_symbol_artwork.cpp
    canvas/test_symbol_parity.cpp
    canvas/test_symbol_render.cpp
    canvas/test_theme_parity.cpp

    session/test_circuit_session.cpp
    session/test_session_diagnosis.cpp

    panels/test_message_console.cpp
    panels/test_preset_library.cpp

    inspector/test_inspector_picker.cpp
    inspector/test_part_models.cpp
    inspector/test_spdt.cpp

    util/test_shortcut.cpp
    util/test_value_format.cpp

    test_scope_scale.cpp)

target_link_libraries(livewdf_editor_tests PRIVATE
    LiveWDF_EditorCore
    juce::juce_graphics
    juce::juce_gui_basics
)
target_include_directories(livewdf_editor_tests SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/catch2"
    "${CMAKE_CURRENT_SOURCE_DIR}/support"
)

add_test(NAME livewdf_editor_tests COMMAND livewdf_editor_tests)
