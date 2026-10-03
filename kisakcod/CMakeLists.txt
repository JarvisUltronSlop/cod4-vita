cmake_minimum_required(VERSION 3.16)

project(KisakCOD)

# Set the default C++ standard
set(CMAKE_CXX_STANDARD 20)

# Keep the existing Win32 build as the default.  The Vita branch owns its
# VitaSDK bootstrap, typed asset backend, ARM object slice, and focused runtime
# smoke targets so the desktop graph never sees Vita-only headers or libraries.
set(KISAK_PLATFORM win32 CACHE STRING "Kisak platform (win32 or vita)")
set_property(CACHE KISAK_PLATFORM PROPERTY STRINGS win32 vita)

if(KISAK_PLATFORM STREQUAL "vita")
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/scripts/platform/vita)
    return()
endif()

add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)

# Set Address Sanitizer
# NOTE: If you use this you will need to copy to the game's directory
# clang_rt.asan_dbg_dynamic-i386.dll and clang_rt.asan_dynamic-i386.dll from the MSVC C directory located below (or similar)
# C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207\bin\Hostx86\x86
set(USE_ASAN OFF)

# Set to ON to build the sound system against OpenAL instead of Miles/MSS.
# When OFF (default), the original Miles Sound System (MSS/AIL) backend is used.
set(KISAK_OPENAL OFF)

option(KISAK_VITA_ASSET_EXPORTER "Build the offline Vita asset exporter without proprietary SDKs" OFF)
if(KISAK_VITA_ASSET_EXPORTER)
    set(KISAK_OPENAL ON)
    set(KISAK_BUILD_MP ON CACHE BOOL "" FORCE)
    set(KISAK_BUILD_SP OFF CACHE BOOL "" FORCE)
    set(KISAK_BUILD_DEDI OFF CACHE BOOL "" FORCE)
    set(KISAK_BUILD_RADIANT OFF CACHE BOOL "" FORCE)
endif()

option(KISAK_BUILD_MP "Build the multiplayer client" ON)
option(KISAK_BUILD_SP "Build the single-player client" ON)
option(KISAK_BUILD_DEDI "Build the dedicated server" ON)
option(KISAK_BUILD_RADIANT "Build Radiant" ON)

# Set the gloabl directory variables
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(DEPS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps)
set(BIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/bin)
set(SCRIPTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/scripts)
set(PLATFORM_DIR ${SCRIPTS_DIR}/platform)

# Set the output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${BIN_DIR})

# Include global scripts/tricks
include(${SCRIPTS_DIR}/extern/increment_build.cmake)

# Include platform overrides
include(${SCRIPTS_DIR}/platform_override.cmake)

# Log out the KISAK_PLATFORM
message("Building KISAK_PLATFORM: ${KISAK_PLATFORM}")

# Include the subprojects
if(KISAK_BUILD_MP)
    add_subdirectory(${SCRIPTS_DIR}/mp)
endif()
if(KISAK_BUILD_SP)
    add_subdirectory(${SCRIPTS_DIR}/sp)
endif()
if(KISAK_BUILD_DEDI)
    add_subdirectory(${SCRIPTS_DIR}/dedi)
endif()
if(KISAK_BUILD_RADIANT)
    add_subdirectory(${SCRIPTS_DIR}/radiant)
endif()
