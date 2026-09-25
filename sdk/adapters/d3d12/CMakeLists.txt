add_library(ofps_sdk_d3d12 STATIC
    d3d12_adapter.cpp
    d3d12_adapter_resources.cpp
    d3d12_adapter_record.cpp)
add_library(OptimizerFps::SdkD3D12 ALIAS ofps_sdk_d3d12)
set_target_properties(ofps_sdk_d3d12 PROPERTIES EXPORT_NAME SdkD3D12)
target_compile_features(ofps_sdk_d3d12 PUBLIC cxx_std_20)
target_include_directories(ofps_sdk_d3d12 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/sdk/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(ofps_sdk_d3d12 PUBLIC OptimizerFps::SdkCore d3d12 dxgi)
if(MSVC)
    target_compile_options(ofps_sdk_d3d12 PRIVATE /W4 /permissive-)
endif()
