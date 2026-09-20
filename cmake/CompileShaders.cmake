# Shader compilation.
#
# Both compilers can be pointed at explicitly; otherwise they are looked up in
# the installed Windows SDKs (fxc) and in $ENV{VULKAN_SDK}/Bin plus PATH (dxc):
#
#   -DPW_FXC_EXECUTABLE=C:/path/to/fxc.exe   (required: DXBC artefacts)
#   -DPW_DXC_EXECUTABLE=C:/path/to/dxc.exe   (optional: SPIR-V artefacts)

include(GNUInstallDirs)

set(_pw_sdk_bin_roots)
file(GLOB _pw_sdk_versions LIST_DIRECTORIES true "C:/Program Files (x86)/Windows Kits/10/bin/*")
list(SORT _pw_sdk_versions COMPARE NATURAL ORDER DESCENDING)
foreach(_pw_sdk_version IN LISTS _pw_sdk_versions)
    if(IS_DIRECTORY "${_pw_sdk_version}/x64")
        list(APPEND _pw_sdk_bin_roots "${_pw_sdk_version}/x64")
    endif()
endforeach()

find_program(PW_FXC_EXECUTABLE NAMES fxc.exe HINTS ${_pw_sdk_bin_roots}
             DOC "Direct3D shader compiler (fxc.exe) used for the DXBC artefacts")
find_program(PW_DXC_EXECUTABLE NAMES dxc.exe HINTS "$ENV{VULKAN_SDK}/Bin"
             DOC "DirectX shader compiler (dxc.exe) used for the optional SPIR-V artefacts")

if(NOT PW_FXC_EXECUTABLE)
    message(FATAL_ERROR "fxc.exe was not found; set PW_FXC_EXECUTABLE")
endif()

set(PW_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${PW_SHADER_OUTPUT_DIR}")
set(_pw_shader_sources
    "${PROJECT_SOURCE_DIR}/shaders/pack.hlsl"
    "${PROJECT_SOURCE_DIR}/shaders/unpack.hlsl"
    "${PROJECT_SOURCE_DIR}/shaders/preview.hlsl"
    "${PROJECT_SOURCE_DIR}/shaders/outline.hlsl"
    "${PROJECT_SOURCE_DIR}/shaders/fullscreen.hlsli"
    "${PROJECT_SOURCE_DIR}/shaders/peripheral_warp_common.hlsli"
    "${PROJECT_SOURCE_DIR}/shaders/peripheral_warp_pack.hlsli"
    "${PROJECT_SOURCE_DIR}/shaders/temporal.hlsl"
    "${PROJECT_SOURCE_DIR}/shaders/temporal_cs.hlsl")

# One manifest drives both products' temporal binaries and pipeline tables.
set(PW_TEMPORAL_SOURCE "${PROJECT_SOURCE_DIR}/shaders/temporal_cs.hlsl")
list(APPEND _pw_shader_sources "${PROJECT_SOURCE_DIR}/shaders/temporal_passes.def" "${PROJECT_SOURCE_DIR}/shaders/temporal_layout.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/shaders/temporal_passes.def")
file(STRINGS "${PROJECT_SOURCE_DIR}/shaders/temporal_passes.def" _pw_pass_rows REGEX "^PW_TEMPORAL_PASS")
foreach(_pw_row IN LISTS _pw_pass_rows)
    string(REGEX REPLACE "PW_TEMPORAL_PASS\\(([A-Za-z]+),.*" "\\1" _pw_name "${_pw_row}")
    list(APPEND PW_TEMPORAL_PASSES_NAMES "temporal_${_pw_name}_cs")
    list(APPEND PW_TEMPORAL_PASSES_ENTRIES "CS${_pw_name}")
endforeach()

function(pw_compile_dxbc name source entry profile)
    set(output "${PW_SHADER_OUTPUT_DIR}/${name}.dxbc")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${PW_FXC_EXECUTABLE}" /nologo /Ges /WX /O3 /T "${profile}" /E "${entry}"
                /I "${PROJECT_SOURCE_DIR}/shaders" /Fo "${output}" "${source}"
        DEPENDS ${_pw_shader_sources}
        VERBATIM)
    set_property(GLOBAL APPEND PROPERTY PW_COMPILED_SHADER_OUTPUTS "${output}")
