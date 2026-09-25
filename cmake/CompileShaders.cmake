# Shader compilation.
#
# Both compilers can be pointed at explicitly; otherwise they are looked up in
# the installed Windows SDKs (fxc) and in $ENV{VULKAN_SDK}/Bin plus PATH (dxc):
#
#   -DOFPS_FXC_EXECUTABLE=C:/path/to/fxc.exe   (required: DXBC artefacts)
#   -DOFPS_DXC_EXECUTABLE=C:/path/to/dxc.exe   (optional: SPIR-V artefacts)

include(GNUInstallDirs)

set(_ofps_sdk_bin_roots)
file(GLOB _ofps_sdk_versions LIST_DIRECTORIES true "C:/Program Files (x86)/Windows Kits/10/bin/*")
list(SORT _ofps_sdk_versions COMPARE NATURAL ORDER DESCENDING)
foreach(_ofps_sdk_version IN LISTS _ofps_sdk_versions)
    if(IS_DIRECTORY "${_ofps_sdk_version}/x64")
        list(APPEND _ofps_sdk_bin_roots "${_ofps_sdk_version}/x64")
    endif()
endforeach()

find_program(OFPS_FXC_EXECUTABLE NAMES fxc.exe HINTS ${_ofps_sdk_bin_roots}
             DOC "Direct3D shader compiler (fxc.exe) used for the DXBC artefacts")
find_program(OFPS_DXC_EXECUTABLE NAMES dxc.exe HINTS "$ENV{VULKAN_SDK}/Bin"
             DOC "DirectX shader compiler (dxc.exe) used for the optional SPIR-V artefacts")

if(NOT OFPS_FXC_EXECUTABLE)
    message(FATAL_ERROR "fxc.exe was not found; set OFPS_FXC_EXECUTABLE")
endif()

set(OFPS_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${OFPS_SHADER_OUTPUT_DIR}")
set(_ofps_shader_sources
    "${PROJECT_SOURCE_DIR}/sdk/shaders/pack.hlsl"
    "${PROJECT_SOURCE_DIR}/sdk/shaders/unpack.hlsl"
    "${PROJECT_SOURCE_DIR}/sdk/shaders/preview.hlsl"
    "${PROJECT_SOURCE_DIR}/sdk/shaders/outline.hlsl"
    "${PROJECT_SOURCE_DIR}/sdk/shaders/fullscreen.hlsli"
    "${PROJECT_SOURCE_DIR}/sdk/shaders/ofps_common.hlsli"
    "${PROJECT_SOURCE_DIR}/sdk/shaders/ofps_pack.hlsli"
    "${PROJECT_SOURCE_DIR}/core/shaders/temporal.hlsl"
    "${PROJECT_SOURCE_DIR}/core/shaders/temporal_mapping.hlsli"
    "${PROJECT_SOURCE_DIR}/core/shaders/temporal_residual.hlsli"
    "${PROJECT_SOURCE_DIR}/core/shaders/temporal_reproject.hlsli"
    "${PROJECT_SOURCE_DIR}/core/shaders/temporal_cs.hlsl"
    "${PROJECT_SOURCE_DIR}/core/shaders/motion_smooth.hlsl"
    "${PROJECT_SOURCE_DIR}/core/shaders/warp_cs.hlsl")

# One manifest drives both products' temporal binaries and pipeline tables.
set(OFPS_TEMPORAL_SOURCE "${PROJECT_SOURCE_DIR}/core/shaders/temporal_cs.hlsl")
list(APPEND _ofps_shader_sources "${PROJECT_SOURCE_DIR}/core/shaders/temporal_passes.def" "${PROJECT_SOURCE_DIR}/core/shaders/temporal_layout.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/core/shaders/temporal_passes.def")
file(STRINGS "${PROJECT_SOURCE_DIR}/core/shaders/temporal_passes.def" _ofps_pass_rows REGEX "^PW_TEMPORAL_PASS")
foreach(_ofps_row IN LISTS _ofps_pass_rows)
    string(REGEX REPLACE "PW_TEMPORAL_PASS\\(([A-Za-z]+),.*" "\\1" _ofps_name "${_ofps_row}")
    list(APPEND OFPS_TEMPORAL_PASSES_NAMES "temporal_${_ofps_name}_cs")
    list(APPEND OFPS_TEMPORAL_PASSES_ENTRIES "CS${_ofps_name}")
endforeach()

function(ofps_compile_dxbc name source entry profile)
    set(output "${OFPS_SHADER_OUTPUT_DIR}/${name}.dxbc")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${OFPS_FXC_EXECUTABLE}" /nologo /Ges /WX /O3 /T "${profile}" /E "${entry}"
                ${ARGN} /I "${PROJECT_SOURCE_DIR}/sdk/shaders" /I "${PROJECT_SOURCE_DIR}/core/shaders" /Fo "${output}" "${source}"
        DEPENDS ${_ofps_shader_sources}
        VERBATIM)
    set_property(GLOBAL APPEND PROPERTY OFPS_COMPILED_SHADER_OUTPUTS "${output}")
