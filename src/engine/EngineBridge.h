#pragma once

#include "AudioEngine.h"
#include "DiskStreamer.h"

#include "model/Session.h"
#include "plugins/PluginHost.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <functional>
#include <map>
#include <memory>

namespace meridian
{

/** Connects the session document to the real-time engine.

    - Mixer controls (volume, pan, mute, solo, arm) are copied straight into
      each track's TrackRealtimeState atomics: no rebuild, effective next block.
    - Structural edits (tracks, clips, fades, inserts, automation) mark the
      graph dirty; one rebuild runs per message-loop pass however many edits
      happened, then the new RenderGraph is published to the engine.
    - Owns the live plugin instances and the disk streams, keyed so they
      survive rebuilds.
    - Runs recording: creates the takes, and once the audio thread has let go
      of them, turns them into clips.

    Message thread only, except engineAboutToStart (device thread). */
class EngineBridge final : public AudioEngine::Listener,
                           private juce::ValueTree::Listener,
                           private juce::AsyncUpdater,
                           private juce::Timer
{
public:
    EngineBridge (AudioEngine&, DiskStreamer&, juce::AudioFormatManager&, PluginHost&);
    ~EngineBridge() override;

    /** Attach the current session (nullptr to detach before deleting it). */
    void attachSession (Session*);

    std::shared_ptr<TrackRealtimeState> getTrackState (int trackId) const;

    // Inserts --------------------------------------------------------------------
    std::shared_ptr<InsertSlot> getInsert (int trackId, int slot) const;
    juce::ValueTree getInsertNode (const juce::ValueTree& track, int slot) const;

    /** Adds (or replaces) the plugin in a slot as an undoable edit. */
    bool insertPlugin (juce::ValueTree track, int slot, const juce::PluginDescription&, juce::String& error);
    void removePlugin (juce::ValueTree track, int slot);
    void setPluginBypassed (juce::ValueTree track, int slot, bool bypassed);

    /** Writes every plugin's current state into the session tree (before saving). */
    void storePluginStates();

    // Recording --------------------------------------------------------------------
    juce::Result startRecording();
    void stopRecording();
    bool isRecording() const noexcept { return ! activeTakes.empty(); }

    // Disk cache: frames per stream half (read-ahead window).
    void setDiskCacheFrames (int frames);
    int getDiskCacheFrames() const noexcept { return diskCacheFrames; }

    /** Notified of problems the user should hear about (missing files, plugin failures). */
    std::function<void (const juce::String&)> onProblem;

    /** Rebuilds immediately instead of waiting for the next message-loop pass. */
    void rebuildNow();

    // Offline rendering ----------------------------------------------------------------
    /** Takes the live engine off the session (it renders silence) so plug-in
        instances can be used by an offline render. Returns once the audio
        thread has let go of the session graph (or no device is running). */
    bool suspendLive();
    void resumeLive();
    bool isLiveSuspended() const noexcept { return liveSuspended; }

    /** A render graph for an offline engine, with its own disk streams that are
        NOT registered with the disk thread (the offline thread reads them). */
    std::unique_ptr<RenderGraph> buildOfflineGraph (double sampleRate, int maxBlockSize);

private:
    // AudioEngine::Listener
    void engineAboutToStart (double sampleRate, int maxBlockSize) override;

    // ValueTree::Listener
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int index) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;

    void handleAsyncUpdate() override;
    void timerCallback() override;

    void rebuildGraph();
    std::unique_ptr<RenderGraph> buildGraph (double sampleRate, int maxBlockSize, bool sharedStreams,
                                             std::vector<std::shared_ptr<ClipStream>>& usedStreams,
                                             std::map<juce::String, std::shared_ptr<ClipStream>>& keep);
    void buildSegments (const juce::ValueTree& track, TrackRender&, bool sharedStreams,
                        std::vector<std::shared_ptr<ClipStream>>& usedStreams,
                        std::map<juce::String, std::shared_ptr<ClipStream>>& keep);
    std::shared_ptr<ClipStream> getOrCreateStream (const juce::String& key, const juce::File&, bool shared);

    std::shared_ptr<TrackRealtimeState> ensureTrackState (const juce::ValueTree& track);
    void syncTrackState (const juce::ValueTree& track);
    std::shared_ptr<SendRealtimeState> ensureSendState (const juce::ValueTree& sendNode);
    void syncSendState (const juce::ValueTree& sendNode);
    void addTrackResources (const juce::ValueTree& track);
    void removeTrackResources (int trackId);
    void instantiateInsert (const juce::ValueTree& insertNode);
    void finaliseTake (RecordTake&);

    AudioEngine& engine;
    DiskStreamer& diskStreamer;
    juce::AudioFormatManager& formatManager;
    PluginHost& pluginHost;
    Session* session = nullptr;

    std::map<int, std::shared_ptr<TrackRealtimeState>> trackStates;
    std::map<std::pair<int, int>, std::shared_ptr<SendRealtimeState>> sendStates; // (trackId, slot)
    std::map<std::pair<int, int>, std::shared_ptr<InsertSlot>> insertSlots;  // (trackId, slot)
    std::map<juce::String, std::shared_ptr<ClipStream>> streams;
    juce::StringArray reportedMissingFiles;

    std::map<int, std::shared_ptr<RecordTake>> activeTakes;
    std::vector<std::shared_ptr<RecordTake>> takesToFinalise;
    juce::TimeSliceThread recordWriterThread { "Record Writer" };

    juce::CriticalSection pluginLock; // plugin (re)preparation vs. device restarts
    std::unique_ptr<juce::AudioPluginInstance> preparedInstance; // handed from insertPlugin() to the listener
    std::pair<int, int> preparedKey { -1, -1 };
    int diskCacheFrames = 32768;
    bool liveSuspended = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EngineBridge)
};

} // namespace meridian
