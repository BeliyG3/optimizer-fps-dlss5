cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.cmake)

project(OptimizerFps VERSION ${OFPS_SDK_VERSION} LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# The two version sources reach the C++ sources through a generated header.
configure_file(cmake/ofps_version.h.in ${CMAKE_BINARY_DIR}/generated/ofps_version.h @ONLY)
include_directories(${CMAKE_BINARY_DIR}/generated)

# ... and the checked-in NOTICE is regenerated at configure time so it can never go stale.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.in ${CMAKE_CURRENT_SOURCE_DIR}/NOTICE @ONLY NEWLINE_STYLE LF)

# The release the add-on reports must have a section in the changelog.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md _ofps_changelog_lines)
set(_ofps_changelog_has_release FALSE)
foreach(_ofps_line IN LISTS _ofps_changelog_lines)
    if(_ofps_line STREQUAL "## ${OFPS_RELEASE_VERSION}")
        set(_ofps_changelog_has_release TRUE)
        break()
    endif()
endforeach()
if(NOT _ofps_changelog_has_release)
    message(FATAL_ERROR
        "CHANGELOG.md has no section for the current release.\n"
        "  OFPS_RELEASE_VERSION is \"${OFPS_RELEASE_VERSION}\" (cmake/Version.cmake)\n"
        "  Expected a line reading exactly: ## ${OFPS_RELEASE_VERSION}\n"
        "Add that heading with the notes for this release, or correct OFPS_RELEASE_VERSION.")
endif()
unset(_ofps_changelog_lines)
unset(_ofps_changelog_has_release)

option(OFPS_BUILD_CORE "Build the x64 core" ON)
option(OFPS_BUILD_TESTS "Build CPU reference tests" ON)
option(OFPS_BUILD_D3D_ADAPTERS "Build Direct3D 11 and Direct3D 12 adapters" ${WIN32})
option(OFPS_BUILD_RESHADE_ADDON "Build the optional ReShade add-on (NGX interposer)" OFF)
option(OFPS_BUILD_RESHADE_REMOTE32 "Build the remote overlay ReShade add-on (32-bit games whose NR runs in a 64-bit host)" OFF)
option(OFPS_COMPILE_SHADERS "Compile DXBC and SPIR-V shader artifacts" ${WIN32})
option(OFPS_STATIC_CRT "Link the C++ runtime statically (/MT): add-ons then depend on no MSVCP140/VCRUNTIME140 in the game process, like renodx" OFF)
if(OFPS_STATIC_CRT AND MSVC)
    cmake_policy(SET CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set(OFPS_RESHADE_SDK_ROOT "" CACHE PATH "Path containing the ReShade include directory")
set(OFPS_IMGUI_ROOT "" CACHE PATH "Path containing imgui.h")
set(OFPS_DETOURS_ROOT "" CACHE PATH "Microsoft Detours root with include/detours and library/detours (ReShade add-on NGX hook)")

# External build dependencies (ReShade SDK, Dear ImGui, Microsoft Detours): fetched on demand or
# taken from the OFPS_*_ROOT escape hatches. Must come after the OFPS_* options and after
# CMAKE_MSVC_RUNTIME_LIBRARY, so the Detours static library inherits the same C runtime. It defines
# OFPS_RESHADE_INCLUDE_DIR / OFPS_IMGUI_INCLUDE_DIR and the OptimizerFps::Detours target that
# hosts/reshade consumes, and does the reshade.hpp / imgui.h validation itself.
include(cmake/Dependencies.cmake)

add_subdirectory(sdk)

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND OFPS_BUILD_CORE AND OFPS_BUILD_D3D_ADAPTERS)
    add_subdirectory(core)
endif()

if(OFPS_COMPILE_SHADERS)
    include(cmake/CompileShaders.cmake)
endif()

if(OFPS_BUILD_RESHADE_ADDON OR OFPS_BUILD_RESHADE_REMOTE32)
    if(NOT WIN32)
        message(FATAL_ERROR "The ReShade add-on is currently built only on Windows")
    endif()
    # reshade.hpp / imgui.h presence and the ImGui ABI are validated in cmake/Dependencies.cmake.
endif()
if(OFPS_BUILD_RESHADE_ADDON)
    add_subdirectory(hosts/reshade)
endif()
if(OFPS_BUILD_RESHADE_REMOTE32)
    add_subdirectory(hosts/remote32)
endif()

if(OFPS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(TARGET peripheral_warp_reshade)
    install(TARGETS peripheral_warp_reshade
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY core/shaders/ DESTINATION ${CMAKE_INSTALL_DATADIR}/optimizer-fps-sdk/shaders
        FILES_MATCHING PATTERN "*.hlsl" PATTERN "*.hlsli" PATTERN "temporal_layout.h"
        PATTERN "temporal_passes.def" PATTERN "temporal_passes.md")
install(EXPORT OptimizerFpsSdkTargets
        FILE OptimizerFpsSdkTargets.cmake
        NAMESPACE OptimizerFps::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OptimizerFpsSdk)
configure_package_config_file(
        cmake/OptimizerFpsSdkConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/OptimizerFpsSdkConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OptimizerFpsSdk)
write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/OptimizerFpsSdkConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion)
install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/OptimizerFpsSdkConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/OptimizerFpsSdkConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OptimizerFpsSdk)
install(FILES LICENSE NOTICE CHANGELOG.md DEPENDENCIES.lock.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/optimizer-fps-sdk)
install(DIRECTORY third_party/licenses/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/optimizer-fps-sdk/licenses)
