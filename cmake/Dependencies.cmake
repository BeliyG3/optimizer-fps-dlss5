# PeripheralWarp external build dependencies.
#
# Header-only ReShade add-on SDK, Dear ImGui and Microsoft Detours are fetched
# on demand into ${CMAKE_SOURCE_DIR}/external (overridable through
# FETCHCONTENT_BASE_DIR) and pinned by SHA-256. Every dependency keeps its
# historical "point at a local checkout" escape hatch: set PW_RESHADE_SDK_ROOT,
# PW_IMGUI_ROOT or PW_DETOURS_ROOT to a non-empty path and nothing is downloaded.
#
# Include this file from the top-level CMakeLists.txt *after* the PW_* options and
# after CMAKE_MSVC_RUNTIME_LIBRARY has been set (PW_STATIC_CRT), so that the
# Detours static library inherits the same C runtime as the add-ons.
#
# Requires CMake >= 3.25 (the presets are schema version 6; DOWNLOAD_EXTRACT_TIMESTAMP needs 3.24).

include_guard(GLOBAL)

# Must be set before FetchContent is included: the module itself defines
# FETCHCONTENT_BASE_DIR (defaulting to ${CMAKE_BINARY_DIR}/_deps) if nothing did.
# Still overridable from the command line or a preset.
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/external"
    CACHE PATH "Directory that holds the fetched PeripheralWarp build dependencies")

include(FetchContent)

# The archives below are plain source drops; none of them must be added as a
# CMake subproject (ReShade even ships a full CMakeLists.txt of its own).
# Pointing SOURCE_SUBDIR at a directory without a CMakeLists.txt makes
# FetchContent_MakeAvailable() populate and stop there.
set(_pw_no_configure_subdir "cmake-do-not-configure")

# --- ReShade add-on SDK (BSD-3-Clause) - only include/ is used ---------------
set(PW_RESHADE_VERSION "6.8.0" CACHE STRING "ReShade add-on SDK tag to fetch")
set(PW_RESHADE_URL_SHA256
    "23197b1d0a032fb875c6c01a45405f1d5f3d753ddcfcf833be2df3675724e397")

# --- Dear ImGui (MIT) - headers only; the ABI must match ReShade -------------
# The *docking* branch tag is required: reshade_overlay.hpp uses ImGuiDockNodeFlags,
# which only exists there. Plain v1.92.5 does not compile against ReShade 6.8.
set(PW_IMGUI_VERSION "1.92.5-docking" CACHE STRING "Dear ImGui tag to fetch (docking branch)")
set(PW_IMGUI_URL_SHA256
    "c816c20e8c75f3e15ae867350e79925502d1a6a85938bb1a73b8927e5f31f9cb")
set(PW_IMGUI_EXPECTED_VERSION_NUM "19250")

# --- Microsoft Detours (MIT) - pinned to a main commit ----------------------
set(PW_DETOURS_COMMIT "adb07604aa56508448b95bf037c2a6d0d3b6831a"
    CACHE STRING "Microsoft Detours commit to fetch")
set(PW_DETOURS_URL_SHA256
    "42125d318f607cded3332bb61bb7bebac8a58a57e46ced6de573610f1d4cedba")

# external/ is shared by every preset, but FetchContent stamps its download
# sub-builds with the generator platform, so an x64 configure followed by a Win32
# one would fail with "generator platform: Win32 does not match ...". The
# sub-builds only download and unpack - nothing is compiled for any architecture -
# so pin their platform and restore the real one at the end of this file.
set(_pw_saved_generator_platform "${CMAKE_GENERATOR_PLATFORM}")
if(CMAKE_GENERATOR MATCHES "Visual Studio")
    set(CMAKE_GENERATOR_PLATFORM "x64")
endif()

