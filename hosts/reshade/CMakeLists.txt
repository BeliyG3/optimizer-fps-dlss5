if(OFPS_BUILD_RESHADE_ADDON)
add_subdirectory(ngx_forwarder)

# schema.cpp provides the POD settings schema table, not the core implementation.
set(OFPS_NGX_SHELL_SOURCES
    ${PROJECT_SOURCE_DIR}/core/api/ofps_core_loader.cpp
    ${PROJECT_SOURCE_DIR}/core/api/ofps_core_loader.h
    ${PROJECT_SOURCE_DIR}/core/settings/schema.cpp
    ${PROJECT_SOURCE_DIR}/hosts/common/ui_source.cpp
    direct_host.h direct_host.cpp
    model_host_ngx.h model_host_ngx.cpp
    menu_settings.h menu_settings.cpp
    menu_state.h menu_state.cpp
    menu_host_queue.h menu_host_queue.cpp menu_outstanding.h menu_outstanding.cpp
    menu_pipeline.h menu_pipeline.cpp menu_pipeline_host.cpp menu_pipeline_step.cpp menu_pipeline_gpu.cpp menu_pipeline_state.h menu_submit.cpp
    menu_marker.h menu_marker.cpp menu_dump.h menu_dump.cpp menu_dump_d3d11.cpp
    menu_bridge_d3d11.h menu_bridge_d3d11.cpp menu_bridge_debug.cpp menu_pipeline_d3d11.cpp menu_pipeline_d3d11_gpu.cpp
    menu_model_pass.cpp menu_core_pass.h menu_core_pass.cpp menu_core_flow.h menu_core_flow.cpp menu_flow_switch.h menu_flow_switch.cpp menu_guides.h menu_guides.cpp menu_gpu_time.cpp menu_params.h menu_params.cpp menu_colour.h menu_colour.cpp
    ngx_param_iface.h ngx_param_shim.h ngx_param_shim.cpp
    menu_param_book.h menu_param_book.cpp
    ngx_hook_api.cpp ngx_params.cpp ngx_forwarder_calls.cpp readable_guides.cpp queue_submit_hook.h queue_submit_hook.cpp
    ngx_hook_api.h shell_host.h shell_host.cpp ngx_params.h ngx_forwarder_calls.h readable_guides.h)
include(addon/sources.cmake)
add_library(peripheral_warp_reshade MODULE ${OFPS_NGX_SHELL_SOURCES} ${OFPS_ADDON_SOURCES})
target_compile_features(peripheral_warp_reshade PRIVATE cxx_std_20)
target_include_directories(peripheral_warp_reshade PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_BINARY_DIR}/generated
    ${PROJECT_SOURCE_DIR}/sdk/include
    ${PROJECT_SOURCE_DIR}/hosts/common
    ${OFPS_RESHADE_INCLUDE_DIR}
    ${OFPS_IMGUI_INCLUDE_DIR})
target_compile_definitions(peripheral_warp_reshade PRIVATE WIN32_LEAN_AND_MEAN= NOMINMAX= OFPS_UI_RESHADE=1)
target_link_libraries(peripheral_warp_reshade PRIVATE OptimizerFps::SdkCore OptimizerFps::SdkD3D12 kernel32 user32 version d3d11 d3d12
    OptimizerFps::Detours)
if(TARGET ofps_core_dll)
add_dependencies(peripheral_warp_reshade ofps_core_dll)
# Always refresh the runtime payload, even when only the core or shaders changed.
add_custom_target(ofps_reshade_core_payload ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:ofps_core_dll>"
        "$<TARGET_FILE_DIR:peripheral_warp_reshade>"
    VERBATIM)
add_dependencies(ofps_reshade_core_payload peripheral_warp_reshade ofps_core_dll)
if(TARGET ofps_shaders)
    get_property(_ofps_shell_shaders GLOBAL PROPERTY OFPS_COMPILED_SHADER_OUTPUTS)
    list(FILTER _ofps_shell_shaders INCLUDE REGEX "\\.dxbc$")
    add_custom_command(TARGET ofps_reshade_core_payload POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:peripheral_warp_reshade>/optimizer-fps-dlss5"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_ofps_shell_shaders}
            "$<TARGET_FILE_DIR:peripheral_warp_reshade>/optimizer-fps-dlss5"
        VERBATIM)
endif()
else()
    message(FATAL_ERROR "The ReShade core add-on requires ofps_core_dll; enable OFPS_BUILD_CORE")
endif()
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_ofps_addon_suffix ".addon64")
else()
    set(_ofps_addon_suffix ".addon32")
endif()
set_target_properties(peripheral_warp_reshade PROPERTIES
    PREFIX ""
    OUTPUT_NAME "optimizer-fps-dlss5"
    SUFFIX "${_ofps_addon_suffix}")
if(MSVC)
    # d3d12.dll is delay-loaded: the add-on's mere load then pulls nothing but kernel32/user32 into the game, like renodx.
    # D3D11 is used through host interfaces, so there is no DLL import to delay-load.
    target_link_options(peripheral_warp_reshade PRIVATE /DELAYLOAD:d3d12.dll)
    target_link_libraries(peripheral_warp_reshade PRIVATE delayimp)
endif()
if(MSVC)
    target_compile_options(peripheral_warp_reshade PRIVATE /W4 /WX /EHsc /permissive-)
endif()
endif()
