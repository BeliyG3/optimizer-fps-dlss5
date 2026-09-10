add_library(peripheral_warp_d3d11 STATIC d3d11_adapter.cpp)
add_library(PeripheralWarp::D3D11 ALIAS peripheral_warp_d3d11)
set_target_properties(peripheral_warp_d3d11 PROPERTIES EXPORT_NAME D3D11)
target_compile_features(peripheral_warp_d3d11 PUBLIC cxx_std_20)
target_include_directories(peripheral_warp_d3d11 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(peripheral_warp_d3d11 PUBLIC PeripheralWarp::Core d3d11 dxgi)
if(MSVC)
    target_compile_options(peripheral_warp_d3d11 PRIVATE /W4 /permissive-)
endif()