endfunction()

ofps_compile_dxbc(fullscreen_vs "${PROJECT_SOURCE_DIR}/sdk/shaders/pack.hlsl" VSMain vs_5_0)
ofps_compile_dxbc(pack_ps "${PROJECT_SOURCE_DIR}/sdk/shaders/pack.hlsl" PSMain ps_5_0)
ofps_compile_dxbc(unpack_ps "${PROJECT_SOURCE_DIR}/sdk/shaders/unpack.hlsl" PSMain ps_5_0)
ofps_compile_dxbc(preview_ps "${PROJECT_SOURCE_DIR}/sdk/shaders/preview.hlsl" PSMain ps_5_0)
ofps_compile_dxbc(outline_ps "${PROJECT_SOURCE_DIR}/sdk/shaders/outline.hlsl" PSMain ps_5_0)
ofps_compile_dxbc(motion_smooth_cs "${PROJECT_SOURCE_DIR}/core/shaders/motion_smooth.hlsl" CSMain cs_5_0)
ofps_compile_dxbc(warp_pack_cs "${PROJECT_SOURCE_DIR}/core/shaders/warp_cs.hlsl" CSPack cs_5_0)
ofps_compile_dxbc(warp_unpack_cs "${PROJECT_SOURCE_DIR}/core/shaders/warp_cs.hlsl" CSUnpack cs_5_0)
foreach(_ofps_pass_name _ofps_pass_entry IN ZIP_LISTS OFPS_TEMPORAL_PASSES_NAMES OFPS_TEMPORAL_PASSES_ENTRIES)
    ofps_compile_dxbc("${_ofps_pass_name}" "${OFPS_TEMPORAL_SOURCE}" "${_ofps_pass_entry}" cs_5_0)
endforeach()
foreach(_ofps_model_pass IN ITEMS Refine FlowLuma)
    ofps_compile_dxbc("temporal_${_ofps_model_pass}Model_cs" "${OFPS_TEMPORAL_SOURCE}" "CS${_ofps_model_pass}" cs_5_0 /D PW_T_MODEL_GRID=1)
endforeach()

