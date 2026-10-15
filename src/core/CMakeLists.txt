# meridian_core: dependency-free C++20 library holding everything that can be
# tested without JUCE, audio hardware or a GUI: the mixing maths, the console
# saturation curve, fader/meter scales, automation evaluation, and the remote
# control protocol (WebSocket framing + command language).

add_library(meridian_core STATIC
    dsp/AutomationCurve.cpp
    dsp/AutomationCurve.h
    dsp/AutomationWriter.cpp
    dsp/AutomationWriter.h
    dsp/Biquad.cpp
    dsp/Biquad.h
    dsp/Compressor.cpp
    dsp/Compressor.h
    dsp/DcBlocker.h
    dsp/DeEsser.cpp
    dsp/DeEsser.h
    dsp/Dither.h
    dsp/DelayLine.h
    dsp/FaderTaper.cpp
    dsp/FdnReverb.cpp
    dsp/FdnReverb.h
    dsp/FloatCompare.h
    dsp/Gate.cpp
    dsp/Gate.h
    dsp/FaderTaper.h
    dsp/Limiter.cpp
    dsp/Limiter.h
    dsp/LinearRamp.h
    dsp/Loudness.cpp
    dsp/Loudness.h
    dsp/MeterScale.h
    dsp/Metronome.cpp
    dsp/Metronome.h
    dsp/PitchDetector.cpp
    dsp/PitchDetector.h
    dsp/MixKernels.cpp
    dsp/MixKernels.h
    dsp/PanLaw.h
    dsp/ParametricEq.cpp
    dsp/ParametricEq.h
    dsp/SaturationCurve.cpp
    dsp/SaturationCurve.h
    dsp/SignalAnalysis.cpp
    dsp/SilenceDetector.cpp
    dsp/SilenceDetector.h
    dsp/SignalAnalysis.h
    dsp/SimdConfig.h
    dsp/StereoDelay.cpp
    dsp/StereoDelay.h

    midi/MeterMap.cpp
    midi/MeterMap.h
    midi/MidiRecording.cpp
    midi/MidiRecording.h
    midi/NoteSequence.cpp
    midi/NoteSequence.h
    midi/TempoMap.cpp
    midi/TempoMap.h

    synth/DrumSynth.cpp
    synth/DrumSynth.h
    synth/PolySynth.cpp
    synth/PolySynth.h

    remote/Base64.cpp
    remote/Base64.h
    remote/CommandDispatcher.cpp
    remote/CommandDispatcher.h
    remote/CommandParser.cpp
    remote/CommandParser.h
    remote/JsonLimits.cpp
    remote/JsonLimits.h
    remote/JsonWriter.h
    remote/RemoteCommand.h
    remote/Sha1.cpp
    remote/Sha1.h
    remote/WebSocketProtocol.cpp
    remote/WebSocketProtocol.h
)

target_include_directories(meridian_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(meridian_core PUBLIC cxx_std_20)
meridian_configure_target(meridian_core)
set_target_properties(meridian_core PROPERTIES FOLDER "Meridian")
add_library(Meridian::core ALIAS meridian_core)
