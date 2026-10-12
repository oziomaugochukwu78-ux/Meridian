cmake_minimum_required(VERSION 3.25)

# "Meridian" is a working title. The product name shown to users is set once,
# below, in MERIDIAN_PRODUCT_NAME.
project(Meridian
    VERSION 0.1.0
    DESCRIPTION "Open-source digital audio workstation with an AI remote-control layer"
    LANGUAGES C CXX)

set(MERIDIAN_PRODUCT_NAME "Meridian" CACHE STRING "Application name shown to users")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(MERIDIAN_BUILD_APP            "Build the application (off = core library + tests only, no JUCE)" ON)
option(MERIDIAN_BUILD_TESTS          "Build the unit tests (Catch2)"                            ON)
option(MERIDIAN_WITH_RUBBERBAND      "Build Elastic Audio time-stretching with Rubber Band"     ON)
option(MERIDIAN_ENABLE_ASIO          "Enable the ASIO driver type on Windows"                   ON)
option(MERIDIAN_ENABLE_AVX2          "Compile x86-64 code with AVX2 (requires a 2013+ CPU)"      OFF)
option(MERIDIAN_WARNINGS_AS_ERRORS   "Treat compiler warnings in Meridian's own code as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(Dependencies)

add_subdirectory(src/core)
if(MERIDIAN_BUILD_APP)
    if(MERIDIAN_BUILD_TESTS)
        add_subdirectory(tests/plugin) # test VST3 for the self test; defined first so src/ can refer to it
    endif()
    add_subdirectory(src)
endif()

if(MERIDIAN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
