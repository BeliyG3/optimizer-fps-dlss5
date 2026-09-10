add_library(peripheral_warp_d3d12 STATIC d3d12_adapter.cpp)
add_library(PeripheralWarp::D3D12 ALIAS peripheral_warp_d3d12)
set_target_properties(peripheral_warp_d3d12 PROPERTIES EXPORT_NAME D3D12)
target_compile_features(peripheral_warp_d3d12 PUBLIC cxx_std_20)
target_include_directories(peripheral_warp_d3d12 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(peripheral_warp_d3d12 PUBLIC PeripheralWarp::Core d3d12 dxgi)
if(MSVC)
    target_compile_options(peripheral_warp_d3d12 PRIVATE /W4 /permissive-)
endif()
