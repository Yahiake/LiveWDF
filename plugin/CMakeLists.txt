cmake_minimum_required(VERSION 3.22)

option(LIVEWDF_VST3_MANIFEST "Write the VST3 moduleinfo.json, which loads the built plugin and so needs a native build" ON)

# ==============================================================================
# LiveWDF Audio Plugin Target (VST3 & Standalone)
# ==============================================================================

juce_add_plugin(LiveWDF_Plugin
    COMPANY_NAME "Yahia Kemari"
    BUNDLE_ID "com.yahia.livewdf"
    PLUGIN_MANUFACTURER_CODE "LWDF"
    PLUGIN_CODE "LWDF"
    FORMATS VST3 Standalone
    PRODUCT_NAME "LiveWDF"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_AUTO_MANIFEST ${LIVEWDF_VST3_MANIFEST}
)

target_sources(LiveWDF_Plugin PRIVATE
    source/PreviewPlayback.cpp
    source/PluginProcessor.cpp
    source/PluginProcessorProcess.cpp
    source/PluginProcessor.h
    source/PluginEditor.cpp
    source/PluginEditor.h
    gui/VintageKnob.cpp
    gui/VintageKnob.h
    gui/DualOscilloscope.cpp
    gui/DualOscilloscope.h
)

target_compile_definitions(LiveWDF_Plugin PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    $<$<PLATFORM_ID:Linux>:JUCE_JACK=1>
)

target_include_directories(LiveWDF_Plugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/source"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)

# The schematic is not a plugin-specific drawing: it is the editor's canvas,
# linked in read-only. That is what keeps a file looking the same in both
# windows, and it removes the second renderer that used to drift.
target_link_libraries(LiveWDF_Plugin PRIVATE
    juce::juce_cryptography
    LiveWDF_EditorCore
    livewdf
    livewdf_options
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_basics
    juce::juce_gui_extra
)

# Manual instrument, not a test and not part of the default build: times the
# real processor's processBlock() path at several block sizes and oversampling
# factors. Build target `LiveWDF_PluginBench`, run it from the repository root.
juce_add_console_app(LiveWDF_PluginBench PRODUCT_NAME "LiveWDF Plugin Bench")
target_sources(LiveWDF_PluginBench PRIVATE bench_plugin.cpp)
target_include_directories(LiveWDF_PluginBench PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/source"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui"
    "${CMAKE_CURRENT_SOURCE_DIR}/../editor/gui"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_compile_definitions(LiveWDF_PluginBench PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)
target_link_libraries(LiveWDF_PluginBench PRIVATE
    LiveWDF_Plugin
    livewdf_options
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_basics
    juce::juce_gui_extra
)
