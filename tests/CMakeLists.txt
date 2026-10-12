# Unit tests for meridian_core. They need neither JUCE nor audio hardware,
# so they build in seconds and run anywhere (including CI).

add_executable(meridian_core_tests
    dsp/AutomationCurveTests.cpp
    dsp/AutomationWriterTests.cpp
    dsp/DelayLineTests.cpp
    dsp/DitherTests.cpp
    dsp/DynamicsTests.cpp
    dsp/EffectsTests.cpp
    dsp/FaderTaperTests.cpp
    dsp/LinearRampTests.cpp
    dsp/LoudnessTests.cpp
    dsp/MetronomeTests.cpp
    dsp/MixKernelsTests.cpp
    dsp/PanLawTests.cpp
    dsp/SaturationCurveTests.cpp
    dsp/SignalAnalysisTests.cpp
    midi/MidiRecordingTests.cpp
    midi/NoteSequenceTests.cpp
    midi/TempoMapTests.cpp
    remote/CommandDispatcherTests.cpp
    remote/CommandParserTests.cpp
    remote/FuzzTests.cpp
    remote/HandshakeTests.cpp
    remote/HashingTests.cpp
    remote/WebSocketReaderTests.cpp
    synth/DrumSynthTests.cpp
    synth/PolySynthTests.cpp
)

target_link_libraries(meridian_core_tests PRIVATE Meridian::core Catch2::Catch2WithMain)
meridian_configure_target(meridian_core_tests)
set_target_properties(meridian_core_tests PROPERTIES FOLDER "Meridian")

include(Catch)
catch_discover_tests(meridian_core_tests)
