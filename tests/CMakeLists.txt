add_executable(peripheral_warp_tests test_math.cpp)
target_link_libraries(peripheral_warp_tests PRIVATE PeripheralWarp::Core)
target_compile_features(peripheral_warp_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(peripheral_warp_tests PRIVATE /W4 /permissive-)
endif()
add_test(NAME peripheral_warp_math COMMAND peripheral_warp_tests)

add_executable(peripheral_warp_input_v2_tests test_input_v2.cpp)
target_link_libraries(peripheral_warp_input_v2_tests PRIVATE PeripheralWarp::Core)
target_compile_features(peripheral_warp_input_v2_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(peripheral_warp_input_v2_tests PRIVATE /W4 /WX /EHsc /permissive-)
endif()
add_test(NAME peripheral_warp_input_v2 COMMAND peripheral_warp_input_v2_tests)

add_executable(peripheral_warp_layout_v2_tests test_layout_v2.cpp)
target_link_libraries(peripheral_warp_layout_v2_tests PRIVATE PeripheralWarp::Core)
target_compile_features(peripheral_warp_layout_v2_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(peripheral_warp_layout_v2_tests PRIVATE /W4 /WX /EHsc /permissive-)
endif()
add_test(NAME peripheral_warp_layout_v2 COMMAND peripheral_warp_layout_v2_tests)

add_executable(peripheral_warp_fixture_tests test_fixtures.cpp)
target_link_libraries(peripheral_warp_fixture_tests PRIVATE PeripheralWarp::Core)
target_compile_features(peripheral_warp_fixture_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(peripheral_warp_fixture_tests PRIVATE /W4 /permissive-)
endif()
add_test(NAME peripheral_warp_fixtures COMMAND peripheral_warp_fixture_tests)

# The add-on's ini schema (adapters/reshade/addon/ini_schema.h) without ReShade: no reshade.hpp, no
# imgui, just the schema bound to an in-memory ini.
add_executable(peripheral_warp_addon_ini_tests test_addon_ini.cpp)
target_link_libraries(peripheral_warp_addon_ini_tests PRIVATE PeripheralWarp::Core)
target_include_directories(peripheral_warp_addon_ini_tests PRIVATE "${PROJECT_SOURCE_DIR}")
target_compile_features(peripheral_warp_addon_ini_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(peripheral_warp_addon_ini_tests PRIVATE /W4 /WX /EHsc /permissive-)
endif()
add_test(NAME peripheral_warp_addon_ini COMMAND peripheral_warp_addon_ini_tests)

if(WIN32 AND PW_BUILD_D3D_ADAPTERS AND PW_COMPILE_SHADERS)
    add_executable(peripheral_warp_d3d11_smoke test_d3d11.cpp)
    target_link_libraries(peripheral_warp_d3d11_smoke PRIVATE PeripheralWarp::D3D11)
    target_compile_features(peripheral_warp_d3d11_smoke PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(peripheral_warp_d3d11_smoke PRIVATE /W4 /WX /EHsc /permissive-)
    endif()
    add_dependencies(peripheral_warp_d3d11_smoke peripheral_warp_shaders)
    add_test(NAME peripheral_warp_d3d11_smoke
        COMMAND peripheral_warp_d3d11_smoke
            "${PW_SHADER_OUTPUT_DIR}/fullscreen_vs.dxbc"
            "${PW_SHADER_OUTPUT_DIR}/pack_ps.dxbc"
            "${PW_SHADER_OUTPUT_DIR}/unpack_ps.dxbc")

    add_executable(peripheral_warp_d3d12_smoke test_d3d12.cpp)
    target_link_libraries(peripheral_warp_d3d12_smoke PRIVATE PeripheralWarp::D3D12 dxgi)
    target_compile_features(peripheral_warp_d3d12_smoke PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(peripheral_warp_d3d12_smoke PRIVATE /W4 /WX /EHsc /permissive-)
    endif()
    add_dependencies(peripheral_warp_d3d12_smoke peripheral_warp_shaders)
    add_test(NAME peripheral_warp_d3d12_smoke
        COMMAND peripheral_warp_d3d12_smoke
            "${PW_SHADER_OUTPUT_DIR}/fullscreen_vs.dxbc"
            "${PW_SHADER_OUTPUT_DIR}/pack_ps.dxbc"
            "${PW_SHADER_OUTPUT_DIR}/unpack_ps.dxbc")
endif()
