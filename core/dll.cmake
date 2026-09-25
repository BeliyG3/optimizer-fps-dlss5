enable_language(RC)
configure_file(core_version.rc.in core_version.rc @ONLY)
# This X-macro manifest is an include, not a linker module definition file.
set_source_files_properties(shaders/temporal_passes.def PROPERTIES HEADER_FILE_ONLY TRUE)
# Compile private copies with MT; never mix the public MD archives into this DLL.
set(_sdk_absolute)
set(_sdk_includes)
foreach(_sdk_target IN ITEMS ofps_sdk_core ofps_sdk_d3d12)
    get_target_property(_sdk_sources ${_sdk_target} SOURCES)
    get_target_property(_sdk_source_dir ${_sdk_target} SOURCE_DIR)
    get_target_property(_sdk_target_includes ${_sdk_target} INTERFACE_INCLUDE_DIRECTORIES)
    foreach(_source IN LISTS _sdk_sources)
        cmake_path(ABSOLUTE_PATH _source BASE_DIRECTORY "${_sdk_source_dir}"
            OUTPUT_VARIABLE _absolute)
        list(APPEND _sdk_absolute "${_absolute}")
    endforeach()
    list(APPEND _sdk_includes ${_sdk_target_includes})
endforeach()
list(REMOVE_DUPLICATES _sdk_absolute)
list(REMOVE_DUPLICATES _sdk_includes)
add_library(ofps_core_sdk_mt STATIC ${_sdk_absolute})
target_include_directories(ofps_core_sdk_mt PUBLIC ${_sdk_includes})
target_compile_features(ofps_core_sdk_mt PUBLIC cxx_std_20)
target_compile_options(ofps_core_sdk_mt PRIVATE /W4 /WX /EHsc /permissive-)
set_target_properties(ofps_core_sdk_mt PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_library(ofps_core_dll SHARED ${OFPS_CORE_SOURCES}
    core_exports.def "${CMAKE_CURRENT_BINARY_DIR}/core_version.rc")
set_target_properties(ofps_core_dll PROPERTIES PREFIX ""
    OUTPUT_NAME "optimizer-fps-dlss5-core"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_features(ofps_core_dll PUBLIC cxx_std_20)
target_compile_options(ofps_core_dll PRIVATE /W4 /WX /EHsc /permissive-)
target_compile_definitions(ofps_core_dll PRIVATE WIN32_LEAN_AND_MEAN= NOMINMAX=)
target_include_directories(ofps_core_dll PRIVATE
    "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/sdk"
    "${PROJECT_SOURCE_DIR}/sdk/include" "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(ofps_core_dll PRIVATE ofps_core_sdk_mt d3d12 dxgi)
install(TARGETS ofps_core_dll RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
