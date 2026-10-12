#pragma once

#include "MasterBusProcessor.h"
#include "RenderGraph.h"
#include "SpscQueue.h"
#include "Transport.h"

#include <dsp/Loudness.h>
#include <dsp/Metronome.h>

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace meridian
{

/** The real-time renderer.

    THREADING MODEL
    ---------------
    - Audio thread: audioDeviceIOCallbackWithContext(). Never locks, allocates,
      frees or blocks. Reads only the current RenderGraph, atomics and its own
      preallocated scratch.
    - Message thread: builds RenderGraphs and publishes them with
      publishGraph(). The audio thread adopts the newest one at the start of
      a block (one atomic exchange) and hands the old one back through a
      wait-free queue; a timer on the message thread deletes it. Freeing
      plugin instances, file readers and buffers therefore always happens
      off the audio thread.
    - Device thread: audioDeviceAboutToStart()/Stopped(), called while no
      audio callbacks are running.

    SIGNAL PATH (per track, per block)
    ----------------------------------
      clips from disk / live input -> insert plugins (float)
        -> fader x mute/solo x pan, ramped (double coefficients)
        -> compensated double-double summing bus
      master: resolve -> console saturation (4x oversampled) -> master fader -> device
*/
class AudioEngine final : public juce::AudioIODeviceCallback,
                          private juce::Timer
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;
        /** Device thread, no callbacks running: re-prepare plugins, rebuild graphs. */
        virtual void engineAboutToStart (double sampleRate, int maxBlockSize) = 0;
    };

    AudioEngine();
    ~AudioEngine() override;

    void setListener (Listener* l) noexcept { listener = l; }

    /** Message thread. Takes ownership; the engine switches to it at the next block. */
    void publishGraph (std::unique_ptr<RenderGraph> graph);

    Transport& getTransport() noexcept               { return transport; }
    const Transport& getTransport() const noexcept   { return transport; }
    MasterBusProcessor& getMasterBus() noexcept      { return masterBus; }

    double getSampleRate() const noexcept            { return sampleRate.load(); }
    int getMaxBlockSize() const noexcept             { return maxBlockSize.load(); }
    bool isDeviceRunning() const noexcept            { return deviceRunning.load(); }

    std::atomic<double> masterVolumeDb { 0.0 };

    /** The click (metronome), mixed into the output after the master fader
        while the transport rolls and during a count-in. Never in bounces. */
    std::atomic<bool> clickEnabled { false };
    std::atomic<double> clickBpm { 120.0 };
    std::atomic<int> clickBeatsPerBar { 4 };
    std::atomic<double> clickLevelDb { -6.0 };

    //==============================================================================
    // Live MIDI input (a MIDI keyboard)

    /** Any thread (MIDI device callbacks). timeSeconds is
        juce::Time::getMillisecondCounterHiRes() / 1000 when the message
        arrived. Record-armed instrument tracks play it; while recording it is
        also captured. Each note sounds one block after it arrived, at its
        exact relative position (constant latency, no jitter). */
    void pushLiveMidi (int pitch, float velocity, bool noteOn, double timeSeconds) noexcept;

    /** A note captured on a record-armed instrument track while recording, at
        the timeline sample where it was played into the engine. */
    struct RecordedMidi
    {
        int trackId = 0;
        juce::int64 time = 0;
        int pitch = 60;
        float velocity = 0.0f;
        bool on = false;
    };

    /** Message thread: the next captured note, if any. */
    bool popRecordedMidi (RecordedMidi& out) noexcept { return recordedMidi.pop (out); }
    /** Captured notes lost because the message thread fell behind (should stay 0). */
    int collectRecordedMidiOverflows() noexcept { return recordedMidiOverflows.exchange (0); }

    /** Loudness metering of the master output (the click is not included):
        one entry per 100 ms, the K-weighted mean square summed over both
        channels (BS.1770) and the true peak in that time. */
    struct LoudnessBlock
    {
        double meanSquare = 0.0;
        float truePeak = 0.0f;
    };
    /** UI thread: the next 100 ms block, if any (feed it to a dsp::LoudnessMeter). */
    bool popLoudnessBlock (LoudnessBlock& out) noexcept { return loudnessBlocks.pop (out); }

    /** UI: peaks of the master output since the last call. */
    std::pair<float, float> collectMasterPeaks() noexcept;

    /** UI: number of blocks with missing disk data since the last call. */
    int collectDiskUnderruns() noexcept { return diskUnderruns.exchange (0); }

    /** Diagnostics: the longest device callback since the last call, as a
        fraction of the time the buffer allows (1.0 = the whole budget; above
        that, Meridian itself caused a dropout). Lets dropouts be attributed to
        the engine or to the OS/driver. */
    double collectWorstCallbackLoad() noexcept { return worstCallbackLoad.exchange (0.0); }

    /** Diagnostics: device callbacks that overran their budget since the last call. */
    int collectCallbackOverruns() noexcept { return callbackOverruns.exchange (0); }

    /** True while a published graph has not yet been picked up by the audio thread. */
    bool hasPendingGraph() const noexcept { return pendingGraph.load() != nullptr; }

    /** Total delay from timeline to output, in samples: compensated plug-in
        latency of the newest graph plus the master console's oversampling. */
    int getTotalLatencySamples() const noexcept { return graphLatency.load() + masterBus.getLatencySamples(); }
    int getPluginLatencySamples() const noexcept { return graphLatency.load(); }

    //==============================================================================
    // Offline rendering (bounce). An engine used this way is never attached to a
    // device; one thread calls prepare() and then renderOffline() repeatedly.

    /** Allocates scratch and prepares the master bus. No callbacks may be running. */
    void prepare (double sampleRate, int maxBlockSize);

    /** Offline mode reads disk synchronously (never drops data) instead of
        relying on the disk thread's read-ahead. */
    void setOfflineMode (bool shouldBeOffline) noexcept { offline = shouldBeOffline; }

    /** Renders the next numSamples of the transport into `outputs` (no inputs). */
    void renderOffline (float* const* outputs, int numOutputs, int numSamples) noexcept;

    //==============================================================================
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

