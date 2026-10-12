#pragma once

#include "MasterBusProcessor.h"
#include "RenderGraph.h"
#include "SpscQueue.h"
#include "Transport.h"

#include <juce_audio_devices/juce_audio_devices.h>

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

    /** UI: peaks of the master output since the last call. */
    std::pair<float, float> collectMasterPeaks() noexcept;

    /** UI: number of blocks with missing disk data since the last call. */
    int collectDiskUnderruns() noexcept { return diskUnderruns.exchange (0); }

    /** True while a published graph has not yet been picked up by the audio thread. */
    bool hasPendingGraph() const noexcept { return pendingGraph.load() != nullptr; }

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
    };

    void timerCallback() override;
    void adoptPendingGraph() noexcept;
    void renderBlock (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numSamples) noexcept;
    void processChunk (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                       int offset, int numSamples) noexcept;
    void renderTrack (TrackRender&, const BlockContext&) noexcept;
    void renderSegments (TrackRender&, const BlockContext&) noexcept;
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
    bool offline = false;

    // Audio-thread scratch, sized in audioDeviceAboutToStart.
    std::vector<double> busSum[2], busComp[2], masterOut[2];
    std::vector<double> faderGains, muteGains, totalGains, sendGains, panLeft, panRight, coefLeft, coefRight, busScratch;
    std::vector<float> silence;
    dsp::LinearRamp masterFader;

    std::atomic<float> masterPeakLeft { 0.0f }, masterPeakRight { 0.0f };
    std::atomic<int> diskUnderruns { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace meridian
