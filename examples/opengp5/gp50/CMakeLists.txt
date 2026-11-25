add_executable(livewdf_codegen_dod250 EXCLUDE_FROM_ALL generate.cpp)
target_link_libraries(livewdf_codegen_dod250 PRIVATE livewdf livewdf_options)
add_executable(livewdf_codegen_microamp EXCLUDE_FROM_ALL generate_microamp.cpp)
target_link_libraries(livewdf_codegen_microamp PRIVATE livewdf livewdf_options)

set(schematic "${PROJECT_SOURCE_DIR}/schematics/pedals_fx/DOD 250 Overdrive Preamp Gray.livewdf")
set(microamp "${PROJECT_SOURCE_DIR}/schematics/pedals_fx/MXR Micro Amp.livewdf")
set(firmware "${CMAKE_CURRENT_BINARY_DIR}/livewdf-gp50")
set(header "${firmware}/dsp/circuit.h")
set(microamp_header "${firmware}/dsp/microamp_coefficients.h")
foreach(file IN ITEMS firmware.json params.h dsp/firmware.h dsp/wdf_solver.h dsp/microamp.h dsp/effects.h nds32/firmware.c)
    list(APPEND firmware_sources "${CMAKE_CURRENT_SOURCE_DIR}/firmware/${file}")
    list(APPEND firmware_copies "${firmware}/${file}")
endforeach()
add_custom_command(
    OUTPUT "${header}" "${microamp_header}" ${firmware_copies}
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/firmware" "${firmware}"
    COMMAND livewdf_codegen_dod250 "${schematic}" "${header}"
    COMMAND livewdf_codegen_microamp "${microamp}" "${microamp_header}"
    DEPENDS livewdf_codegen_dod250 livewdf_codegen_microamp "${schematic}" "${microamp}" ${firmware_sources}
    VERBATIM
)
add_custom_target(livewdf_opengp5_gp50 DEPENDS "${header}" "${microamp_header}" ${firmware_copies})

if(LIVEWDF_BUILD_TESTS)
    add_executable(livewdf_dod250_parity parity.cpp)
    target_link_libraries(livewdf_dod250_parity PRIVATE livewdf livewdf_options)
    target_include_directories(livewdf_dod250_parity PRIVATE "${firmware}")
    add_dependencies(livewdf_dod250_parity livewdf_opengp5_gp50)
    add_test(NAME livewdf_dod250_parity COMMAND livewdf_dod250_parity "${schematic}")
    add_executable(livewdf_microamp_parity parity_microamp.cpp)
    target_link_libraries(livewdf_microamp_parity PRIVATE livewdf livewdf_options)
    target_include_directories(livewdf_microamp_parity PRIVATE "${firmware}")
    add_dependencies(livewdf_microamp_parity livewdf_opengp5_gp50)
    add_test(NAME livewdf_microamp_parity COMMAND livewdf_microamp_parity "${microamp}")
endif()
