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
    "Rockerverb CL"   "amps/Orange Rockerverb 50w - Clean Channel (Preamp only)"
    "TS808"           "pedals_fx/Ibanez TS_808"
    "DOD 250"         "pedals_fx/DOD 250 Overdrive Preamp Gray"
    "Rockerverb OD"   "amps/Orange Rockerverb 50w - Dirty Channel (Preamp only)"
    "Guv'nor"         "pedals_fx/Marshall Guv'nor"
    "Squeezer"        "pedals_fx/Dan Armstrong Orange Squeezer"
    "Maag EQ4"        "studio/eq/Maag EQ4-500 Parallel Circuit Model"
    "Bluesbreaker"    "pedals_fx/Marshall Bluesbreaker Mk I"
    "Boss SD-1"       "pedals_fx/Boss SD-1 Super OverDrive"
    "Trident 80B"     "studio/eq/Trident Series 80B Four-Band EQ S800"
    "Klon Centaur"    "pedals_fx/Klon Centaur"
    "Pro Co RAT"      "pedals_fx/Pro Co RAT LM308"
    "Dual Rectifier"  "amps/Mesa Boogie Dual Rectifier (Preamp Only)"
    "Cry Baby"        "pedals_fx/Dunlop Cry Baby GCB-95")
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
string(JSON catalog_manifest SET "${catalog_manifest}" description "\"Sixteen LiveWDF circuits for the GP-50 desktop emulator.\"")
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
    add_test(NAME livewdf_gp50_catalog
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_catalog.py"
               "${LIVEWDF_OPENGP5_EMULATOR}/tools/simulate.py" "${catalog}" "$<TARGET_FILE:livewdf_gp50_catalog>")
endif()