function(_pw_declare_archive name url sha256)
    FetchContent_Declare(${name}
        URL "${url}"
        URL_HASH "SHA256=${sha256}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR "${_pw_no_configure_subdir}")
endfunction()

# ---------------------------------------------------------------------------
# ReShade
# ---------------------------------------------------------------------------
if(PW_BUILD_RESHADE_ADDON OR PW_BUILD_RESHADE_REMOTE32)
    if("${PW_RESHADE_SDK_ROOT}" STREQUAL "")
        _pw_declare_archive(reshade
            "https://github.com/crosire/reshade/archive/refs/tags/v${PW_RESHADE_VERSION}.tar.gz"
            "${PW_RESHADE_URL_SHA256}")
        FetchContent_MakeAvailable(reshade)
        set(PW_RESHADE_SDK_ROOT "${reshade_SOURCE_DIR}" CACHE PATH
            "Path containing the ReShade include directory" FORCE)
        message(STATUS "PeripheralWarp: fetched ReShade ${PW_RESHADE_VERSION} into ${reshade_SOURCE_DIR}")
    else()
        message(STATUS "PeripheralWarp: using local ReShade SDK ${PW_RESHADE_SDK_ROOT}")
    endif()

    if(NOT EXISTS "${PW_RESHADE_SDK_ROOT}/include/reshade.hpp")
        message(FATAL_ERROR "PW_RESHADE_SDK_ROOT must contain include/reshade.hpp (got '${PW_RESHADE_SDK_ROOT}')")
    endif()
    set(PW_RESHADE_INCLUDE_DIR "${PW_RESHADE_SDK_ROOT}/include" CACHE INTERNAL "")

    # -----------------------------------------------------------------------
    # Dear ImGui
    # -----------------------------------------------------------------------
    if("${PW_IMGUI_ROOT}" STREQUAL "")
        _pw_declare_archive(imgui
            "https://github.com/ocornut/imgui/archive/refs/tags/v${PW_IMGUI_VERSION}.tar.gz"
            "${PW_IMGUI_URL_SHA256}")
        FetchContent_MakeAvailable(imgui)
        set(PW_IMGUI_ROOT "${imgui_SOURCE_DIR}" CACHE PATH "Path containing imgui.h" FORCE)
        message(STATUS "PeripheralWarp: fetched Dear ImGui ${PW_IMGUI_VERSION} into ${imgui_SOURCE_DIR}")
    else()
        message(STATUS "PeripheralWarp: using local Dear ImGui ${PW_IMGUI_ROOT}")
    endif()

    if(NOT EXISTS "${PW_IMGUI_ROOT}/imgui.h")
        message(FATAL_ERROR "PW_IMGUI_ROOT must contain imgui.h (got '${PW_IMGUI_ROOT}')")
    endif()
    set(PW_IMGUI_INCLUDE_DIR "${PW_IMGUI_ROOT}" CACHE INTERNAL "")

    # reshade_overlay.hpp hard-codes the ImGui ABI it was generated against.
    file(STRINGS "${PW_IMGUI_ROOT}/imgui.h" _pw_imgui_version_line
         REGEX "^#define[ \t]+IMGUI_VERSION_NUM[ \t]+[0-9]+")
    string(REGEX MATCH "[0-9]+$" _pw_imgui_version_num "${_pw_imgui_version_line}")
    if(NOT _pw_imgui_version_num STREQUAL PW_IMGUI_EXPECTED_VERSION_NUM)
        message(FATAL_ERROR
            "Dear ImGui ABI mismatch: reshade_overlay.hpp requires IMGUI_VERSION_NUM "
            "${PW_IMGUI_EXPECTED_VERSION_NUM}, '${PW_IMGUI_ROOT}/imgui.h' declares '${_pw_imgui_version_num}'")
    endif()
endif()

# ---------------------------------------------------------------------------
# Microsoft Detours (only the full 64-bit add-on hooks NGX)
# ---------------------------------------------------------------------------
if(PW_BUILD_RESHADE_ADDON)
    if("${PW_DETOURS_ROOT}" STREQUAL "")
        _pw_declare_archive(detours
            "https://github.com/microsoft/Detours/archive/${PW_DETOURS_COMMIT}.tar.gz"
            "${PW_DETOURS_URL_SHA256}")
        FetchContent_MakeAvailable(detours)
        set(PW_DETOURS_SOURCE_DIR "${detours_SOURCE_DIR}")
        message(STATUS "PeripheralWarp: fetched Microsoft Detours ${PW_DETOURS_COMMIT} into ${detours_SOURCE_DIR}")
    else()
        message(STATUS "PeripheralWarp: using local Microsoft Detours ${PW_DETOURS_ROOT}")
    endif()
    include(${CMAKE_CURRENT_LIST_DIR}/Detours.cmake)
endif()

set(CMAKE_GENERATOR_PLATFORM "${_pw_saved_generator_platform}")
unset(_pw_saved_generator_platform)
