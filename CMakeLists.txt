# obs-irl-source — IRL streaming source plugin for OBS
# https://irlserver.com
#
# Copyright (C) 2026 Thomas Lekanger
# SPDX-License-Identifier: AGPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)
project(obs-irl-source VERSION 0.3.0 LANGUAGES C)

if(DEFINED OBS_SOURCE_DIR AND EXISTS "${OBS_SOURCE_DIR}/cmake/finders")
    list(APPEND CMAKE_MODULE_PATH "${OBS_SOURCE_DIR}/cmake/finders")
elseif(DEFINED ENV{OBS_SOURCE_DIR} AND EXISTS "$ENV{OBS_SOURCE_DIR}/cmake/finders")
    list(APPEND CMAKE_MODULE_PATH "$ENV{OBS_SOURCE_DIR}/cmake/finders")
endif()

# ── Find OBS ─────────────────────────────────────────────────
# Try find_package first (works with OBS build tree, cmake install, or
# system libobs-dev which ships CMake config files).
find_package(libobs QUIET)

if(libobs_FOUND)
    message(STATUS "Found libobs via find_package")
    set(_OBS_USE_FIND_PACKAGE ON)
else()
    set(_OBS_USE_FIND_PACKAGE OFF)

    # Fall back to manual OBS_SOURCE_DIR approach.
    if(NOT DEFINED OBS_SOURCE_DIR AND DEFINED ENV{OBS_SOURCE_DIR})
        set(OBS_SOURCE_DIR "$ENV{OBS_SOURCE_DIR}")
    endif()

    set(_OBS_INCLUDE_DIRS "")
    set(_OBS_LIB_DIRS "")

    if(OBS_SOURCE_DIR)
        list(APPEND _OBS_INCLUDE_DIRS "${OBS_SOURCE_DIR}/libobs")
        # Generated headers from OBS build (obsconfig.h, libobs_EXPORT.h)
        if(EXISTS "${OBS_SOURCE_DIR}/build/config")
            list(APPEND _OBS_INCLUDE_DIRS "${OBS_SOURCE_DIR}/build/config")
        endif()
        if(EXISTS "${OBS_SOURCE_DIR}/build/libobs")
            list(APPEND _OBS_INCLUDE_DIRS "${OBS_SOURCE_DIR}/build/libobs")
        endif()
        # OBS threading.h includes <pthread.h> from w32-pthreads on Windows
        if(WIN32 AND EXISTS "${OBS_SOURCE_DIR}/deps/w32-pthreads")
            list(APPEND _OBS_INCLUDE_DIRS "${OBS_SOURCE_DIR}/deps/w32-pthreads")
        endif()
        if(NOT EXISTS "${OBS_SOURCE_DIR}/build/config/obsconfig.h" AND
           NOT EXISTS "${OBS_SOURCE_DIR}/build/libobs/libobs_EXPORT.h")
            message(WARNING
                "OBS_SOURCE_DIR is set, but generated OBS headers were not found. "
                "Build libobs in ../obs-studio first so obsconfig.h/libobs_EXPORT.h exist.")
        endif()
        if(WIN32)
            list(APPEND _OBS_LIB_DIRS
                "${OBS_SOURCE_DIR}/build/libobs/Release"
                "${OBS_SOURCE_DIR}/build/libobs/RelWithDebInfo"
                "${OBS_SOURCE_DIR}/build/libobs/Debug"
                "${OBS_SOURCE_DIR}/build/libobs"
                "${OBS_SOURCE_DIR}/build/deps/w32-pthreads/Release"
                "${OBS_SOURCE_DIR}/build/deps/w32-pthreads/RelWithDebInfo"
                "${OBS_SOURCE_DIR}/build/deps/w32-pthreads/Debug"
            )
        else()
            # On macOS (Xcode) and Linux, search multiple possible locations.
            # The library may be named libobs.dylib, libobs.so, or libobs.0.dylib.
            find_library(_OBS_LIB_PATH
                NAMES obs libobs
                PATHS
                    "${OBS_SOURCE_DIR}/build/libobs/Release"
                    "${OBS_SOURCE_DIR}/build/libobs/RelWithDebInfo"
                    "${OBS_SOURCE_DIR}/build/libobs/Debug"
                    "${OBS_SOURCE_DIR}/build/libobs"
                NO_DEFAULT_PATH
            )
            if(_OBS_LIB_PATH)
                get_filename_component(_OBS_LIB_DIR "${_OBS_LIB_PATH}" DIRECTORY)
                list(APPEND _OBS_LIB_DIRS "${_OBS_LIB_DIR}")
            else()
                list(APPEND _OBS_LIB_DIRS "${OBS_SOURCE_DIR}/build/libobs")
            endif()
        endif()
    else()
        # Try system-installed OBS (e.g. libobs-dev on Ubuntu)
        find_path(_SYSTEM_OBS_INCLUDE obs-module.h
            PATHS /usr/include/obs /usr/local/include/obs
            NO_DEFAULT_PATH
        )
        if(_SYSTEM_OBS_INCLUDE)
            list(APPEND _OBS_INCLUDE_DIRS "${_SYSTEM_OBS_INCLUDE}")
        endif()
    endif()
