# Remote overlay for 32-bit games: no renderer, SDK core, or Detours.
add_library(peripheral_warp_reshade_remote MODULE
    ${PROJECT_SOURCE_DIR}/core/settings/schema.cpp
    remote_main.cpp remote_overlay.cpp remote_overlay.h
    remote_ui_source.cpp remote_ui_source.h
    ini_section.cpp ini_section.h
    host_watch.cpp host_watch.h
    overlay_event.cpp overlay_event.h
    remote_link.cpp remote_link.h
    ipc.h
    ../common/overlay_widgets.cpp ../common/overlay_widgets.h
    ../reshade/addon/overlay_zone.cpp ../reshade/addon/overlay_zone.h)
target_compile_features(peripheral_warp_reshade_remote PRIVATE cxx_std_20)
target_include_directories(peripheral_warp_reshade_remote PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/hosts/common
    ${OFPS_RESHADE_INCLUDE_DIR}
    ${OFPS_IMGUI_INCLUDE_DIR})
target_link_libraries(peripheral_warp_reshade_remote PRIVATE kernel32 user32)
target_compile_definitions(peripheral_warp_reshade_remote PRIVATE OFPS_UI_RESHADE=1)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_ofps_remote_suffix ".addon64")
else()
    set(_ofps_remote_suffix ".addon32")
endif()
set_target_properties(peripheral_warp_reshade_remote PROPERTIES
    PREFIX ""
    OUTPUT_NAME "optimizer-fps-dlss5-remote"
    SUFFIX "${_ofps_remote_suffix}")
if(MSVC)
    target_compile_options(peripheral_warp_reshade_remote PRIVATE /W4 /WX /EHsc /permissive-)
endif()
