#pragma once

#include "ClipStream.h"
#include "TrackRealtimeState.h"

#include <dsp/AutomationCurve.h>
#include <dsp/DelayLine.h>
#include <midi/MeterMap.h>
#include <synth/DrumSynth.h>
#include <synth/PolySynth.h>

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <vector>

namespace meridian
{

/** A plugin in a track's insert slot. Shared between the message thread's
    registry and any render graphs that reference it; destroyed on the message
    thread when the last graph using it is retired. */
struct InsertSlot
{
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::PluginDescription description;
    std::atomic<bool> bypassed { false };
    juce::AudioProcessorListener* latencyListener = nullptr; // told when the plug-in's latency changes

    ~InsertSlot()
    {
        if (plugin != nullptr && latencyListener != nullptr)
            plugin->removeListener (latencyListener);
    }
};

/** A take being recorded on one track. The ThreadedWriter's FIFO makes
    write() safe to call from the audio thread; a background thread drains it
    to disk. */
struct RecordTake
{
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
    juce::File file;
    int trackId = 0;
    int numChannels = 1;
    int inputChannel = 0;

    std::atomic<juce::int64> startSample { -1 };   ///< timeline position of the first recorded sample
    std::atomic<juce::int64> samplesWritten { 0 };
    std::atomic<bool> overflowed { false };        ///< the disk could not keep up; samples were lost

    /** Round-trip delay (device input + output + engine latency) between the
        performer hearing a timeline position and their sound reaching us.
        The finished take is moved earlier by this much. */
    juce::int64 latencyCompensation = 0;
};

/** The audible part of one clip. Overlapping clips are resolved when the graph
    is built (later clips cover earlier ones), so a clip may contribute several
    segments; fades are always evaluated against the original clip bounds. */
struct ClipSegment
{
    juce::int64 start = 0;          ///< timeline
    juce::int64 length = 0;
    juce::int64 sourceOffset = 0;   ///< file position at `start`
    float gain = 1.0f;

    juce::int64 clipStart = 0;      ///< original clip bounds, for fades
    juce::int64 clipEnd = 0;
    juce::int64 fadeIn = 0;
    juce::int64 fadeOut = 0;

    std::shared_ptr<ClipStream> stream;
};

/** A plug-in delay compensation delay for one signal path. The DelayLine
    object outlives graph rebuilds (it is kept by EngineBridge), so editing
    during playback never empties it; `samples` is this graph's amount. */
struct PathDelay
{
    std::shared_ptr<dsp::DelayLine> line;
    int samples = 0;
};

/** One note-on or note-off on an instrument track, in timeline samples. */
struct MidiEvent
{
    juce::int64 time = 0;
    int pitch = 60;     ///< or midi::sustainPedal (on = pedal down)
    float velocity = 0.0f;
    bool on = false;
};

/** An instrument track's synth and its playback bookkeeping. Kept by
    EngineBridge across graph rebuilds (so editing during playback never cuts
    off sounding notes) and touched only by the audio thread once a graph
    referencing it is published. PolySynth::prepare/setParams allocate
    nothing, so the audio thread can apply them itself. */
struct InstrumentState
{
    synth::PolySynth synth;
    synth::DrumSynth drums;        // used instead of `synth` on drum tracks
    double preparedRate = 0.0;
    uint64_t appliedParamsVersion = 0;
    juce::int64 nextPosition = -1; ///< where playback continues if nothing jumps; -1 = not rolling
};

struct SendRender
{
    std::shared_ptr<SendRealtimeState> state;
    int targetBus = -1;                            ///< index into RenderGraph::buses
    int slot = 0;                                  ///< A..E as 0..4
    PathDelay compensation;
    dsp::AutomationCurve levelAutomation;          ///< dB, interpolated linearly in dB
};

/** Automation of one plug-in parameter (normalised 0..1), applied at the start
    of each block. The parameter belongs to an insert this graph keeps alive. */
struct ParameterAutomation
{
    juce::AudioProcessorParameter* parameter = nullptr;
    dsp::AutomationCurve curve;
    float lastValue = -1.0f;                       ///< audio thread: what was last applied
};

/** The input of an aux track: a stereo compensated (double-double) sum,
    exactly like the master bus, cleared every block. */
struct BusInput
{
    std::vector<double> sum[2], comp[2];
};

struct TrackRender
{
    std::shared_ptr<TrackRealtimeState> state;
    int numChannels = 1;
    int inputChannel = 0;                          ///< -1 = no input

    // Routing
    bool isAux = false;
    int ownBus = -1;                               ///< aux tracks: the BusInput they read
    int outputBus = -1;                            ///< -1 = master, else a BusInput index
    PathDelay outputCompensation;                  ///< aligns this track's output with the slowest path into the same bus
    std::vector<SendRender> sends;
    /** Aux tracks: every track that feeds this bus (directly or through other
        buses). If one of them is soloed, the aux stays audible ("implicit solo"). */
    std::vector<std::shared_ptr<TrackRealtimeState>> upstream;
    std::vector<ClipSegment> segments;             ///< sorted by start

    // Instrument tracks: the synth, its settings in this graph, and every note
    // event sorted by time (note-offs before note-ons at the same sample, so a
    // repeated note retriggers cleanly).
    std::shared_ptr<InstrumentState> instrument;
    bool drumKit = false;                          ///< play `drums` instead of `synth`
    /** A plug-in instrument (VST3/AU...) in insert A plays the MIDI instead of the
        built-in synth: it receives `midi` for the block, sample-accurate. */
    bool pluginInstrument = false;
    synth::PolySynth::Params synthParams;
    synth::DrumSynth::Params drumParams;
    uint64_t synthParamsVersion = 0;
    std::vector<MidiEvent> events;

    std::vector<std::shared_ptr<InsertSlot>> inserts;
    std::shared_ptr<RecordTake> take;
    dsp::AutomationCurve volumeAutomation;         ///< values in dB, interpolated linearly in dB
    dsp::AutomationCurve panAutomation;            ///< -1..+1, sample-accurate
    dsp::AutomationCurve muteAutomation;           ///< 0 or 1, applied per block with the 5 ms declick ramp
    std::vector<ParameterAutomation> parameterAutomation;

    // Scratch, sized for the graph's maxBlockSize when the graph is built.
    juce::AudioBuffer<float> buffer;
    juce::AudioBuffer<float> clipScratch;
    juce::MidiBuffer midi;
};

/** Immutable snapshot of everything the audio thread needs to render.

    Built on the message thread from the session tree, published to the engine
    through a single atomic pointer, and retired back to the message thread for
    deletion. The audio thread never allocates, frees or locks because of it. */
struct RenderGraph
{
    /** Render order: every track comes before any aux it feeds, so a bus's
        input is complete by the time the bus itself is processed. */
    std::vector<TrackRender> tracks;
    std::vector<BusInput> buses;
    int maxBlockSize = 0;
    double sampleRate = 0.0;

    /** Plug-in latency of the slowest path to the master bus, after
        compensation: everything reaches the master this much late. */
    int compensatedLatency = 0;

    /** Where the click's beats fall under the session's tempo and meter maps
        (null: the engine's plain click settings). Freed with the graph, on the
        message thread. */
    std::shared_ptr<const midi::ClickGrid> clickGrid;
};

} // namespace meridian