endif()

# ── OBS dependency headers when using an OBS source tree ─────────────────────
set(_OBS_EXTRA_INCLUDE_DIRS "")
set(_OBS_EXTRA_LIBS "")

find_package(SIMDe QUIET)
if(SIMDe_FOUND)
    list(APPEND _OBS_EXTRA_INCLUDE_DIRS "${SIMDe_INCLUDE_DIR}")
else()
    find_path(SIMDE_FALLBACK_INCLUDE_DIR
        NAMES simde/simde-common.h
        PATHS /usr/include /usr/local/include
    )
    if(SIMDE_FALLBACK_INCLUDE_DIR)
        list(APPEND _OBS_EXTRA_INCLUDE_DIRS "${SIMDE_FALLBACK_INCLUDE_DIR}")
    endif()
endif()

find_package(jansson QUIET)
if(jansson_FOUND)
    list(APPEND _OBS_EXTRA_INCLUDE_DIRS "${jansson_INCLUDE_DIR}")
    list(APPEND _OBS_EXTRA_LIBS jansson::jansson)
else()
    find_path(JANSSON_FALLBACK_INCLUDE_DIR
        NAMES jansson.h
        PATHS /usr/include /usr/local/include
    )
    find_library(JANSSON_FALLBACK_LIBRARY
        NAMES jansson
        PATHS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib
    )
    if(JANSSON_FALLBACK_INCLUDE_DIR AND JANSSON_FALLBACK_LIBRARY)
        list(APPEND _OBS_EXTRA_INCLUDE_DIRS "${JANSSON_FALLBACK_INCLUDE_DIR}")
        list(APPEND _OBS_EXTRA_LIBS "${JANSSON_FALLBACK_LIBRARY}")
    endif()
endif()

find_package(Uthash QUIET)
if(Uthash_FOUND)
    list(APPEND _OBS_EXTRA_INCLUDE_DIRS "${Uthash_INCLUDE_DIR}")
else()
    find_path(UTHASH_FALLBACK_INCLUDE_DIR
        NAMES uthash.h
        PATHS /usr/include /usr/local/include
    )
    if(UTHASH_FALLBACK_INCLUDE_DIR)
        list(APPEND _OBS_EXTRA_INCLUDE_DIRS "${UTHASH_FALLBACK_INCLUDE_DIR}")
    endif()
endif()

# ── Find FFmpeg ──────────────────────────────────────────────
# Try pkg-config first (Linux, MSYS2, macOS with Homebrew).
# Fall back to find_path/find_library for MSVC + pre-built FFmpeg.
set(_FFMPEG_FOUND OFF)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBAV QUIET IMPORTED_TARGET
        libavformat
        libavcodec
        libswresample
        libswscale
        libavutil
    )
    if(LIBAV_FOUND)
        set(_FFMPEG_FOUND ON)
    endif()
endif()

