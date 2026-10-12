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

# --- Catch2 3.16.1 (BSL-1.0), tests only --------------------------------------
if(MERIDIAN_BUILD_TESTS)
    FetchContent_Declare(Catch2
        URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.1.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(Catch2)
endif()
