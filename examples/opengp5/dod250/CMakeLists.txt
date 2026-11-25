add_executable(livewdf_codegen_dod250 EXCLUDE_FROM_ALL generate.cpp)
target_link_libraries(livewdf_codegen_dod250 PRIVATE livewdf livewdf_options)

set(schematic "${PROJECT_SOURCE_DIR}/schematics/pedals_fx/DOD 250 Overdrive Preamp Gray.livewdf")
set(firmware "${CMAKE_CURRENT_BINARY_DIR}/livewdf-dod250")
set(header "${firmware}/dsp/circuit.h")
foreach(file IN ITEMS firmware.json params.h dsp/firmware.h dsp/wdf_solver.h nds32/firmware.c)
    list(APPEND firmware_sources "${CMAKE_CURRENT_SOURCE_DIR}/firmware/${file}")
    list(APPEND firmware_copies "${firmware}/${file}")
endforeach()
add_custom_command(
    OUTPUT "${header}" ${firmware_copies}
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/firmware" "${firmware}"
    COMMAND livewdf_codegen_dod250 "${schematic}" "${header}"
    DEPENDS livewdf_codegen_dod250 "${schematic}" ${firmware_sources}
    VERBATIM
)
add_custom_target(livewdf_opengp5_dod250 DEPENDS "${header}" ${firmware_copies})

if(LIVEWDF_BUILD_TESTS)
    add_executable(livewdf_dod250_parity parity.cpp)
    target_link_libraries(livewdf_dod250_parity PRIVATE livewdf livewdf_options)
    target_include_directories(livewdf_dod250_parity PRIVATE "${firmware}")
    add_dependencies(livewdf_dod250_parity livewdf_opengp5_dod250)
    add_test(NAME livewdf_dod250_parity COMMAND livewdf_dod250_parity "${schematic}")
endif()
