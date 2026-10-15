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
    DESTINATION . COMPONENT app)

install(FILES "${juce_SOURCE_DIR}/LICENSE.md" DESTINATION licenses RENAME JUCE-LICENSE.md COMPONENT app)
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
