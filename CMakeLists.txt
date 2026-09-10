cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.cmake)

project(PeripheralWarp VERSION ${PW_SDK_VERSION} LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# The two version sources reach the C++ sources through a generated header.
configure_file(cmake/pw_version.h.in ${CMAKE_BINARY_DIR}/generated/pw_version.h @ONLY)
include_directories(${CMAKE_BINARY_DIR}/generated)

# ... and the checked-in NOTICE is regenerated at configure time so it can never go stale.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.in ${CMAKE_CURRENT_SOURCE_DIR}/NOTICE @ONLY NEWLINE_STYLE LF)

# The release the add-on reports must have a section in the changelog.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md _pw_changelog_lines)
set(_pw_changelog_has_release FALSE)
foreach(_pw_line IN LISTS _pw_changelog_lines)
    if(_pw_line STREQUAL "## ${PW_RELEASE_VERSION}")
        set(_pw_changelog_has_release TRUE)
        break()
    endif()
endforeach()
if(NOT _pw_changelog_has_release)
    message(FATAL_ERROR
        "CHANGELOG.md has no section for the current release.\n"
        "  PW_RELEASE_VERSION is \"${PW_RELEASE_VERSION}\" (cmake/Version.cmake)\n"
        "  Expected a line reading exactly: ## ${PW_RELEASE_VERSION}\n"
        "Add that heading with the notes for this release, or correct PW_RELEASE_VERSION.")
endif()
unset(_pw_changelog_lines)
unset(_pw_changelog_has_release)

option(PW_BUILD_TESTS "Build CPU reference tests" ON)
option(PW_BUILD_D3D_ADAPTERS "Build Direct3D 11 and Direct3D 12 adapters" ${WIN32})
option(PW_BUILD_RESHADE_ADDON "Build the optional ReShade add-on (NGX interposer)" OFF)
option(PW_BUILD_RESHADE_REMOTE32 "Build the remote overlay ReShade add-on (32-bit games whose NR runs in a 64-bit host)" OFF)
option(PW_COMPILE_SHADERS "Compile DXBC and SPIR-V shader artifacts" ${WIN32})
option(PW_STATIC_CRT "Link the C++ runtime statically (/MT): add-ons then depend on no MSVCP140/VCRUNTIME140 in the game process, like renodx" OFF)
if(PW_STATIC_CRT AND MSVC)
    cmake_policy(SET CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set(PW_RESHADE_SDK_ROOT "" CACHE PATH "Path containing the ReShade include directory")
set(PW_IMGUI_ROOT "" CACHE PATH "Path containing imgui.h")
set(PW_DETOURS_ROOT "" CACHE PATH "Microsoft Detours root with include/detours and library/detours (ReShade add-on NGX hook)")

# External build dependencies (ReShade SDK, Dear ImGui, Microsoft Detours): fetched on demand or
# taken from the PW_*_ROOT escape hatches. Must come after the PW_* options and after
# CMAKE_MSVC_RUNTIME_LIBRARY, so the Detours static library inherits the same C runtime. It defines
# PW_RESHADE_INCLUDE_DIR / PW_IMGUI_INCLUDE_DIR and the PeripheralWarp::Detours target that
# adapters/reshade consumes, and does the reshade.hpp / imgui.h validation itself.
include(cmake/Dependencies.cmake)

add_library(peripheral_warp_core STATIC
    src/core/input_v2.cpp
    src/core/layout.cpp
    src/core/layout_v2.cpp
    src/core/math.cpp)
add_library(PeripheralWarp::Core ALIAS peripheral_warp_core)
set_target_properties(peripheral_warp_core PROPERTIES EXPORT_NAME Core)
target_compile_features(peripheral_warp_core PUBLIC cxx_std_20)
target_include_directories(peripheral_warp_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
if(MSVC)
    target_compile_options(peripheral_warp_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(peripheral_warp_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PW_COMPILE_SHADERS)
    include(cmake/CompileShaders.cmake)
endif()

if(PW_BUILD_D3D_ADAPTERS)
    if(NOT WIN32)
        message(FATAL_ERROR "Direct3D adapters are available only on Windows")
    endif()
    add_subdirectory(adapters/d3d11)
    add_subdirectory(adapters/d3d12)
endif()

if(PW_BUILD_RESHADE_ADDON OR PW_BUILD_RESHADE_REMOTE32)
    if(NOT WIN32)
        message(FATAL_ERROR "The ReShade add-on is currently built only on Windows")
    endif()
    # reshade.hpp / imgui.h presence and the ImGui ABI are validated in cmake/Dependencies.cmake.
    add_subdirectory(adapters/reshade)
endif()

if(PW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

set(_pw_install_targets peripheral_warp_core)
if(TARGET peripheral_warp_d3d11)
    list(APPEND _pw_install_targets peripheral_warp_d3d11)
endif()
if(TARGET peripheral_warp_d3d12)
    list(APPEND _pw_install_targets peripheral_warp_d3d12)
endif()
install(TARGETS ${_pw_install_targets} EXPORT PeripheralWarpTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(TARGET peripheral_warp_reshade)
    install(TARGETS peripheral_warp_reshade
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(PW_BUILD_D3D_ADAPTERS)
    install(FILES adapters/common/adapter_types.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/peripheral_warp/adapters/common)
    install(FILES adapters/d3d11/d3d11_adapter.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/peripheral_warp/adapters/d3d11)
    install(FILES adapters/d3d12/d3d12_adapter.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/peripheral_warp/adapters/d3d12)
endif()
install(DIRECTORY shaders/ DESTINATION ${CMAKE_INSTALL_DATADIR}/peripheral-warp/shaders
        FILES_MATCHING PATTERN "*.hlsl" PATTERN "*.hlsli")
install(EXPORT PeripheralWarpTargets
        FILE PeripheralWarpTargets.cmake
        NAMESPACE PeripheralWarp::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PeripheralWarp)
configure_package_config_file(
        cmake/PeripheralWarpConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/PeripheralWarpConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PeripheralWarp)
write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/PeripheralWarpConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion)
install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/PeripheralWarpConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/PeripheralWarpConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PeripheralWarp)
install(FILES LICENSE NOTICE CHANGELOG.md DEPENDENCIES.lock.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/peripheral-warp)
install(DIRECTORY third_party/licenses/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/peripheral-warp/licenses)
