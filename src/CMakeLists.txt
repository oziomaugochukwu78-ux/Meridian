# The Meridian desktop application.

juce_add_gui_app(Meridian
    PRODUCT_NAME "${MERIDIAN_PRODUCT_NAME}"
    COMPANY_NAME "Meridian Project"
    COMPANY_COPYRIGHT "Copyright (C) 2026 the Meridian contributors. Licensed under the GNU AGPL v3."
    BUNDLE_ID "org.meridian-daw.meridian"
    VERSION "${PROJECT_VERSION}"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Meridian records audio from your audio interface.")

target_sources(Meridian PRIVATE
    app/AppController.cpp
    app/AutomationRecorder.cpp
    app/GroupLinker.cpp
    app/GroupLinker.h
    app/AutomationRecorder.h
    app/AppController.h
    app/CommandIds.h
    app/Main.cpp
    app/MainComponent.cpp
    app/MainComponent.h
    app/MainWindow.cpp
    app/MainWindow.h
    app/SelfTest.cpp
    app/SelfTest.h
    app/SoakTest.cpp
    app/SoakTest.h

    engine/AudioEngine.cpp
    engine/AudioEngine.h
    engine/ClipStream.cpp
    engine/ClipStream.h
    engine/DiskStreamer.cpp
    engine/DiskStreamer.h
    engine/EngineBridge.cpp
    engine/EngineBridge.h
    engine/MasterBusProcessor.cpp
    engine/MasterBusProcessor.h
    engine/RenderGraph.h
    engine/SpscQueue.h
    engine/TrackRealtimeState.h
    engine/Transport.h

    model/Automation.cpp
    model/Automation.h
    model/ConsolePresets.cpp
    model/ConsolePresets.h
    model/EditClipboard.cpp
    model/EditClipboard.h
    model/Fades.cpp
    model/Fades.h
    model/Groups.cpp
    model/Groups.h
    model/MidiModel.cpp
    model/MidiModel.h
    model/Playlists.cpp
    model/Playlists.h
    model/Routing.cpp
    model/Routing.h
    model/Session.cpp
    model/Session.h
    model/SessionIds.h
    model/SessionBackups.cpp
    model/SessionBackups.h
    model/SessionRepair.cpp
    model/SessionRepair.h
    model/SessionTemplates.cpp
    model/SessionTemplates.h
    model/Shuffle.cpp
    model/Shuffle.h

    media/AudioImporter.cpp
    media/AudioImporter.h
    media/Bouncer.cpp
    media/Bouncer.h
    media/Mp3Writer.cpp
    media/Mp3Writer.h
    media/StripSilence.cpp
    media/StripSilence.h
    media/TimeStretchEngine.cpp
    media/TimeStretchEngine.h

    plugins/BuiltinEffects.cpp
    plugins/BuiltinEffects.h
    plugins/PluginHost.cpp
    plugins/PluginHost.h
    plugins/PluginWindow.cpp
    plugins/PluginWindow.h

    remote/RemoteControlServer.cpp
    remote/RemoteControlServer.h
    remote/RemoteTools.cpp
    remote/RemoteTools.h

    ui/LevelMeter.cpp
    ui/LevelMeter.h
    ui/MeridianLookAndFeel.cpp
    ui/MeridianLookAndFeel.h
    ui/Theme.h
    ui/TimeFormat.h
    ui/TransportBar.cpp
    ui/TransportBar.h
    ui/dialogs/DashboardComponent.cpp
    ui/dialogs/DashboardComponent.h
    ui/dialogs/DialogUtils.h
    ui/dialogs/NewTracksComponent.cpp
    ui/dialogs/NewTracksComponent.h
    ui/dialogs/PlaybackEngineComponent.cpp
    ui/dialogs/PlaybackEngineComponent.h
    ui/dialogs/TimeStretchComponent.cpp
    ui/dialogs/TimeStretchComponent.h
    ui/edit/AutomationView.h
    ui/edit/ClipComponent.cpp
    ui/edit/ClipComponent.h
    ui/edit/EditState.h
    ui/edit/EditToolbar.cpp
    ui/edit/EditToolbar.h
    ui/edit/EditView.cpp
    ui/edit/EditView.h
    ui/edit/PianoRoll.cpp
    ui/edit/PianoRoll.h
    ui/edit/TimelineRuler.cpp
    ui/edit/TimelineRuler.h
    ui/edit/TrackHeader.cpp
    ui/edit/TrackHeader.h
    ui/edit/TrackLane.cpp
    ui/edit/TrackLane.h
    ui/mix/ChannelStrip.cpp
    ui/mix/ChannelStrip.h
    ui/mix/InsertSlotButton.cpp
    ui/mix/InsertSlotButton.h
    ui/mix/MasterStrip.cpp
    ui/mix/MasterStrip.h
    ui/mix/MixView.cpp
    ui/mix/MixView.h
    ui/mix/SendSlotButton.cpp
    ui/mix/SendSlotButton.h
)

target_include_directories(Meridian PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(Meridian PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Meridian,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Meridian,JUCE_VERSION>"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    # Route assertion failures through juce::Logger, so the self test can fail on
    # them even without a debugger attached (where they would otherwise be silent).
    JUCE_LOG_ASSERTIONS=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1
    JUCE_PLUGINHOST_AU=$<BOOL:${APPLE}>
    JUCE_ASIO=$<AND:$<BOOL:${WIN32}>,$<BOOL:${MERIDIAN_ENABLE_ASIO}>>
    MERIDIAN_WITH_RUBBERBAND=$<BOOL:${MERIDIAN_WITH_RUBBERBAND}>
    MERIDIAN_WITH_LAME=$<BOOL:${MERIDIAN_WITH_LAME}>
    # Reading MP3 (import, and the self test's check of exported MP3s): JUCE's own decoder.
    JUCE_USE_MP3AUDIOFORMAT=1
    # Reference renders for the self test's null tests (see SelfTest::testGoldenRenders).
    MERIDIAN_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden")

target_link_libraries(Meridian
    PRIVATE
        Meridian::core
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(MERIDIAN_WITH_RUBBERBAND)
    target_link_libraries(Meridian PRIVATE Meridian::rubberband)
endif()
if(MERIDIAN_WITH_LAME)
    target_link_libraries(Meridian PRIVATE Meridian::lame)
endif()

# The self test loads the test plug-in from the build tree.
if(TARGET MeridianTestGain_VST3)
    target_compile_definitions(Meridian PRIVATE
        MERIDIAN_TEST_PLUGIN_BINARY="$<TARGET_FILE:MeridianTestGain_VST3>")
    add_dependencies(Meridian MeridianTestGain_VST3)
endif()
if(TARGET MeridianTestSynth_VST3)
    target_compile_definitions(Meridian PRIVATE
        MERIDIAN_TEST_SYNTH_BINARY="$<TARGET_FILE:MeridianTestSynth_VST3>")
    add_dependencies(Meridian MeridianTestSynth_VST3)
endif()

# Strict IEEE semantics in the app as well: the engine includes the core DSP
# headers inline.
if(MSVC)
    target_compile_options(Meridian PRIVATE /fp:precise /utf-8 /permissive-)
else()
    target_compile_options(Meridian PRIVATE -ffp-contract=off)
endif()

# The application builds JUCE's module sources too; JUCE is warning-clean under
# its recommended flags, so the whole target can be held to zero warnings.
if(MERIDIAN_WARNINGS_AS_ERRORS)
    target_compile_options(Meridian PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/WX,-Werror>)
endif()
meridian_configure_simd(Meridian)

set_target_properties(Meridian PROPERTIES FOLDER "Meridian")
