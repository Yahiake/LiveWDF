cmake_minimum_required(VERSION 3.22)

option(LIVEWDF_VST3_MANIFEST "Write the VST3 moduleinfo.json, which loads the built plugin and so needs a native build" ON)

# ==============================================================================
# LiveWDF Audio Plugin Target (VST3 & Standalone)
# ==============================================================================

if(ANDROID)
    # Gradle supplies the Android app bundle. The library exposes JUCE's app
    # entry point and shares the processor and editor with the desktop plugin.
    juce_add_gui_app(LiveWDF_Plugin PRODUCT_NAME "LiveWDF" BUNDLE_ID "com.yahia.livewdf")
    set_target_properties(LiveWDF_Plugin PROPERTIES OUTPUT_NAME juce_jni)
    set(android_standalone ../android/native/AndroidStandalone.cpp)
    target_sources(LiveWDF_Plugin PRIVATE "${android_standalone}")
    set_property(SOURCE "${android_standalone}" PROPERTY COMPILE_DEFINITIONS
        JUCE_CORE_INCLUDE_JNI_HELPERS=1 LIVEWDF_VERSION_STRING="${PROJECT_VERSION}")

    option(LIVEWDF_ENABLE_ANDROID_USB "Build the direct Android USB audio backend" ON)
    set(LIVEWDF_ANDROID_USB_SOURCE_DIR "" CACHE PATH "Local checkout of the direct Android USB audio driver")
    if(LIVEWDF_ENABLE_ANDROID_USB)
        if(LIVEWDF_ANDROID_USB_SOURCE_DIR)
            add_subdirectory("${LIVEWDF_ANDROID_USB_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/android-usb")
        else()
            include(FetchContent)
            FetchContent_Declare(android_usb_audio_source
                GIT_REPOSITORY https://github.com/jopdorp/android-usb-audio.git
                GIT_TAG 61a121ab324751d5e4e97f00bcbc0198e0bc1d9c)
            FetchContent_MakeAvailable(android_usb_audio_source)
        endif()
        target_link_libraries(LiveWDF_Plugin PRIVATE android_usb_audio)
        set_property(SOURCE "${android_standalone}" APPEND PROPERTY COMPILE_DEFINITIONS LIVEWDF_ANDROID_USB=1)
    endif()
else()
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
endif()

file(GLOB factory_impulse_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/ir/cabinet/*.wav"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/ir/cabinet/*.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/ir/cabinet/*.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/ir/reverb/*.flac"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/ir/reverb/*.md")
juce_add_binary_data(LiveWDF_FactoryImpulseData
    HEADER_NAME FactoryImpulseData.h
    NAMESPACE FactoryImpulseData
    SOURCES ${factory_impulse_files})

target_sources(LiveWDF_Plugin PRIVATE
    source/PluginProcessor.cpp
    source/PluginProcessorProcess.cpp
    source/PluginProcessor.h
    source/OutputEffects.cpp
    source/OutputEffects.h
    source/SplitReverb.cpp
    source/SplitReverb.h
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
    LiveWDF_EditorCore
    LiveWDF_FactoryImpulseData
    livewdf
    livewdf_options
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_basics
    juce::juce_gui_extra
)

if(ANDROID)
    return()
endif()

if(LIVEWDF_BUILD_TESTS)
    add_executable(livewdf_split_reverb_test tests/test_split_reverb.cpp source/SplitReverb.cpp)
    target_compile_definitions(livewdf_split_reverb_test PRIVATE JUCE_USE_CURL=0)
    target_link_libraries(livewdf_split_reverb_test PRIVATE livewdf_options juce::juce_dsp)
    add_test(NAME livewdf_split_reverb COMMAND livewdf_split_reverb_test)
endif()

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

if(LIVEWDF_BUILD_TESTS)
    add_test(NAME livewdf_output_effects_state COMMAND LiveWDF_PluginBench --check-output-effects-state)
    add_test(NAME livewdf_factory_output_irs COMMAND LiveWDF_PluginBench
        "${CMAKE_CURRENT_SOURCE_DIR}/../schematics/amps/Orange Rockerverb 50w - Dirty Channel (Preamp only).livewdf"
        --check-factory-output-irs)
    add_test(NAME livewdf_preview_pad COMMAND LiveWDF_PluginBench
        "${CMAKE_CURRENT_SOURCE_DIR}/../schematics/studio/eq/Neve 1081 Gold Label Equalizer.livewdf"
        --check-preview-pad)
    add_test(NAME livewdf_external_impedance_defaults COMMAND LiveWDF_PluginBench
        --check-external-impedance-defaults)
    add_test(NAME livewdf_live_preview_capture COMMAND LiveWDF_PluginBench
        "${CMAKE_CURRENT_SOURCE_DIR}/../schematics/pedals_fx/Boss HM-2 Heavy Metal.livewdf"
        --check-live-preview-capture)
    add_test(NAME livewdf_live_preview_pot_motion COMMAND LiveWDF_PluginBench
        "${CMAKE_CURRENT_SOURCE_DIR}/../schematics/studio/eq/Neve 1081 Gold Label Equalizer.livewdf"
        --check-live-preview-pot-motion)
    add_test(NAME livewdf_bench_preview_wav COMMAND LiveWDF_PluginBench
        "${CMAKE_CURRENT_SOURCE_DIR}/../schematics/pedals_fx/MXR Micro Amp.livewdf"
        --live-preview --wav "${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/guitarjam-di/strat-neck-middle-di.wav")
endif()