if(NOT _FFMPEG_FOUND)
    # Manual FFmpeg discovery (Windows MSVC with pre-built FFmpeg).
    #
    # Pass -DFFMPEG_DIR=C:/path/to/ffmpeg on the cmake command line,
    # or set the FFMPEG_DIR environment variable.  The directory should
    # contain include/ and lib/ sub-directories.
    if(NOT DEFINED FFMPEG_DIR AND DEFINED ENV{FFMPEG_DIR})
        set(FFMPEG_DIR "$ENV{FFMPEG_DIR}")
    endif()

    set(_FFMPEG_SEARCH_PATHS "")
    if(FFMPEG_DIR)
        list(APPEND _FFMPEG_SEARCH_PATHS "${FFMPEG_DIR}")
    endif()

    find_path(AVFORMAT_INCLUDE_DIR libavformat/avformat.h
        PATHS ${_FFMPEG_SEARCH_PATHS}
        PATH_SUFFIXES include
    )
    find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h
        PATHS ${_FFMPEG_SEARCH_PATHS}
        PATH_SUFFIXES include
    )
    find_path(SWRESAMPLE_INCLUDE_DIR libswresample/swresample.h
        PATHS ${_FFMPEG_SEARCH_PATHS}
        PATH_SUFFIXES include
    )
    find_path(SWSCALE_INCLUDE_DIR libswscale/swscale.h
        PATHS ${_FFMPEG_SEARCH_PATHS}
        PATH_SUFFIXES include
    )
    find_path(AVUTIL_INCLUDE_DIR libavutil/avutil.h
        PATHS ${_FFMPEG_SEARCH_PATHS}
        PATH_SUFFIXES include
    )

    find_library(AVFORMAT_LIB NAMES avformat PATHS ${_FFMPEG_SEARCH_PATHS} PATH_SUFFIXES lib)
    find_library(AVCODEC_LIB  NAMES avcodec  PATHS ${_FFMPEG_SEARCH_PATHS} PATH_SUFFIXES lib)
    find_library(SWRESAMPLE_LIB NAMES swresample PATHS ${_FFMPEG_SEARCH_PATHS} PATH_SUFFIXES lib)
    find_library(SWSCALE_LIB  NAMES swscale  PATHS ${_FFMPEG_SEARCH_PATHS} PATH_SUFFIXES lib)
    find_library(AVUTIL_LIB   NAMES avutil   PATHS ${_FFMPEG_SEARCH_PATHS} PATH_SUFFIXES lib)

    if(AVFORMAT_LIB AND AVCODEC_LIB AND SWRESAMPLE_LIB AND SWSCALE_LIB AND AVUTIL_LIB)
        set(_FFMPEG_FOUND ON)
        set(_FFMPEG_MANUAL_INCLUDE_DIRS
            ${AVFORMAT_INCLUDE_DIR}
            ${AVCODEC_INCLUDE_DIR}
            ${SWRESAMPLE_INCLUDE_DIR}
            ${SWSCALE_INCLUDE_DIR}
            ${AVUTIL_INCLUDE_DIR}
        )
        list(REMOVE_DUPLICATES _FFMPEG_MANUAL_INCLUDE_DIRS)
        set(_FFMPEG_MANUAL_LIBS
            ${AVFORMAT_LIB}
            ${AVCODEC_LIB}
            ${SWRESAMPLE_LIB}
            ${SWSCALE_LIB}
            ${AVUTIL_LIB}
        )
    endif()
endif()

if(NOT _FFMPEG_FOUND)
    message(FATAL_ERROR
        "FFmpeg not found.  Either:\n"
        "  - Install FFmpeg dev packages (apt install libavformat-dev ...)\n"
        "  - Install via MSYS2 (pacman -S mingw-w64-ucrt-x86_64-ffmpeg)\n"
        "  - Pass -DFFMPEG_DIR=C:/path/to/ffmpeg (pre-built Windows SDK)\n"
    )
endif()

# ── Plugin shared library ────────────────────────────────────
set(PLUGIN_SOURCES
    src/plugin.c
    src/irl-source.c
    src/receiver.c
    src/audio-buffer.c
    src/audio-speed.c
    src/video-handler.c
    src/pts-repair.c
    src/settings.c
)

add_library(obs-irl-source MODULE ${PLUGIN_SOURCES})

