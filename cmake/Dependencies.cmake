# Third-party dependencies, pinned to exact releases.
#
# Sources are fetched as release archives, so building does not require Git.
# To build against a local checkout instead (offline work, patching JUCE),
# pass e.g. -DFETCHCONTENT_SOURCE_DIR_JUCE=C:/src/JUCE at configure time.

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- JUCE 9.0.3 (AGPLv3 / commercial) -----------------------------------------
# Since JUCE 8.0.11 the VST3 SDK (MIT) and the ASIO SDK (GPLv3) ship inside
# JUCE, so no separate Steinberg downloads are needed.
if(MERIDIAN_BUILD_APP)
    FetchContent_Declare(JUCE
        URL https://github.com/juce-framework/JUCE/archive/refs/tags/9.0.3.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# --- Rubber Band 4.0.0 (GPLv2-or-later / commercial) --------------------------
# Built from its official single-file compilation unit, which uses Rubber Band's
# built-in FFT and resampler, so there are no further dependencies.
if(MERIDIAN_BUILD_APP AND MERIDIAN_WITH_RUBBERBAND)
    FetchContent_Declare(rubberband
        URL https://github.com/breakfastquay/rubberband/archive/refs/tags/v4.0.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(rubberband)

    add_library(rubberband_single STATIC
        "${rubberband_SOURCE_DIR}/single/RubberBandSingle.cpp")
    target_include_directories(rubberband_single SYSTEM PUBLIC "${rubberband_SOURCE_DIR}")
    if(MSVC)
        target_compile_definitions(rubberband_single PRIVATE _USE_MATH_DEFINES NOMINMAX)
        target_compile_options(rubberband_single PRIVATE /W0 /utf-8)
    else()
        target_compile_options(rubberband_single PRIVATE -w)
    endif()
    if(APPLE)
        target_link_libraries(rubberband_single PUBLIC "-framework Accelerate")
    endif()
    set_target_properties(rubberband_single PROPERTIES FOLDER "ThirdParty")
    add_library(Meridian::rubberband ALIAS rubberband_single)
endif()

# --- LAME 3.100 (LGPLv2-or-later), MP3 export --------------------------------
# libmp3lame only (no decoder, no command-line frontend), as plain C without
# the optional SSE kernels. LAME's own build uses autoconf, so a portable
# config.h is written here instead. Pinned by hash: SourceForge mirrors vary.
if(MERIDIAN_BUILD_APP AND MERIDIAN_WITH_LAME)
    FetchContent_Declare(lame
        URL https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz
        URL_HASH SHA256=ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(lame) # no CMakeLists.txt inside: this only downloads and unpacks

    set(MERIDIAN_LAME_CONFIG_DIR "${CMAKE_BINARY_DIR}/lame-config")
    file(WRITE "${MERIDIAN_LAME_CONFIG_DIR}/config.h" [=[
/* Written by Meridian's cmake/Dependencies.cmake for libmp3lame (any platform). */
#ifndef MERIDIAN_LAME_CONFIG_H
#define MERIDIAN_LAME_CONFIG_H
#define STDC_HEADERS 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDINT_H 1
#define HAVE_STRCHR 1
#define HAVE_MEMCPY 1
#define PACKAGE "lame"
#define PROTOTYPES 1
#define USE_FAST_LOG 1
#define LAME_LIBRARY_BUILD 1
typedef float       ieee754_float32_t;
typedef double      ieee754_float64_t;
typedef long double ieee854_float80_t;
#endif
]=])

    set(MERIDIAN_LAME_SOURCES
        VbrTag.c bitstream.c encoder.c fft.c gain_analysis.c id3tag.c lame.c newmdct.c presets.c psymodel.c
        quantize.c quantize_pvt.c reservoir.c set_get.c tables.c takehiro.c util.c vbrquantize.c version.c)
    list(TRANSFORM MERIDIAN_LAME_SOURCES PREPEND "${lame_SOURCE_DIR}/libmp3lame/")
    add_library(mp3lame STATIC ${MERIDIAN_LAME_SOURCES})
    target_include_directories(mp3lame SYSTEM PUBLIC "${lame_SOURCE_DIR}/include")
    target_include_directories(mp3lame PRIVATE "${MERIDIAN_LAME_CONFIG_DIR}" "${lame_SOURCE_DIR}/libmp3lame")
    target_compile_definitions(mp3lame PRIVATE HAVE_CONFIG_H)
    if(MSVC)
        target_compile_definitions(mp3lame PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(mp3lame PRIVATE /W0)
    else()
        target_compile_options(mp3lame PRIVATE -w)
        target_link_libraries(mp3lame PUBLIC m)
    endif()
    set_target_properties(mp3lame PROPERTIES FOLDER "ThirdParty" POSITION_INDEPENDENT_CODE ON)
    add_library(Meridian::lame ALIAS mp3lame)
endif()

# --- Catch2 3.16.1 (BSL-1.0), tests only --------------------------------------
if(MERIDIAN_BUILD_TESTS)
    FetchContent_Declare(Catch2
        URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.1.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(Catch2)
endif()
