if(WIN32)
    add_library(ofps_installer_reshade_stub SHARED installer/reshade_stub.cpp)
    set_target_properties(ofps_installer_reshade_stub PROPERTIES
        OUTPUT_NAME "reshade-installer-stub")
endif()

add_executable(ofps_sdk_math_tests test_math.cpp)
target_link_libraries(ofps_sdk_math_tests PRIVATE OptimizerFps::SdkCore)
target_compile_features(ofps_sdk_math_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(ofps_sdk_math_tests PRIVATE /W4 /permissive-)
endif()
add_test(NAME ofps_sdk_math COMMAND ofps_sdk_math_tests)

add_executable(ofps_sdk_input_v2_tests test_input_v2.cpp)
target_link_libraries(ofps_sdk_input_v2_tests PRIVATE OptimizerFps::SdkCore)
target_compile_features(ofps_sdk_input_v2_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(ofps_sdk_input_v2_tests PRIVATE /W4 /WX /EHsc /permissive-)
endif()
add_test(NAME ofps_sdk_input_v2 COMMAND ofps_sdk_input_v2_tests)

add_executable(ofps_sdk_layout_v2_tests test_layout_v2.cpp)
target_link_libraries(ofps_sdk_layout_v2_tests PRIVATE OptimizerFps::SdkCore)
target_compile_features(ofps_sdk_layout_v2_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(ofps_sdk_layout_v2_tests PRIVATE /W4 /WX /EHsc /permissive-)
endif()
add_test(NAME ofps_sdk_layout_v2 COMMAND ofps_sdk_layout_v2_tests)

add_executable(ofps_sdk_fixture_tests test_fixtures.cpp)
target_link_libraries(ofps_sdk_fixture_tests PRIVATE OptimizerFps::SdkCore)
target_compile_features(ofps_sdk_fixture_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(ofps_sdk_fixture_tests PRIVATE /W4 /permissive-)
endif()
add_test(NAME ofps_sdk_fixtures COMMAND ofps_sdk_fixture_tests)

# The add-on's ini schema (hosts/reshade/addon/ini_schema.h) without ReShade: no reshade.hpp, no
# imgui, just the schema bound to an in-memory ini.
add_executable(test_settings_schema_tests test_settings_schema.cpp test_ini_migration.cpp)
target_link_libraries(test_settings_schema_tests PRIVATE OptimizerFps::SdkCore)
target_include_directories(test_settings_schema_tests PRIVATE "${PROJECT_SOURCE_DIR}")
target_compile_features(test_settings_schema_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(test_settings_schema_tests PRIVATE /W4 /WX /EHsc /permissive-)
endif()
add_test(NAME test_settings_schema COMMAND test_settings_schema_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/reshade_ini")

add_executable(ofps_ui_source_tests test_ui_source.cpp
    ../hosts/common/ui_source.cpp ../core/settings/schema.cpp)
target_include_directories(ofps_ui_source_tests PRIVATE "${PROJECT_SOURCE_DIR}")
target_compile_features(ofps_ui_source_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(ofps_ui_source_tests PRIVATE /W4 /WX /EHsc /permissive-)
endif()
add_test(NAME test_ui_source COMMAND ofps_ui_source_tests)

add_executable(test_remote_v4_tests test_remote_v4.cpp test_remote_v4_link.cpp
    ../hosts/remote32/remote_link.cpp ../hosts/remote32/remote_ui_source.cpp
    ../core/settings/schema.cpp)
target_compile_definitions(test_remote_v4_tests PRIVATE OFPS_REMOTE_TEST=1)
target_include_directories(test_remote_v4_tests PRIVATE "${PROJECT_SOURCE_DIR}")
target_compile_features(test_remote_v4_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(test_remote_v4_tests PRIVATE /W4 /WX /EHsc /permissive-)
endif()
target_link_libraries(test_remote_v4_tests PRIVATE kernel32)
add_test(NAME test_remote_v4 COMMAND test_remote_v4_tests)
if(TARGET peripheral_warp_reshade AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(probe_setting_export probe_setting_export.cpp)
    target_include_directories(probe_setting_export PRIVATE "${PROJECT_SOURCE_DIR}")
    target_compile_features(probe_setting_export PRIVATE cxx_std_20)
    target_compile_options(probe_setting_export PRIVATE /W4 /WX /EHsc /permissive-)
    add_dependencies(probe_setting_export peripheral_warp_reshade)
    add_test(NAME test_setting_export COMMAND probe_setting_export
        "$<TARGET_FILE:peripheral_warp_reshade>")
endif()
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_executable(test_remote_v4_live test_remote_v4_live.cpp
        ../hosts/remote32/remote_link.cpp ../hosts/remote32/remote_ui_source.cpp
        ../core/settings/schema.cpp)
    target_include_directories(test_remote_v4_live PRIVATE "${PROJECT_SOURCE_DIR}")
    target_compile_definitions(test_remote_v4_live PRIVATE OFPS_REMOTE_TEST=1)
    target_compile_features(test_remote_v4_live PRIVATE cxx_std_20)
    target_compile_options(test_remote_v4_live PRIVATE /W4 /WX /EHsc /permissive-)
    target_link_libraries(test_remote_v4_live PRIVATE kernel32)
endif()

add_executable(ofps_temporal_phase_tests test_temporal_phase.cpp)
target_include_directories(ofps_temporal_phase_tests PRIVATE "${PROJECT_SOURCE_DIR}")
target_compile_features(ofps_temporal_phase_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(ofps_temporal_phase_tests PRIVATE /W4 /WX /EHsc /permissive-)
endif()
add_test(NAME ofps_temporal_phase COMMAND ofps_temporal_phase_tests)
if(TARGET ofps_core)
    add_executable(ofps_temporal_profile_tests test_temporal_profile.cpp)
    target_link_libraries(ofps_temporal_profile_tests PRIVATE ofps_core)
    target_compile_options(ofps_temporal_profile_tests PRIVATE /W4 /WX /EHsc /permissive-)
    add_test(NAME ofps_temporal_profile COMMAND ofps_temporal_profile_tests)
    add_executable(ofps_flow_tests test_flow.cpp)
    target_link_libraries(ofps_flow_tests PRIVATE ofps_core)
    target_compile_options(ofps_flow_tests PRIVATE /W4 /WX /EHsc /permissive-)
    add_test(NAME ofps_flow COMMAND ofps_flow_tests)
endif()

add_executable(ofps_warp_policy_tests test_warp_policy.cpp ../core/warp/path_policy.cpp)
target_include_directories(ofps_warp_policy_tests PRIVATE "${PROJECT_SOURCE_DIR}")
target_compile_features(ofps_warp_policy_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(ofps_warp_policy_tests PRIVATE /W4 /WX /EHsc /permissive-)
endif()
add_test(NAME ofps_warp_policy COMMAND ofps_warp_policy_tests)

if(WIN32 AND OFPS_BUILD_D3D_ADAPTERS AND OFPS_COMPILE_SHADERS)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(OFPS_DEPTH_SHADER_TEST_BIN "${CMAKE_CURRENT_BINARY_DIR}/test_depth_shader.dxbc")
    add_custom_command(OUTPUT "${OFPS_DEPTH_SHADER_TEST_BIN}"
        COMMAND "${OFPS_FXC_EXECUTABLE}" /nologo /T cs_5_0 /E TestDepth /O3 /WX
            /I "${PROJECT_SOURCE_DIR}/core/shaders"
            /I "${PROJECT_SOURCE_DIR}/sdk/shaders"
            /Fo "${OFPS_DEPTH_SHADER_TEST_BIN}"
            "${CMAKE_CURRENT_SOURCE_DIR}/test_depth_shader.hlsl"
        DEPENDS test_depth_shader.hlsl
            "${PROJECT_SOURCE_DIR}/core/shaders/temporal.hlsl"
            "${PROJECT_SOURCE_DIR}/core/shaders/temporal_mapping.hlsli"
            "${PROJECT_SOURCE_DIR}/core/shaders/temporal_reproject.hlsli"
        VERBATIM)
    add_custom_target(ofps_depth_shader_binary DEPENDS "${OFPS_DEPTH_SHADER_TEST_BIN}")
    add_executable(ofps_depth_shader_tests test_depth_shader.cpp)
    add_dependencies(ofps_depth_shader_tests ofps_depth_shader_binary)
    target_compile_features(ofps_depth_shader_tests PRIVATE cxx_std_20)
    target_compile_options(ofps_depth_shader_tests PRIVATE /W4 /WX /EHsc /permissive-)
    target_link_libraries(ofps_depth_shader_tests PRIVATE d3d11 d3dcompiler)
    add_test(NAME ofps_depth_shader COMMAND ofps_depth_shader_tests "${OFPS_DEPTH_SHADER_TEST_BIN}")
    add_test(NAME ofps_temporal_contract
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/temporal_contract.py"
            "${PROJECT_SOURCE_DIR}/core/shaders/temporal_passes.def" "${OFPS_SHADER_OUTPUT_DIR}" "${OFPS_FXC_EXECUTABLE}")
    add_executable(ofps_sdk_d3d11_smoke test_d3d11.cpp
        d3d11_cases_adapter.cpp d3d11_cases_fixtures.cpp)
    target_link_libraries(ofps_sdk_d3d11_smoke PRIVATE OptimizerFps::SdkD3D11)
    target_compile_features(ofps_sdk_d3d11_smoke PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(ofps_sdk_d3d11_smoke PRIVATE /W4 /WX /EHsc /permissive-)
    endif()
    add_dependencies(ofps_sdk_d3d11_smoke ofps_shaders)
    add_test(NAME ofps_sdk_d3d11_smoke
        COMMAND ofps_sdk_d3d11_smoke
            "${OFPS_SHADER_OUTPUT_DIR}/fullscreen_vs.dxbc"
            "${OFPS_SHADER_OUTPUT_DIR}/pack_ps.dxbc"
            "${OFPS_SHADER_OUTPUT_DIR}/unpack_ps.dxbc")

    add_executable(ofps_sdk_d3d12_smoke test_d3d12.cpp
        d3d12_cases_adapter.cpp d3d12_cases_fixtures.cpp d3d12_cases_roundtrip.cpp)
    target_link_libraries(ofps_sdk_d3d12_smoke PRIVATE OptimizerFps::SdkD3D12 dxgi)
    target_compile_features(ofps_sdk_d3d12_smoke PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(ofps_sdk_d3d12_smoke PRIVATE /W4 /WX /EHsc /permissive-)
    endif()
    add_dependencies(ofps_sdk_d3d12_smoke ofps_shaders)
    add_test(NAME ofps_sdk_d3d12_smoke
        COMMAND ofps_sdk_d3d12_smoke
            "${OFPS_SHADER_OUTPUT_DIR}/fullscreen_vs.dxbc"
            "${OFPS_SHADER_OUTPUT_DIR}/pack_ps.dxbc"
            "${OFPS_SHADER_OUTPUT_DIR}/unpack_ps.dxbc")
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME ofps_compare_reference COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/bench/test_compare_reference.py")
set_tests_properties(ofps_compare_reference PROPERTIES SKIP_RETURN_CODE 77)

if(TARGET ofps_core)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME ofps_core_includes COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check-core-includes.py" "${PROJECT_SOURCE_DIR}")
endif()

if(TARGET ofps_core)
add_executable(ofps_settings_values_tests test_settings_values.cpp test_abi_c.c)
target_link_libraries(ofps_settings_values_tests PRIVATE ofps_core)
# D3D12's anonymous C structs/unions require C11 under /W4 /WX.
target_compile_features(ofps_settings_values_tests PRIVATE c_std_11 cxx_std_20)
target_compile_options(ofps_settings_values_tests PRIVATE /W4 /WX)
add_test(NAME ofps_settings_values COMMAND ofps_settings_values_tests)
endif()

if(TARGET ofps_core)
add_executable(ofps_gpu_pools_tests test_gpu_pools.cpp test_compute_pipeline.cpp)
target_link_libraries(ofps_gpu_pools_tests PRIVATE ofps_core d3d12 dxgi d3dcompiler)
target_compile_options(ofps_gpu_pools_tests PRIVATE /W4 /WX)
if(TARGET ofps_shaders)
    add_dependencies(ofps_gpu_pools_tests ofps_shaders)
    add_test(NAME ofps_gpu_pools COMMAND ofps_gpu_pools_tests "${OFPS_SHADER_OUTPUT_DIR}")
else()
    add_test(NAME ofps_gpu_pools COMMAND ofps_gpu_pools_tests)
endif()
endif()

if(TARGET ofps_core)
add_executable(ofps_model_host_ngx_tests test_model_host_ngx.cpp ../hosts/reshade/model_host_ngx.cpp ../hosts/reshade/ngx_params.cpp)
target_link_libraries(ofps_model_host_ngx_tests PRIVATE ofps_core d3d12 dxgi)
target_compile_options(ofps_model_host_ngx_tests PRIVATE /W4 /WX)
add_test(NAME ofps_model_host_ngx COMMAND ofps_model_host_ngx_tests)
endif()

if(TARGET ofps_core)
add_executable(ofps_readable_guides_tests test_readable_guides.cpp
    ../hosts/reshade/readable_guides.cpp ../hosts/reshade/ngx_params.cpp)
target_include_directories(ofps_readable_guides_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/sdk/include")
target_compile_features(ofps_readable_guides_tests PRIVATE cxx_std_20)
target_compile_definitions(ofps_readable_guides_tests PRIVATE WIN32_LEAN_AND_MEAN= NOMINMAX=)
target_compile_options(ofps_readable_guides_tests PRIVATE /W4 /WX /EHsc /permissive-)
target_link_libraries(ofps_readable_guides_tests PRIVATE ofps_core d3d12 dxgi d3dcompiler)
add_test(NAME ofps_readable_guides COMMAND ofps_readable_guides_tests)
endif()

if(TARGET ofps_core)
    add_executable(ofps_ngx_params_tests test_ngx_params.cpp ../hosts/reshade/ngx_params.cpp)
    target_link_libraries(ofps_ngx_params_tests PRIVATE ofps_core d3d12 dxgi)
    target_compile_options(ofps_ngx_params_tests PRIVATE /W4 /WX)
    add_test(NAME ofps_ngx_params COMMAND ofps_ngx_params_tests)
    add_executable(ofps_host_shape_tests test_host_shape.cpp)
    target_link_libraries(ofps_host_shape_tests PRIVATE ofps_core d3d12 dxgi)
    target_compile_options(ofps_host_shape_tests PRIVATE /W4 /WX)
    add_test(NAME ofps_host_shape COMMAND ofps_host_shape_tests)
endif()

if(TARGET ofps_core)
add_executable(ofps_core_impl_tests test_core_impl.cpp)
target_link_libraries(ofps_core_impl_tests PRIVATE ofps_core d3d12 dxgi)
target_compile_options(ofps_core_impl_tests PRIVATE /W4 /WX)
add_test(NAME ofps_core_impl COMMAND ofps_core_impl_tests)
target_link_libraries(test_settings_schema_tests PRIVATE ofps_core)
target_compile_definitions(test_settings_schema_tests PRIVATE OFPS_TEST_CORE=1)
else()
target_sources(test_settings_schema_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/settings/schema.cpp)
endif()

if(TARGET ofps_core)
set(OFPS_CORE_API_TEST_SOURCES
    test_core_api.cpp test_core_api_scenarios.cpp test_core_api_compute.cpp test_core_api_half_ulp.cpp
    test_core_api_entry.h
    test_core_api_checks.h test_core_api_codec.h test_core_api_fakes.h
    test_core_api_frame.h test_core_api_gpu.h test_core_api_pattern.h test_core_api_reference.h test_core_api_scenarios.h)
add_executable(ofps_core_api_tests ${OFPS_CORE_API_TEST_SOURCES})
target_sources(ofps_core_api_tests PRIVATE test_core_api_compute_direct.cpp)
target_link_libraries(ofps_core_api_tests PRIVATE ofps_core OptimizerFps::SdkCore d3d12 dxgi d3dcompiler)
target_include_directories(ofps_core_api_tests PRIVATE "${PROJECT_SOURCE_DIR}")
target_compile_features(ofps_core_api_tests PRIVATE cxx_std_20)
if(MSVC)
target_compile_options(ofps_core_api_tests PRIVATE /W4 /WX /EHsc /permissive-)
endif()
add_dependencies(ofps_core_api_tests ofps_shaders)
add_custom_command(TARGET ofps_core_api_tests POST_BUILD COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:ofps_core_api_tests>/optimizer-fps-dlss5" COMMAND "${CMAKE_COMMAND}" -E copy_directory "${OFPS_SHADER_OUTPUT_DIR}" "$<TARGET_FILE_DIR:ofps_core_api_tests>/optimizer-fps-dlss5" VERBATIM)
add_test(NAME ofps_core_api COMMAND ofps_core_api_tests)
endif()
if(TARGET ofps_core_dll)
    add_executable(ofps_core_api_dll_tests ${OFPS_CORE_API_TEST_SOURCES})
    target_compile_definitions(ofps_core_api_dll_tests PRIVATE OFPS_TEST_DLL=1
        WIN32_LEAN_AND_MEAN= NOMINMAX=)
    target_include_directories(ofps_core_api_dll_tests PRIVATE "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/sdk" "${PROJECT_SOURCE_DIR}/sdk/include")
    target_compile_features(ofps_core_api_dll_tests PRIVATE cxx_std_20)
    target_compile_options(ofps_core_api_dll_tests PRIVATE /W4 /WX /EHsc /permissive-)
    target_link_libraries(ofps_core_api_dll_tests PRIVATE
        OptimizerFps::SdkCore d3d12 dxgi d3dcompiler)
    add_dependencies(ofps_core_api_dll_tests ofps_core_dll)
    if(TARGET ofps_core_assets)
        add_dependencies(ofps_core_api_dll_tests ofps_core_assets)
    endif()
    add_test(NAME ofps_core_api_dll COMMAND ofps_core_api_dll_tests
        "$<TARGET_FILE:ofps_core_dll>")
endif()
if(TARGET ofps_core_dll)
    add_library(ofps_core_wrong_version SHARED
        fixtures/core_wrong_version.cpp ../core/core_exports.def)
    set_target_properties(ofps_core_wrong_version PROPERTIES
        PREFIX "" OUTPUT_NAME "optimizer-fps-dlss5-core"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/wrong-version")
    target_include_directories(ofps_core_wrong_version PRIVATE "${PROJECT_SOURCE_DIR}")
    target_compile_features(ofps_core_wrong_version PRIVATE cxx_std_20)
    target_compile_options(ofps_core_wrong_version PRIVATE /W4 /WX /EHsc /permissive-)

    add_library(ofps_core_no_exports SHARED fixtures/core_no_exports.cpp)
    set_target_properties(ofps_core_no_exports PROPERTIES
        PREFIX "" OUTPUT_NAME "optimizer-fps-dlss5-core"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/no-exports")
    target_compile_options(ofps_core_no_exports PRIVATE /W4 /WX /EHsc /permissive-)

    add_executable(ofps_core_loader_tests test_core_loader.cpp ../core/api/ofps_core_loader.cpp)
    target_include_directories(ofps_core_loader_tests PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}/generated")
    target_compile_features(ofps_core_loader_tests PRIVATE cxx_std_20)
    target_compile_definitions(ofps_core_loader_tests PRIVATE WIN32_LEAN_AND_MEAN= NOMINMAX=)
    target_compile_options(ofps_core_loader_tests PRIVATE /W4 /WX /EHsc /permissive-)
    target_link_libraries(ofps_core_loader_tests PRIVATE kernel32)
    add_dependencies(ofps_core_loader_tests ofps_core_dll ofps_core_wrong_version ofps_core_no_exports)
    add_test(NAME ofps_core_loader COMMAND ofps_core_loader_tests
        "$<TARGET_FILE:ofps_core_dll>" "$<TARGET_FILE:ofps_core_wrong_version>" "$<TARGET_FILE:ofps_core_no_exports>")
endif()

if(TARGET ofps_core)
    add_executable(ofps_direct_host_tests test_direct_host.cpp ../hosts/reshade/direct_host.cpp)
    target_include_directories(ofps_direct_host_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_compile_features(ofps_direct_host_tests PRIVATE cxx_std_20)
    target_compile_options(ofps_direct_host_tests PRIVATE /W4 /WX /EHsc /permissive-)
    target_link_libraries(ofps_direct_host_tests PRIVATE kernel32)
    add_test(NAME ofps_direct_host COMMAND ofps_direct_host_tests)
endif()