private:
    struct BlockContext
    {
        const float* const* inputs;
        int numInputs;
        int inputOffset;
        int numSamples;
        juce::int64 position;
        Transport::State state;
        bool anySolo;
        bool looping;                 ///< loop playback is on: keep the loop start cued
        juce::int64 loopStart, loopEnd;
    };

    void timerCallback() override;
    void adoptPendingGraph() noexcept;
    void renderBlock (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numSamples) noexcept;
    void processChunk (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                       int offset, int numSamples, Transport::State state, bool countingIn) noexcept;
    void renderClick (int numSamples, juce::int64 gridPosition) noexcept;
    void renderTrack (TrackRender&, const BlockContext&) noexcept;
    void renderSegments (TrackRender&, const BlockContext&) noexcept;
    void renderInstrument (TrackRender&, const BlockContext&) noexcept;
    void writeTake (RecordTake&, const BlockContext&) noexcept;
    void mixTrackIntoBus (TrackRender&, const BlockContext&) noexcept;
    void deleteAllGraphs();

    Transport transport;
    MasterBusProcessor masterBus;
    Listener* listener = nullptr;

    std::atomic<RenderGraph*> pendingGraph { nullptr };
    RenderGraph* activeGraph = nullptr;           // audio thread
    SpscQueue<RenderGraph*, 256> retiredGraphs;   // audio -> message thread

    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<int> maxBlockSize { 0 };
    std::atomic<bool> deviceRunning { false };
    std::atomic<int> graphLatency { 0 };
    bool offline = false;

    // Audio-thread scratch, sized in audioDeviceAboutToStart.
    std::vector<double> busSum[2], busComp[2], masterOut[2];
    std::vector<double> faderGains, muteGains, totalGains, sendGains, panLeft, panRight, coefLeft, coefRight, busScratch;
    void drainLiveMidi (int numSamples) noexcept;
    void meterLoudness (int numSamples) noexcept;

    std::array<dsp::KWeighting, 2> loudnessWeighting;
    dsp::TruePeakMeter loudnessPeak;
    double loudnessSum = 0.0;
    int loudnessFill = 0, loudnessBlockLength = 4800;
    SpscQueue<LoudnessBlock, 128> loudnessBlocks;  // audio thread -> UI, 12.8 s of slack

    struct LiveNote { int pitch = 60; float velocity = 0.0f; bool on = false; double time = 0.0; };
    struct BlockNote { int offset = 0; int pitch = 60; float velocity = 0.0f; bool on = false; };
    juce::SpinLock liveMidiProducers;              // serialises MIDI device threads; never the audio thread
    SpscQueue<LiveNote, 1024> liveMidiIn;          // MIDI threads -> audio thread
    std::array<BlockNote, 256> blockNotes {};      // this block's live notes, offsets within the block
    int numBlockNotes = 0;
    SpscQueue<RecordedMidi, 8192> recordedMidi;    // audio thread -> message thread
    std::atomic<int> recordedMidiOverflows { 0 };

    std::vector<float> silence, clickScratch;
    dsp::Metronome metronome;
    dsp::Metronome::Params appliedClick;
    std::vector<float> delayedMain[2], delayedSend[2];   // plug-in delay compensation output
    dsp::LinearRamp masterFader;

    std::atomic<float> masterPeakLeft { 0.0f }, masterPeakRight { 0.0f };
    std::atomic<int> diskUnderruns { 0 };
    std::atomic<double> worstCallbackLoad { 0.0 };
    std::atomic<int> callbackOverruns { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace meridian
