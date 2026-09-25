add_library(ofps_sdk_d3d11 STATIC
    d3d11_adapter.cpp
    d3d11_adapter_resources.cpp
    d3d11_adapter_record.cpp)
add_library(OptimizerFps::SdkD3D11 ALIAS ofps_sdk_d3d11)
set_target_properties(ofps_sdk_d3d11 PROPERTIES EXPORT_NAME SdkD3D11)
target_compile_features(ofps_sdk_d3d11 PUBLIC cxx_std_20)
target_include_directories(ofps_sdk_d3d11 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/sdk/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(ofps_sdk_d3d11 PUBLIC OptimizerFps::SdkCore d3d11 dxgi)
if(MSVC)
    target_compile_options(ofps_sdk_d3d11 PRIVATE /W4 /permissive-)
endif()
