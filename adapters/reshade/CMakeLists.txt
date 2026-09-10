if(PW_BUILD_RESHADE_ADDON)
add_subdirectory(ngx_forwarder)

include(ngx/sources.cmake)
include(addon/sources.cmake)
add_library(peripheral_warp_reshade MODULE ${PW_NGX_SOURCES} ${PW_ADDON_SOURCES})
target_compile_features(peripheral_warp_reshade PRIVATE cxx_std_20)
target_include_directories(peripheral_warp_reshade PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/adapters
    ${PW_RESHADE_INCLUDE_DIR}
    ${PW_IMGUI_INCLUDE_DIR})
target_link_libraries(peripheral_warp_reshade PRIVATE PeripheralWarp::Core PeripheralWarp::D3D11 PeripheralWarp::D3D12 kernel32 user32 d3d11 d3d12
    PeripheralWarp::Detours)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_pw_addon_suffix ".addon64")
else()
    set(_pw_addon_suffix ".addon32")
endif()
set_target_properties(peripheral_warp_reshade PROPERTIES
    PREFIX ""
    OUTPUT_NAME "optimizer-fps-dlss5"
    SUFFIX "${_pw_addon_suffix}")
if(MSVC)
    # d3d12.dll is delay-loaded: the add-on's mere load then pulls nothing but kernel32/user32 into the game, like renodx.
    target_link_options(peripheral_warp_reshade PRIVATE /DELAYLOAD:d3d12.dll /DELAYLOAD:d3d11.dll)
    target_link_libraries(peripheral_warp_reshade PRIVATE delayimp)
endif()
if(MSVC)
    target_compile_options(peripheral_warp_reshade PRIVATE /W4 /WX /EHsc /permissive-)
endif()
endif()

# Remote overlay for 32-bit games (stage 26.11): no renderer, no SDK core, no Detours - only the
# ReShade API and ImGui headers. It draws the PeripheralWarp tab inside the game process and talks
# to optimizer-fps-dlss5.addon64 in the 64-bit Neural Rendering host through a shared memory block.
if(PW_BUILD_RESHADE_REMOTE32)
    add_library(peripheral_warp_reshade_remote MODULE
        producer_remote.cpp
        pw_remote_ipc.h)
    target_compile_features(peripheral_warp_reshade_remote PRIVATE cxx_std_20)
    target_include_directories(peripheral_warp_reshade_remote PRIVATE
        ${PW_RESHADE_INCLUDE_DIR}
        ${PW_IMGUI_INCLUDE_DIR})
    target_link_libraries(peripheral_warp_reshade_remote PRIVATE kernel32 user32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_pw_remote_suffix ".addon64")
    else()
        set(_pw_remote_suffix ".addon32")
    endif()
    set_target_properties(peripheral_warp_reshade_remote PROPERTIES
        PREFIX ""
        OUTPUT_NAME "optimizer-fps-dlss5-remote"
        SUFFIX "${_pw_remote_suffix}")
    if(MSVC)
        target_compile_options(peripheral_warp_reshade_remote PRIVATE /W4 /WX /EHsc /permissive-)
    endif()
endif()
