set(sim "${LIVEWDF_OPENGP5_EMULATOR}/platform/host/sim_dsp.c")
set(platform "${LIVEWDF_OPENGP5_EMULATOR}/opengp5/platform")
if(NOT EXISTS "${sim}" OR NOT EXISTS "${platform}/dsp/gp5_dsp.h")
    message(FATAL_ERROR "LIVEWDF_OPENGP5_EMULATOR needs an OpenGP5-Emulator checkout with its submodule")
endif()

add_executable(livewdf_gp50_catalog_gen EXCLUDE_FROM_ALL generate_catalog.cpp)
target_link_libraries(livewdf_gp50_catalog_gen PRIVATE livewdf livewdf_options)

set(catalog_entries
    "Micro Amp"       "pedals_fx/MXR Micro Amp"
    "API 312"         "studio/preamps/API 312 Preamp VP312 Rev A"
    "Rockerverb CL"   "amps/Orange Rockerverb 50w - Clean Channel"
    "TS808"           "pedals_fx/Ibanez TS_808"
    "DOD 250"         "pedals_fx/DOD 250 Overdrive Preamp Gray"
    "Rockerverb OD"   "amps/Orange Rockerverb 50w - Dirty Channel"
    "Guv'nor"         "pedals_fx/Marshall Guv'nor"
    "Squeezer"        "pedals_fx/Dan Armstrong Orange Squeezer"
    "Maag EQ4"        "studio/eq/Maag EQ4-500 Parallel Circuit Model"
    "Bluesbreaker"    "pedals_fx/Marshall Bluesbreaker Mk I"
    "Boss SD-1"       "pedals_fx/Boss SD-1 Super OverDrive"
    "Trident 80B"     "studio/eq/Trident Series 80B Four-Band EQ S800"
    "Klon Centaur"    "pedals_fx/Klon Centaur"
    "Pro Co RAT"      "pedals_fx/Pro Co RAT LM308"
    "Dual Rectifier"  "amps/Mesa Boogie Dual Rectifier"
    "Cry Baby"        "pedals_fx/Dunlop Cry Baby GCB-95"
    "Ampeg B-15N"     "amps/Ampeg B-15N Portaflex"
    "Hiwatt DR103"    "amps/Hiwatt DR103"
    "Orange OR120"    "amps/Orange OR120 Graphic MkII"
    "Princeton"      "amps/Fender Princeton Reverb AA1164"
    "Vox AC15"        "amps/Vox AC15 No. 3 - EF86 Normal"
    "ENGL E530"       "amps/ENGL E530 - Lead Hi Gain"
    "Twin Reverb"    "amps/Fender Twin Reverb AB763"
    "Tweed Deluxe"   "amps/Fender Tweed Deluxe 5E3 - Instrument"
    "JCM800 2203"     "amps/Marshall JCM800 2203 - High Input"
    "Roland JC-120"   "amps/Roland JC-120"
    "Cali76"          "pedals_fx/Origin Effects Cali76 FET Compressor (Circuit Approximation)"
    "Power Boost"     "pedals_fx/Colorsound Power Boost 18V"
    "Fuzzrite"        "pedals_fx/Mosrite Fuzzrite Silicon"
    "Harrison 32C"    "studio/eq/Harrison 32-Series 3232 Channel EQ"
    "Bassman 5F6-A"   "amps/Fender Bassman 5F6-A"
    "Fuzz Face"       "pedals_fx/Fuzz Face"
    "Nobels ODR-1"    "pedals_fx/Nobels ODR-1 Natural Overdrive"
    "MCI JH-400B"     "studio/preamps/MCI JH-400B Mic Preamp 43E241"
    "Peavey 5150"     "amps/Peavey 5150 - Lead"
    "Boss DS-1A"      "pedals_fx/Boss DS-1A Distortion"
    "Super Lead"     "amps/Marshall 1959 Super Lead - Jumped"
    "Boss BF-2"       "pedals_fx/Boss BF-2 Flanger"
    "Octavia"         "pedals_fx/Roger Mayer Octavia Transistor"
    "SPL Transient"  "studio/dynamics/SPL Transient Designer 9842 DET Model"
    "Sontec 432C"     "studio/eq/Sontec MES-432C-9 Circuit Model"
    "Laney GH100L"    "amps/Laney GH100L - Drive"
    "Electric Mistress" "pedals_fx/EHX Electric Mistress 9V"
    "SSL 4000 E"      "studio/eq/SSL 4000 E 82E242 Black EQ"
    "Deluxe Reverb"  "amps/Fender Deluxe Reverb AB763"
    "Tweed Champ"    "amps/Fender Tweed Champ 5F1 - Input 1"
    "Mark IIC+"       "amps/Mesa Boogie Mark IIC+ - Lead")
