#pragma once

#include "engine/AudioEngine.h"
#include "engine/DiskStreamer.h"
#include "engine/EngineBridge.h"
#include "media/Bouncer.h"
#include "model/Session.h"
#include "model/SessionTemplates.h"
#include "plugins/PluginHost.h"
#include "remote/RemoteControlServer.h"

#include <remote/CommandDispatcher.h>

#include <juce_audio_utils/juce_audio_utils.h>

namespace meridian
{

enum class WindowKind { edit, mix };

/** Owns every long-lived service and the current session, and is the single
    implementation of the remote-control surface (RemoteActions), so the AI
    port and the UI drive exactly the same code paths.

    Message thread only. Broadcasts a change message whenever the session is
    replaced, so views can rebind. */
class AppController final : public remote::RemoteActions,
                            public juce::ChangeBroadcaster,
                            private juce::ChangeListener,
                            private juce::ValueTree::Listener
{
public:
    AppController();
    ~AppController() override;

    // Services ------------------------------------------------------------------------
    juce::PropertiesFile& getSettings();
    juce::AudioDeviceManager& getDeviceManager() noexcept   { return deviceManager; }
    juce::AudioFormatManager& getFormatManager() noexcept   { return formatManager; }
    juce::AudioThumbnailCache& getThumbnailCache() noexcept { return thumbnailCache; }
    PluginHost& getPluginHost() noexcept                    { return *pluginHost; }
    AudioEngine& getEngine() noexcept                       { return engine; }
    EngineBridge& getBridge() noexcept                      { return *bridge; }
    RemoteControlServer& getRemoteServer() noexcept         { return remoteServer; }
    juce::RecentlyOpenedFilesList& getRecentSessions() noexcept { return recentSessions; }

    // Session lifecycle ----------------------------------------------------------------
    Session* getSession() const noexcept { return session.get(); }
    juce::UndoManager* getUndoManager() const noexcept { return session != nullptr ? &session->getUndoManager() : nullptr; }

    juce::Result createSession (const juce::File& parentFolder, const Session::Settings&, const SessionTemplate*);
    juce::Result openSession (const juce::File& sessionFile);
    juce::Result saveSession();
    juce::File getDefaultSessionsFolder() const;
    void setRememberRecentSessions (bool shouldRemember) noexcept { rememberRecentSessions = shouldRemember; }

    /** Pushes the session's master fader and console settings into the engine. */
    void applyMasterSettings();

    // Transport ----------------------------------------------------------------------------
    // (Distinct names from the RemoteActions overrides below, which wrap these.)
    juce::Result transportPlay();
    void transportStop();
    void togglePlay();
    juce::Result transportRecord();
    void returnToZero();
    void locate (juce::int64 samplePosition);
    bool isRolling() const noexcept;
    bool isRecording() const noexcept;

    // Editing -------------------------------------------------------------------------------
    void addTracks (int count, int numChannels, const juce::String& baseName);
    void deleteTrack (juce::ValueTree track);

    /** Copies (and converts) files into the session on a background thread,
        then places each on targetTrack (single file) or on a new track. */
    void importAudioFiles (const juce::Array<juce::File>& files, juce::int64 position, juce::ValueTree targetTrack = {});

    /** Elastic Audio render of a clip on a background thread; the clip is
        switched to the rendered file when done (undoable). */
    void renderElasticAudio (juce::ValueTree clip, double timeRatio, double pitchSemitones, bool preserveFormants);

    int getNumBackgroundJobs() const noexcept { return runningJobs.load(); }

    // Bounce (mixdown to a file) ---------------------------------------------------------
    /** Whole session (to the end of the last clip, plus a one-second tail) into
        <session>/Bounced Files/, in the session's format. */
    BounceRequest makeSessionBounceRequest() const;
    juce::int64 getSessionEnd() const;

    /** Renders on the calling (message) thread and returns when done. Live
        playback is paused for the duration. */
    BounceResult bounceNow (const BounceRequest&);

    /** Renders on a background thread; `onDone` runs on the message thread. */
    juce::Result startBounce (const BounceRequest&, std::function<void (const BounceResult&)> onDone = {});

    bool isBouncing() const noexcept           { return offlineEngine != nullptr; }
    double getBounceProgress() const noexcept  { return bounceProgress.load(); }
    const BounceResult& getLastBounce() const noexcept { return lastBounce; }

    // Remote control ----------------------------------------------------------------------
    juce::Result startRemoteControl();
    void stopRemoteControl();

    /** Wired by the main window: performs the Edit/Mix swap and says which is now shown. */
    std::function<WindowKind()> toggleWindowHandler;
    std::function<WindowKind()> activeWindowQuery;

    /** Wired by the main window: shows a non-blocking error/warning to the user. */
    std::function<void (const juce::String& title, const juce::String& message)> showProblem;

    // RemoteActions -------------------------------------------------------------------------
    remote::ActionResult play() override;
    remote::ActionResult stop() override;
    remote::ActionResult record() override;
    remote::ActionResult toggleWindow() override;
    remote::ActionResult toggleTrackMute (int trackNumber) override;
    remote::ActionResult setTrackVolume (int trackNumber, double decibels) override;
    remote::ActionResult bounce() override;
    std::string statusJson() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void installSession (std::unique_ptr<Session>);
    void syncDeviceToSession();
    void reportProblem (const juce::String& title, const juce::String& message);
    juce::String nextTrackName (const juce::String& baseName) const;
    juce::String describePosition() const;
    std::unique_ptr<AudioEngine> createOfflineEngine (juce::String& error);

    // Declaration order is destruction order in reverse: the engine must die
    // before the plug-in host and disk streamer whose objects it references.
    juce::ApplicationProperties properties;
    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;
    juce::AudioThumbnailCache thumbnailCache { 512 };
    std::unique_ptr<PluginHost> pluginHost;
    DiskStreamer diskStreamer;
    AudioEngine engine;
    std::unique_ptr<EngineBridge> bridge;
    std::unique_ptr<AudioEngine> offlineEngine;   // exists only while a background bounce runs
    std::unique_ptr<Session> session;
    RemoteControlServer remoteServer { [this] (const std::string& message) { return remote::handleRemoteMessage (message, *this); } };
    juce::RecentlyOpenedFilesList recentSessions;
    juce::ThreadPool jobs { juce::ThreadPoolOptions{}.withThreadName ("Media Jobs").withNumberOfThreads (2) };

    std::atomic<int> runningJobs { 0 };
    std::atomic<bool> shuttingDown { false };
    std::atomic<double> bounceProgress { 0.0 };
    BounceResult lastBounce;
    int sessionGeneration = 0; // invalidates background results meant for a replaced session
    bool rememberRecentSessions = true;
    std::shared_ptr<bool> lifetime = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppController)
};

} // namespace meridian