if(OFPS_DXC_EXECUTABLE)
    function(ofps_compile_spirv name source entry profile)
        set(output "${OFPS_SHADER_OUTPUT_DIR}/${name}.spv")
        add_custom_command(
            OUTPUT "${output}"
            # -Wno-ambig-lit-shift: dxc warns (and -WX fails) on `1 << ring` with an int literal;
            # the temporal shader is kept byte-identical with the OptiScaler fork's copy, which is
            # built with fxc alone, so the suffix cannot be added there.
            COMMAND "${OFPS_DXC_EXECUTABLE}" -spirv -fspv-target-env=vulkan1.1 -Ges -WX -O3 -Wno-ambig-lit-shift
                    -T "${profile}" -E "${entry}" ${ARGN} -I "${PROJECT_SOURCE_DIR}/sdk/shaders" -I "${PROJECT_SOURCE_DIR}/core/shaders"
                    -Fo "${output}" "${source}"
            DEPENDS ${_ofps_shader_sources}
            VERBATIM)
        set_property(GLOBAL APPEND PROPERTY OFPS_COMPILED_SHADER_OUTPUTS "${output}")
    endfunction()
    ofps_compile_spirv(fullscreen_vs "${PROJECT_SOURCE_DIR}/sdk/shaders/pack.hlsl" VSMain vs_6_0)
    ofps_compile_spirv(pack_ps "${PROJECT_SOURCE_DIR}/sdk/shaders/pack.hlsl" PSMain ps_6_0)
    ofps_compile_spirv(unpack_ps "${PROJECT_SOURCE_DIR}/sdk/shaders/unpack.hlsl" PSMain ps_6_0)
    ofps_compile_spirv(preview_ps "${PROJECT_SOURCE_DIR}/sdk/shaders/preview.hlsl" PSMain ps_6_0)
    ofps_compile_spirv(outline_ps "${PROJECT_SOURCE_DIR}/sdk/shaders/outline.hlsl" PSMain ps_6_0)
    ofps_compile_spirv(motion_smooth_cs "${PROJECT_SOURCE_DIR}/core/shaders/motion_smooth.hlsl" CSMain cs_6_0)
    ofps_compile_spirv(warp_pack_cs "${PROJECT_SOURCE_DIR}/core/shaders/warp_cs.hlsl" CSPack cs_6_0)
    ofps_compile_spirv(warp_unpack_cs "${PROJECT_SOURCE_DIR}/core/shaders/warp_cs.hlsl" CSUnpack cs_6_0)
    foreach(_ofps_pass_name _ofps_pass_entry IN ZIP_LISTS OFPS_TEMPORAL_PASSES_NAMES OFPS_TEMPORAL_PASSES_ENTRIES)
        ofps_compile_spirv("${_ofps_pass_name}" "${OFPS_TEMPORAL_SOURCE}" "${_ofps_pass_entry}" cs_6_0)
    endforeach()
    foreach(_ofps_model_pass IN ITEMS Refine FlowLuma)
        ofps_compile_spirv("temporal_${_ofps_model_pass}Model_cs" "${OFPS_TEMPORAL_SOURCE}" "CS${_ofps_model_pass}" cs_6_0 -DPW_T_MODEL_GRID=1)
    endforeach()
else()
    message(STATUS "dxc.exe was not found; SPIR-V artefacts will not be generated (set OFPS_DXC_EXECUTABLE to enable them)")
endif()

get_property(_ofps_outputs GLOBAL PROPERTY OFPS_COMPILED_SHADER_OUTPUTS)
add_custom_target(ofps_shaders ALL DEPENDS ${_ofps_outputs})

if(TARGET ofps_core_dll)
    get_property(_stage_outputs GLOBAL PROPERTY OFPS_COMPILED_SHADER_OUTPUTS)
    list(FILTER _stage_outputs INCLUDE REGEX "\\.dxbc$")
    add_custom_target(ofps_core_assets ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:ofps_core_dll>/optimizer-fps-dlss5"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_stage_outputs}
            "$<TARGET_FILE_DIR:ofps_core_dll>/optimizer-fps-dlss5"
        VERBATIM)
    add_dependencies(ofps_core_assets ofps_shaders)
    add_dependencies(ofps_core_dll ofps_core_assets)
    install(FILES ${_stage_outputs}
        DESTINATION "${CMAKE_INSTALL_BINDIR}/optimizer-fps-dlss5")
endif()

install(FILES ${_ofps_outputs} DESTINATION ${CMAKE_INSTALL_DATADIR}/optimizer-fps-sdk/shaders/bin)