list(LENGTH catalog_entries catalog_entry_count)
math(EXPR catalog_count "${catalog_entry_count} / 2")
set(catalog_files)
set(catalog_args)
while(catalog_entries)
    list(POP_FRONT catalog_entries label path)
    set(path "${PROJECT_SOURCE_DIR}/schematics/${path}.livewdf")
    list(APPEND catalog_files "${path}")
    list(APPEND catalog_args "${label}" "${path}")
endwhile()

set(catalog "${CMAKE_CURRENT_BINARY_DIR}/livewdf-gp50-catalog")
set(catalog_sources nds32/firmware.c dsp/firmware.h dsp/effects.h)
set(catalog_outputs "${catalog}/catalog_config.h" "${catalog}/catalog_schematics.inc" "${catalog}/params.h")
set(catalog_dependencies)
foreach(file IN LISTS catalog_sources)
    list(APPEND catalog_outputs "${catalog}/${file}")
    list(APPEND catalog_dependencies "${CMAKE_CURRENT_SOURCE_DIR}/firmware/${file}")
endforeach()
set(catalog_manifest_source "${CMAKE_CURRENT_SOURCE_DIR}/firmware/firmware.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${catalog_manifest_source}")
file(READ "${catalog_manifest_source}" catalog_manifest)
string(JSON catalog_manifest SET "${catalog_manifest}" id "\"livewdf-gp50-catalog\"")
string(JSON catalog_manifest SET "${catalog_manifest}" description "\"${catalog_count} LiveWDF circuits for the GP-50 desktop emulator.\"")
file(GENERATE OUTPUT "${catalog}/firmware.json" CONTENT "${catalog_manifest}\n")

add_custom_command(OUTPUT ${catalog_outputs}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${catalog}/nds32" "${catalog}/dsp"
    COMMAND livewdf_gp50_catalog_gen "${catalog}" ${catalog_args}
    DEPENDS livewdf_gp50_catalog_gen ${catalog_files} ${catalog_dependencies}
    COMMAND_EXPAND_LISTS VERBATIM)
foreach(file IN LISTS catalog_sources)
    add_custom_command(OUTPUT "${catalog}/catalog_config.h" APPEND
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/firmware/${file}" "${catalog}/${file}")
endforeach()
add_custom_target(livewdf_gp50_catalog_sources DEPENDS ${catalog_outputs})

add_library(livewdf_gp50_catalog MODULE host_catalog.cpp "${sim}")
add_dependencies(livewdf_gp50_catalog livewdf_gp50_catalog_sources)
target_include_directories(livewdf_gp50_catalog PRIVATE "${catalog}/dsp" "${catalog}" "${platform}/dsp")
target_compile_definitions(livewdf_gp50_catalog PRIVATE GP5_HOST GP50_BOARD GP5_HAS_PARAMS SIM_LIVE)
target_compile_options(livewdf_gp50_catalog PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-builtin>)
target_link_libraries(livewdf_gp50_catalog PRIVATE livewdf)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(livewdf_gp50_catalog PRIVATE "LINKER:--exclude-libs,ALL")
endif()
set_target_properties(livewdf_gp50_catalog PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY "${catalog}"
    C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)

if(LIVEWDF_BUILD_TESTS)
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    foreach(test IN ITEMS catalog patches)
        add_test(NAME livewdf_gp50_${test}
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_${test}.py"
                    "${LIVEWDF_OPENGP5_EMULATOR}/tools/simulate.py" "${catalog}" "$<TARGET_FILE:livewdf_gp50_catalog>")
    endforeach()
endif()