# ── Include directories ──────────────────────────────────────
target_include_directories(obs-irl-source PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(NOT _OBS_USE_FIND_PACKAGE AND _OBS_INCLUDE_DIRS)
    target_include_directories(obs-irl-source PRIVATE ${_OBS_INCLUDE_DIRS})
    # OBS headers check HAVE_OBSCONFIG_H to include the generated config
    if(OBS_SOURCE_DIR AND EXISTS "${OBS_SOURCE_DIR}/build/config/obsconfig.h")
        target_compile_definitions(obs-irl-source PRIVATE HAVE_OBSCONFIG_H)
    endif()
endif()

if(_OBS_EXTRA_INCLUDE_DIRS)
    list(REMOVE_DUPLICATES _OBS_EXTRA_INCLUDE_DIRS)
    target_include_directories(obs-irl-source PRIVATE ${_OBS_EXTRA_INCLUDE_DIRS})
endif()

if(LIBAV_FOUND)
    target_include_directories(obs-irl-source PRIVATE ${LIBAV_INCLUDE_DIRS})
elseif(_FFMPEG_MANUAL_INCLUDE_DIRS)
    target_include_directories(obs-irl-source PRIVATE ${_FFMPEG_MANUAL_INCLUDE_DIRS})
endif()

# ── Link directories (OBS) ───────────────────────────────────
if(NOT _OBS_USE_FIND_PACKAGE AND _OBS_LIB_DIRS)
    target_link_directories(obs-irl-source PRIVATE ${_OBS_LIB_DIRS})
endif()

# ── Link libraries ───────────────────────────────────────────
if(_OBS_USE_FIND_PACKAGE)
    set(_OBS_LINK_TARGET OBS::libobs)
elseif(_OBS_LIB_PATH)
    # Link against the exact library found by find_library (handles
    # versioned names like libobs.0.dylib that -lobs can't find)
    set(_OBS_LINK_TARGET "${_OBS_LIB_PATH}")
else()
    set(_OBS_LINK_TARGET obs)
endif()

if(LIBAV_FOUND)
    target_link_libraries(obs-irl-source PRIVATE ${_OBS_LINK_TARGET} PkgConfig::LIBAV)
else()
    target_link_libraries(obs-irl-source PRIVATE ${_OBS_LINK_TARGET} ${_FFMPEG_MANUAL_LIBS})
endif()

if(_OBS_EXTRA_LIBS)
    target_link_libraries(obs-irl-source PRIVATE ${_OBS_EXTRA_LIBS})
endif()

if(WIN32 AND NOT _OBS_USE_FIND_PACKAGE)
    # Link w32-pthreads from OBS source tree (plugin uses pthread API directly)
    target_link_libraries(obs-irl-source PRIVATE w32-pthreads)
elseif(NOT WIN32)
    target_link_libraries(obs-irl-source PRIVATE pthread)
endif()

# ── C standard ───────────────────────────────────────────────
target_compile_features(obs-irl-source PRIVATE c_std_11)

# ── Compiler warnings ────────────────────────────────────────
if(MSVC)
    target_compile_options(obs-irl-source PRIVATE /W4 /wd4100)
    # Export nothing by default (OBS loads the module entry points)
    set_target_properties(obs-irl-source PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
else()
    target_compile_options(obs-irl-source PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
    )
endif()

# ── Output name ──────────────────────────────────────────────
# Remove "lib" prefix so output is obs-irl-source.so/.dll everywhere.
set_target_properties(obs-irl-source PROPERTIES PREFIX "")

# ── Windows-specific ─────────────────────────────────────────
if(WIN32)
    # Copy FFmpeg DLLs next to the plugin for convenience
    if(FFMPEG_DIR AND EXISTS "${FFMPEG_DIR}/bin")
        add_custom_command(TARGET obs-irl-source POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${FFMPEG_DIR}/bin"
                "$<TARGET_FILE_DIR:obs-irl-source>"
            COMMENT "Copying FFmpeg DLLs to output directory"
        )
    endif()
endif()

# ── Install ──────────────────────────────────────────────────
include(GNUInstallDirs)

if(APPLE)
    set(OBS_PLUGIN_DIR "$ENV{HOME}/Library/Application Support/obs-studio/plugins/obs-irl-source/bin")
elseif(WIN32)
    set(OBS_PLUGIN_DIR "$ENV{APPDATA}/obs-studio/plugins/obs-irl-source/bin/64bit")
else()
    set(OBS_PLUGIN_DIR "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
endif()

install(TARGETS obs-irl-source DESTINATION "${OBS_PLUGIN_DIR}")

# ── Summary ──────────────────────────────────────────────────
message(STATUS "")
message(STATUS "obs-irl-source v${PROJECT_VERSION}")
message(STATUS "  OS:        ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Compiler:  ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
if(_OBS_USE_FIND_PACKAGE)
    message(STATUS "  OBS:       find_package (libobs)")
else()
    message(STATUS "  OBS dir:   ${OBS_SOURCE_DIR}")
endif()
if(LIBAV_FOUND)
    message(STATUS "  FFmpeg:    pkg-config (${LIBAV_VERSION})")
else()
    message(STATUS "  FFmpeg:    manual (${FFMPEG_DIR})")
endif()
message(STATUS "  Install:   ${OBS_PLUGIN_DIR}")
message(STATUS "")
