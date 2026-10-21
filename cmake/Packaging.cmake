# Installable package: a portable ZIP containing the application, a quick-start
# guide, the documentation and the licences of everything inside the binary.
#
#   cmake --build build/dist --config Release
#   cpack --config build/dist/CPackConfig.cmake -C Release -B build/dist/package

# Everything Meridian ships is in the "app" component. JUCE also defines install
# rules (its headers and build tools, for developers); CPACK_INSTALL_CMAKE_PROJECTS
# below packages only "app", so none of that ends up in the ZIP.
install(TARGETS Meridian
    RUNTIME DESTINATION . COMPONENT app
    BUNDLE DESTINATION . COMPONENT app)

install(FILES
    "${PROJECT_SOURCE_DIR}/packaging/QUICKSTART.txt"
    "${PROJECT_SOURCE_DIR}/README.md"
    "${PROJECT_SOURCE_DIR}/ROADMAP.md"
    "${PROJECT_SOURCE_DIR}/CHANGELOG.md"
    DESTINATION . COMPONENT app)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION . RENAME LICENSE.txt COMPONENT app)
if(WIN32)
    # Adds Meridian to Claude Desktop's config (either the installer or the Store version).
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/Connect to Claude.cmd"
                  "${PROJECT_SOURCE_DIR}/packaging/Connect to Claude.ps1"
            DESTINATION . COMPONENT app)
endif()

# AGPL section 6: say where the exact source of this build is. The commit is read at
# configure time; "-dirty" marks a build from uncommitted changes (not for release).
find_package(Git QUIET)
set(MERIDIAN_SOURCE_COMMIT "unknown")
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --abbrev=40
                    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                    OUTPUT_VARIABLE MERIDIAN_SOURCE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
endif()
configure_file("${PROJECT_SOURCE_DIR}/packaging/SOURCE.txt.in" "${CMAKE_BINARY_DIR}/SOURCE.txt" @ONLY)
install(FILES "${CMAKE_BINARY_DIR}/SOURCE.txt" DESTINATION . COMPONENT app)

install(FILES "${juce_SOURCE_DIR}/LICENSE.md" DESTINATION licenses RENAME JUCE-LICENSE.md COMPONENT app)
install(FILES "${juce_SOURCE_DIR}/JUCE.spdx.json" DESTINATION licenses COMPONENT app)

# Libraries that JUCE compiles into the binary, whose licences ask for their notices
# to travel with it.
set(_juce_modules "${juce_SOURCE_DIR}/modules")
set(_third_party
    "zlib|juce_core/zip/zlib/LICENSE"
    "FLAC|juce_audio_formats/codecs/flac/Flac Licence.txt"
    "Ogg|juce_audio_formats/codecs/ogg/COPYING"
    "Vorbis|juce_audio_formats/codecs/vorbis/COPYING"
    "Opus|juce_audio_formats/codecs/opus/opus/COPYING"
    "Opusfile|juce_audio_formats/codecs/opus/opusfile/COPYING"
    "libopusenc|juce_audio_formats/codecs/opus/libopusenc/COPYING"
    "libpng|juce_graphics/image_formats/pnglib/LICENSE"
    "IJG-JPEG|juce_graphics/image_formats/jpglib/README"
    "libwebp|juce_graphics/image_formats/libwebp/COPYING"
    "HarfBuzz|juce_graphics/fonts/harfbuzz/COPYING"
    "SheenBidi|juce_graphics/unicode/sheenbidi/LICENSE"
    "LunaSVG|juce_graphics/drawables/lunasvg/LICENSE"
    "PlutoVG|juce_graphics/drawables/lunasvg/plutovg/LICENSE"
    "LV2|juce_audio_processors_headless/format_types/LV2_SDK/lv2/COPYING"
    "lilv|juce_audio_processors_headless/format_types/LV2_SDK/lilv/COPYING"
    "serd|juce_audio_processors_headless/format_types/LV2_SDK/serd/COPYING"
    "sord|juce_audio_processors_headless/format_types/LV2_SDK/sord/COPYING"
    "sratom|juce_audio_processors_headless/format_types/LV2_SDK/sratom/COPYING")
foreach(_entry IN LISTS _third_party)
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _name)
    list(GET _parts 1 _path)
    install(FILES "${_juce_modules}/${_path}" DESTINATION licenses/third-party
            RENAME "${_name}.txt" COMPONENT app)
endforeach()
install(FILES "${juce_SOURCE_DIR}/modules/juce_audio_devices/native/asio/LICENSE.txt"
        DESTINATION licenses RENAME ASIO-SDK-LICENSE.txt COMPONENT app)
install(FILES "${juce_SOURCE_DIR}/modules/juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt"
        DESTINATION licenses RENAME VST3-SDK-LICENSE.txt COMPONENT app)
if(MERIDIAN_WITH_RUBBERBAND)
    install(FILES "${rubberband_SOURCE_DIR}/COPYING" DESTINATION licenses RENAME RubberBand-COPYING.txt COMPONENT app)
endif()
if(MERIDIAN_WITH_LAME)
    install(FILES "${lame_SOURCE_DIR}/COPYING" DESTINATION licenses RENAME LAME-COPYING.txt COMPONENT app)
endif()

set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};${PROJECT_NAME};app;/")

set(CPACK_PACKAGE_NAME "${MERIDIAN_PRODUCT_NAME}")
set(CPACK_PACKAGE_VENDOR "Meridian Project")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
if(WIN32)
    set(CPACK_SYSTEM_NAME "windows-x64")
elseif(APPLE)
    set(CPACK_SYSTEM_NAME "macos")
else()
    set(CPACK_SYSTEM_NAME "linux-x64")
endif()
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${CPACK_SYSTEM_NAME}")
set(CPACK_GENERATOR ZIP)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
