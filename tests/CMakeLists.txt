cmake_minimum_required(VERSION 3.22)

# ------------------------------------------------------------------------------
# Test and benchmark support
#
# The test tree is grouped by the layer it exercises, mirroring `src/`:
#
#   smoke/        build and harness sanity
#   engine/       WDF engine contracts
#   devices/      device laws and models
#   spice/        netlist parser
#   circuit/      SPQR/tree prototypes
#   compiler/     netlist -> compiled circuit (no engine, no sample rate)
#   runtime/      runtime facade contracts and dense-engine parity
#   format/       document model, diagnostics, loading
#   integration/  cross-layer fixtures and oracles
#   bench/        manual instruments, never CTest gates
#   support/      shared fixtures and helpers
#   oracle/       generated/reference headers
#
# One binary is deliberate: Catch2 gives the filtering, and the engine tests
# must stay JUCE-free. The editor has its own binary under `tests/editor`.
# ------------------------------------------------------------------------------

add_library(livewdf_test_support INTERFACE)
target_include_directories(livewdf_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(livewdf_test_support INTERFACE livewdf livewdf_options)

add_executable(livewdf_tests
    main.cpp

    smoke/test_smoke.cpp
    smoke/test_runtime_kernels.cpp

    engine/test_engine_smoke.cpp
    engine/test_accounting.cpp
    engine/test_block_solve.cpp
    engine/test_core_contracts.cpp
    engine/test_nan_sentinel.cpp
    engine/test_elements_l_i.cpp
    engine/test_newton_solver.cpp
    engine/test_root_groups.cpp
    engine/test_root_quadric.cpp
    engine/test_varres.cpp
    engine/test_vcvs.cpp
    engine/test_opamp_engine.cpp

    devices/test_device_registry.cpp
    devices/test_diode.cpp
    devices/test_diode_pair_port.cpp
    devices/test_jfet.cpp
    devices/test_mosfet.cpp
    devices/test_pentode.cpp
    devices/test_pentode_contracts.cpp
    devices/test_device_jacobian_contracts.cpp
    devices/test_pot_and_diodes.cpp
    devices/test_transformer.cpp
    devices/test_triode_grid.cpp
    devices/test_new_presets.cpp
    devices/test_bjt.cpp
    devices/test_bjt_engine.cpp
    devices/test_bjt_reference.cpp
    devices/test_opamp.cpp
    devices/test_power_supply.cpp
    devices/test_omega.cpp
    dsp/test_bucket_brigade.cpp
    dsp/test_rms_detector.cpp
    dsp/test_halfband_oversampler.cpp

    spice/test_spice_parser.cpp
    spice/test_dense_diagnostics.cpp
    spice/test_root_transfer_diagnostics.cpp

    circuit/test_spqr_tree.cpp
    circuit/test_tree_executor.cpp
    circuit/test_signal_flow.cpp
    circuit/test_pot_contracts.cpp

    compiler/test_compiler.cpp
    runtime/test_runtime_parity.cpp
    runtime/test_runtime_allocation.cpp

    format/test_ac_source.cpp
    format/test_connectivity.cpp
    # `test_livewdf_file.cpp`, `test_reload.cpp`: deleted with the format's move to
    # geometry-only. Both fixtures wrote net names beside pins (`pins = {{"pos",
    # "in"}}`) and drew nothing, which is the one thing the format no longer
    # carries -- so the round trips they existed for have no v3 equivalent to
    # rewrite into, and a v2 fixture is refused rather than reconciled.
    format/test_lowpass_shapes.cpp
    format/test_terminals.cpp

    integration/test_circuits_extra.cpp
    integration/test_extended_circuits.cpp
    # `test_fuzz_face.cpp` and `fuzz_face_fixture.{h,cpp}` deleted: the embedded
    # sheet is a v2 document -- it carries `pins` maps and wire `net` names and
    # draws nothing at its own pin points -- and legacy documents are refused, not
    # reconciled. BJT coverage lives in `devices/test_bjt*.cpp`, which builds the
    # circuit directly.
    integration/test_oracle_ngspice.cpp
    integration/test_oracle_parts.cpp
    integration/test_oracle_ngspice_transistors.cpp
    integration/test_oracle_wdf_compiler.cpp)
target_link_libraries(livewdf_tests PRIVATE livewdf_test_support)
if(livewdf_avx2_kernels)
    target_compile_definitions(livewdf_tests PRIVATE LIVEWDF_BUILD_AVX2)
endif()
target_include_directories(livewdf_tests SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/catch2")

add_test(NAME livewdf_tests COMMAND livewdf_tests
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

# ------------------------------------------------------------------------------
# Manual instruments
#
# These are not registered with CTest. They answer performance questions, and a
# benchmark that becomes a pass/fail gate stops being a measurement.
# ------------------------------------------------------------------------------

add_executable(livewdf_bench_circuits bench/bench_circuits.cpp)
target_link_libraries(livewdf_bench_circuits PRIVATE livewdf_test_support)

add_executable(livewdf_bench bench/bench_stage.cpp)
target_link_libraries(livewdf_bench PRIVATE livewdf_test_support)

add_executable(livewdf_bench_floor bench/bench_floor.cpp)
target_link_libraries(livewdf_bench_floor PRIVATE livewdf_test_support)

add_executable(livewdf_bench_omega bench/bench_omega.cpp)
target_link_libraries(livewdf_bench_omega PRIVATE livewdf_test_support)

add_executable(livewdf_bench_tree bench/bench_tree.cpp)
target_link_libraries(livewdf_bench_tree PRIVATE livewdf_test_support)

add_executable(livewdf_bench_scaling bench/bench_scaling.cpp)
target_link_libraries(livewdf_bench_scaling PRIVATE livewdf_test_support)

add_executable(livewdf_bench_runtime bench/bench_runtime.cpp)
target_link_libraries(livewdf_bench_runtime PRIVATE livewdf_test_support)

add_executable(livewdf_bench_parts bench/bench_parts.cpp)
target_link_libraries(livewdf_bench_parts PRIVATE livewdf_test_support)

add_executable(livewdf_bench_semiconductors bench/bench_semiconductors.cpp)
target_link_libraries(livewdf_bench_semiconductors PRIVATE livewdf_test_support)
