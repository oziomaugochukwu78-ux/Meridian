# meridian_core: dependency-free C++20 library holding everything that can be
# tested without JUCE, audio hardware or a GUI: the mixing maths, the console
# saturation curve, fader/meter scales, automation evaluation, and the remote
# control protocol (WebSocket framing + command language).

add_library(meridian_core STATIC
    dsp/AutomationCurve.cpp
    dsp/AutomationCurve.h
    dsp/DcBlocker.h
    dsp/DelayLine.h
    dsp/FaderTaper.cpp
    dsp/FloatCompare.h
    dsp/FaderTaper.h
    dsp/LinearRamp.h
    dsp/MeterScale.h
    dsp/MixKernels.cpp
    dsp/MixKernels.h
    dsp/PanLaw.h
    dsp/SaturationCurve.cpp
    dsp/SaturationCurve.h
    dsp/SignalAnalysis.cpp
    dsp/SignalAnalysis.h
    dsp/SimdConfig.h

    midi/NoteSequence.cpp
    midi/NoteSequence.h
    midi/TempoMap.cpp
    midi/TempoMap.h

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
