include(sources.cmake)
add_library(ofps_core STATIC ${OFPS_CORE_SOURCES})
add_library(OptimizerFps::Core ALIAS ofps_core)
target_compile_features(ofps_core PUBLIC cxx_std_20)
target_include_directories(ofps_core PUBLIC ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/sdk ${PROJECT_SOURCE_DIR}/sdk/include)
target_link_libraries(ofps_core PUBLIC OptimizerFps::SdkD3D12 d3d12 dxgi)
if(MSVC)
target_compile_options(ofps_core PRIVATE /W4 /WX /EHsc /permissive-)
endif()
target_include_directories(ofps_core PUBLIC ${PROJECT_SOURCE_DIR}/core/api)
# Match the shell's empty definitions when these public macros propagate to it.
target_compile_definitions(ofps_core PUBLIC WIN32_LEAN_AND_MEAN= NOMINMAX=)

target_include_directories(ofps_core PRIVATE "${PROJECT_BINARY_DIR}/generated")

include(dll.cmake)
