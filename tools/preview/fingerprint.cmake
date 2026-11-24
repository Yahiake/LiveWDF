# A sidecar is valid only for the circuit equations that qualified it. Keep
# names relative and normalize CRLF so the fingerprint transfers across hosts.
file(GLOB_RECURSE preview_inputs CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    "src/circuit/*.h" "src/circuit/*.cpp" "src/devices/*.h" "src/devices/*.cpp"
    "src/dsp/*.h" "src/dsp/*.cpp" "src/format/*.h" "src/format/*.cpp"
    "src/spice/*.h" "src/spice/*.cpp" "src/wdf/*.h" "src/wdf/*.cpp")
list(SORT preview_inputs)
set(preview_source_index "")
foreach(input IN LISTS preview_inputs)
    set(path "${CMAKE_CURRENT_SOURCE_DIR}/${input}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(READ "${path}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    string(SHA256 digest "${contents}")
    string(APPEND preview_source_index "${input}:${digest}\n")
endforeach()
string(SHA256 LIVEWDF_PREVIEW_ENGINE_SHA256 "${preview_source_index}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/PreviewFingerprint.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated/livewdf/PreviewFingerprint.h" @ONLY)
target_include_directories(livewdf INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/generated")
