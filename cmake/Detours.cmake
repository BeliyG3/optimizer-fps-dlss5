# Microsoft Detours (MIT) for the ReShade add-on's NGX hooks.
#
# Two shapes are supported and both expose the same imported-style target:
#
#   PeripheralWarp::Detours   include directory + the detours library
#
#   * PW_DETOURS_ROOT set  -> a prebuilt tree with include/detours/detours.h and
#                             library/detours/detours.lib (e.g. the OptiScaler
#                             vendored copy). Nothing is compiled.
#   * otherwise            -> the sources fetched by cmake/Dependencies.cmake are
#                             compiled into the static library pw_detours, which
#                             inherits CMAKE_MSVC_RUNTIME_LIBRARY (PW_STATIC_CRT).
#
# Included from cmake/Dependencies.cmake; do not include directly.

include_guard(GLOBAL)

if(TARGET pw_detours)
    return()
endif()

if(NOT "${PW_DETOURS_ROOT}" STREQUAL "")
    if(NOT EXISTS "${PW_DETOURS_ROOT}/include/detours/detours.h"
       OR NOT EXISTS "${PW_DETOURS_ROOT}/library/detours/detours.lib")
        message(FATAL_ERROR
            "PW_DETOURS_ROOT must contain include/detours/detours.h and "
            "library/detours/detours.lib (Microsoft Detours, MIT)")
    endif()
    add_library(pw_detours INTERFACE)
    target_include_directories(pw_detours INTERFACE "${PW_DETOURS_ROOT}/include/detours")
    target_link_libraries(pw_detours INTERFACE "${PW_DETOURS_ROOT}/library/detours/detours.lib")
    add_library(PeripheralWarp::Detours ALIAS pw_detours)
    return()
endif()

if(NOT DEFINED PW_DETOURS_SOURCE_DIR OR NOT EXISTS "${PW_DETOURS_SOURCE_DIR}/src/detours.cpp")
    message(FATAL_ERROR "Detours sources were not populated; set PW_DETOURS_ROOT or let FetchContent run")
endif()

# creatwth.cpp #includes "uimports.cpp" from the same directory, so the whole set
# has to be compiled out of ${PW_DETOURS_SOURCE_DIR}/src.
add_library(pw_detours STATIC
    "${PW_DETOURS_SOURCE_DIR}/src/detours.cpp"
    "${PW_DETOURS_SOURCE_DIR}/src/modules.cpp"
    "${PW_DETOURS_SOURCE_DIR}/src/disasm.cpp"
    "${PW_DETOURS_SOURCE_DIR}/src/image.cpp"
    "${PW_DETOURS_SOURCE_DIR}/src/creatwth.cpp")
add_library(PeripheralWarp::Detours ALIAS pw_detours)

target_include_directories(pw_detours PUBLIC "${PW_DETOURS_SOURCE_DIR}/src")
target_compile_definitions(pw_detours PRIVATE WIN32_LEAN_AND_MEAN)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(pw_detours PRIVATE DETOURS_X64 DETOURS_64BIT _AMD64_)
else()
    target_compile_definitions(pw_detours PRIVATE DETOURS_X86 _X86_)
endif()

if(MSVC)
    # Upstream Detours is not warning-clean under our /W4 /WX regime, and it is
    # third-party code we do not patch. /W0 is appended after the flags coming
    # from CMAKE_CXX_FLAGS, so it wins; /WX is only ever added per target here,
    # never globally, so nothing has to be stripped.
    target_compile_options(pw_detours PRIVATE /W0)
endif()

set_target_properties(pw_detours PROPERTIES FOLDER "third_party")