endfunction()

pw_compile_dxbc(fullscreen_vs "${PROJECT_SOURCE_DIR}/shaders/pack.hlsl" VSMain vs_5_0)
pw_compile_dxbc(pack_ps "${PROJECT_SOURCE_DIR}/shaders/pack.hlsl" PSMain ps_5_0)
pw_compile_dxbc(unpack_ps "${PROJECT_SOURCE_DIR}/shaders/unpack.hlsl" PSMain ps_5_0)
pw_compile_dxbc(preview_ps "${PROJECT_SOURCE_DIR}/shaders/preview.hlsl" PSMain ps_5_0)
pw_compile_dxbc(outline_ps "${PROJECT_SOURCE_DIR}/shaders/outline.hlsl" PSMain ps_5_0)
foreach(_pw_pass_name _pw_pass_entry IN ZIP_LISTS PW_TEMPORAL_PASSES_NAMES PW_TEMPORAL_PASSES_ENTRIES)
    pw_compile_dxbc("${_pw_pass_name}" "${PW_TEMPORAL_SOURCE}" "${_pw_pass_entry}" cs_5_0)
endforeach()

if(PW_DXC_EXECUTABLE)
    function(pw_compile_spirv name source entry profile)
        set(output "${PW_SHADER_OUTPUT_DIR}/${name}.spv")
        add_custom_command(
            OUTPUT "${output}"
            # -Wno-ambig-lit-shift: dxc warns (and -WX fails) on `1 << ring` with an int literal;
            # the temporal shader is kept byte-identical with the OptiScaler fork's copy, which is
            # built with fxc alone, so the suffix cannot be added there.
            COMMAND "${PW_DXC_EXECUTABLE}" -spirv -fspv-target-env=vulkan1.1 -Ges -WX -O3 -Wno-ambig-lit-shift
                    -T "${profile}" -E "${entry}" -I "${PROJECT_SOURCE_DIR}/shaders"
                    -Fo "${output}" "${source}"
            DEPENDS ${_pw_shader_sources}
            VERBATIM)
        set_property(GLOBAL APPEND PROPERTY PW_COMPILED_SHADER_OUTPUTS "${output}")
    endfunction()
    pw_compile_spirv(fullscreen_vs "${PROJECT_SOURCE_DIR}/shaders/pack.hlsl" VSMain vs_6_0)
    pw_compile_spirv(pack_ps "${PROJECT_SOURCE_DIR}/shaders/pack.hlsl" PSMain ps_6_0)
    pw_compile_spirv(unpack_ps "${PROJECT_SOURCE_DIR}/shaders/unpack.hlsl" PSMain ps_6_0)
    pw_compile_spirv(preview_ps "${PROJECT_SOURCE_DIR}/shaders/preview.hlsl" PSMain ps_6_0)
    pw_compile_spirv(outline_ps "${PROJECT_SOURCE_DIR}/shaders/outline.hlsl" PSMain ps_6_0)
    foreach(_pw_pass_name _pw_pass_entry IN ZIP_LISTS PW_TEMPORAL_PASSES_NAMES PW_TEMPORAL_PASSES_ENTRIES)
        pw_compile_spirv("${_pw_pass_name}" "${PW_TEMPORAL_SOURCE}" "${_pw_pass_entry}" cs_6_0)
    endforeach()
else()
    message(STATUS "dxc.exe was not found; SPIR-V artefacts will not be generated (set PW_DXC_EXECUTABLE to enable them)")
endif()

get_property(_pw_outputs GLOBAL PROPERTY PW_COMPILED_SHADER_OUTPUTS)
add_custom_target(peripheral_warp_shaders ALL DEPENDS ${_pw_outputs})
install(FILES ${_pw_outputs} DESTINATION ${CMAKE_INSTALL_DATADIR}/peripheral-warp/shaders/bin)
