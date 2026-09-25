# Microsoft Detours (MIT) for the ReShade add-on's NGX hooks.
#
# Two shapes are supported and both expose the same imported-style target:
#
#   OptimizerFps::Detours   include directory + the detours library
#
#   * OFPS_DETOURS_ROOT set  -> a prebuilt tree with include/detours/detours.h and
#                             library/detours/detours.lib (e.g. the OptiScaler
#                             vendored copy). Nothing is compiled.
#   * otherwise            -> the sources fetched by cmake/Dependencies.cmake are
#                             compiled into the static library ofps_detours, which
#                             inherits CMAKE_MSVC_RUNTIME_LIBRARY (OFPS_STATIC_CRT).
#
# Included from cmake/Dependencies.cmake; do not include directly.

include_guard(GLOBAL)

if(TARGET ofps_detours)
    return()
endif()

if(NOT "${OFPS_DETOURS_ROOT}" STREQUAL "")
    if(NOT EXISTS "${OFPS_DETOURS_ROOT}/include/detours/detours.h"
       OR NOT EXISTS "${OFPS_DETOURS_ROOT}/library/detours/detours.lib")
        message(FATAL_ERROR
            "OFPS_DETOURS_ROOT must contain include/detours/detours.h and "
            "library/detours/detours.lib (Microsoft Detours, MIT)")
    endif()
    add_library(ofps_detours INTERFACE)
    target_include_directories(ofps_detours INTERFACE "${OFPS_DETOURS_ROOT}/include/detours")
    target_link_libraries(ofps_detours INTERFACE "${OFPS_DETOURS_ROOT}/library/detours/detours.lib")
    add_library(OptimizerFps::Detours ALIAS ofps_detours)
    return()
endif()

if(NOT DEFINED OFPS_DETOURS_SOURCE_DIR OR NOT EXISTS "${OFPS_DETOURS_SOURCE_DIR}/src/detours.cpp")
    message(FATAL_ERROR "Detours sources were not populated; set OFPS_DETOURS_ROOT or let FetchContent run")
endif()

# creatwth.cpp #includes "uimports.cpp" from the same directory, so the whole set
# has to be compiled out of ${OFPS_DETOURS_SOURCE_DIR}/src.
add_library(ofps_detours STATIC
    "${OFPS_DETOURS_SOURCE_DIR}/src/detours.cpp"
    "${OFPS_DETOURS_SOURCE_DIR}/src/modules.cpp"
    "${OFPS_DETOURS_SOURCE_DIR}/src/disasm.cpp"
    "${OFPS_DETOURS_SOURCE_DIR}/src/image.cpp"
    "${OFPS_DETOURS_SOURCE_DIR}/src/creatwth.cpp")
add_library(OptimizerFps::Detours ALIAS ofps_detours)

target_include_directories(ofps_detours PUBLIC "${OFPS_DETOURS_SOURCE_DIR}/src")
target_compile_definitions(ofps_detours PRIVATE WIN32_LEAN_AND_MEAN)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(ofps_detours PRIVATE DETOURS_X64 DETOURS_64BIT _AMD64_)
else()
    target_compile_definitions(ofps_detours PRIVATE DETOURS_X86 _X86_)
endif()

if(MSVC)
    # Upstream Detours is not warning-clean under our /W4 /WX regime, and it is
    # third-party code we do not patch. /W0 is appended after the flags coming
    # from CMAKE_CXX_FLAGS, so it wins; /WX is only ever added per target here,
    # never globally, so nothing has to be stripped.
    target_compile_options(ofps_detours PRIVATE /W0)
endif()

set_target_properties(ofps_detours PROPERTIES FOLDER "third_party")
