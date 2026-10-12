#include "AppController.h"

#include "media/AudioImporter.h"
#include "media/TimeStretchEngine.h"
#include "ui/TimeFormat.h"

#include <remote/JsonWriter.h>

#include <cmath>

namespace meridian
{
namespace
{
constexpr const char* deviceStateKey = "audioDeviceState";
constexpr const char* recentSessionsKey = "recentSessions";
constexpr const char* remoteEnabledKey = "remoteControlEnabled";
constexpr const char* remotePortKey = "remoteControlPort";
constexpr const char* remoteTokenKey = "remoteControlToken";
constexpr const char* remoteOriginsKey = "remoteControlAllowedOrigins";
constexpr const char* diskCacheKey = "diskCacheFrames";
constexpr double faderFloorDb = -144.0;

juce::String trackLabel (int number, const juce::ValueTree& t)
{
    return "Track " + juce::String (number) + " \"" + track::name (t) + "\"";
}
} // namespace

AppController::AppController()
{
    juce::PropertiesFile::Options options;
    options.applicationName = JUCE_APPLICATION_NAME_STRING;
    options.filenameSuffix = ".settings";
    options.folderName = JUCE_APPLICATION_NAME_STRING;
    options.osxLibrarySubFolder = "Application Support";
    properties.setStorageParameters (options);

    formatManager.registerBasicFormats();
    pluginHost = std::make_unique<PluginHost> (getSettings());
    bridge = std::make_unique<EngineBridge> (engine, diskStreamer, formatManager, *pluginHost);
    bridge->onProblem = [this] (const juce::String& message) { reportProblem ("Problem", message); };
    bridge->setDiskCacheFrames (getSettings().getIntValue (diskCacheKey, 32768));
    bridge->setDelayCompensationEnabled (getSettings().getBoolValue ("delayCompensation", true));

    recentSessions.setMaxNumberOfItems (12);
    recentSessions.restoreFromString (getSettings().getValue (recentSessionsKey));

    // Audio device: restore the last setup, otherwise the system default.
    const auto savedDevice = getSettings().getXmlValue (deviceStateKey);
    const auto error = deviceManager.initialise (8, 2, savedDevice.get(), true);
    if (error.isNotEmpty())
    {
        DBG ("Audio device: " << error); // braces: DBG is empty in Release builds
    }
    deviceManager.addAudioCallback (&engine);
    deviceManager.addChangeListener (this);

    if (getSettings().getBoolValue (remoteEnabledKey, true))
    {
        if (const auto result = startRemoteControl(); result.failed())
        {
            DBG ("Remote control: " << result.getErrorMessage());
        }
    }
}

AppController::~AppController()
{
    shuttingDown.store (true);
    *lifetime = false;

    remoteServer.stop();
    jobs.removeAllJobs (true, 10000);

    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (&engine);
    deviceManager.closeAudioDevice();

    bridge->attachSession (nullptr);
    if (session != nullptr)
        session->getState().removeListener (this);
    session.reset();
    properties.closeFiles();
}

juce::PropertiesFile& AppController::getSettings()
{
    return *properties.getUserSettings();
}

void AppController::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Device setup changed (Playback Engine dialog or system): remember it.
    if (auto xml = deviceManager.createStateXml())
        getSettings().setValue (deviceStateKey, xml.get());
}

void AppController::reportProblem (const juce::String& title, const juce::String& message)
{
    if (showProblem)
        showProblem (title, message);
    else
        DBG (title << ": " << message);
}

//==============================================================================
// Session lifecycle

juce::File AppController::getDefaultSessionsFolder() const
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
        .getChildFile (juce::String (JUCE_APPLICATION_NAME_STRING) + " Sessions");
}

juce::Result AppController::createSession (const juce::File& parentFolder, const Session::Settings& settings,
                                           const SessionTemplate* sessionTemplate)
{
    if (! parentFolder.isDirectory() && ! parentFolder.createDirectory())
        return juce::Result::fail ("Could not create " + parentFolder.getFullPathName() + ".");

    juce::String error;
    auto newSession = Session::createNew (parentFolder, settings, error);
    if (newSession == nullptr)
        return juce::Result::fail (error);

    if (sessionTemplate != nullptr)
    {
        applyTemplate (*newSession, *sessionTemplate);
        newSession->save (error);
    }

    installSession (std::move (newSession));
    return juce::Result::ok();
}

juce::Result AppController::openSession (const juce::File& file)
{
    juce::String error;
    auto loaded = Session::load (file, error);
    if (loaded == nullptr)
        return juce::Result::fail (error);

    const auto repairs = loaded->getLoadRepairs();
    installSession (std::move (loaded));

    if (! repairs.isEmpty())
    {
        auto list = repairs;
        if (list.size() > 12)
        {
            list.removeRange (12, list.size());
            list.add ("... and " + juce::String (repairs.size() - 12) + " more");
        }
        reportProblem ("Session repaired",
                       file.getFileName() + " contained " + juce::String (repairs.size())
                           + " invalid value(s), which were fixed. Save the session to keep the repaired version.\n\n"
                           + list.joinIntoString ("\n"));
    }
    return juce::Result::ok();
}

void AppController::installSession (std::unique_ptr<Session> newSession)
{
    transportStop();

    // Background jobs for the old session keep running; their results are
    // discarded on arrival because the generation no longer matches.
    bridge->attachSession (nullptr);
    if (session != nullptr)
        session->getState().removeListener (this);

    session = std::move (newSession);
    ++sessionGeneration;

    if (session != nullptr)
        session->getState().addListener (this);

    engine.getTransport().locate (0);
    syncDeviceToSession();
    bridge->attachSession (session.get());
    applyMasterSettings();

    if (session != nullptr && rememberRecentSessions)
    {
        recentSessions.addFile (session->getSessionFile());
        getSettings().setValue (recentSessionsKey, recentSessions.toString());
    }

    sendChangeMessage();
}

void AppController::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    // Master settings live on the SESSION node; this also covers undo/redo.
    if (tree.hasType (ids::SESSION)
        && (property == ids::masterVolumeDb || property == ids::consoleEnabled || property == ids::consoleDrive
            || property == ids::consoleCharacter || property == ids::consoleCurve))
        applyMasterSettings();
}

void AppController::applyMasterSettings()
{
    if (session == nullptr)
        return;

    const auto& s = session->getState();
    auto& console = engine.getMasterBus();
    engine.masterVolumeDb.store (s.getProperty (ids::masterVolumeDb, 0.0));
    console.consoleEnabled.store (s.getProperty (ids::consoleEnabled, true));
    console.driveDb.store (s.getProperty (ids::consoleDrive, 6.0));
    console.character.store (s.getProperty (ids::consoleCharacter, 0.35));
    console.curveType.store (s.getProperty (ids::consoleCurve, 0));
}

void AppController::syncDeviceToSession()
{
    // With no audio device open there is no rate to switch and nothing to warn
    // about; the Playback Engine dialog, where a device gets chosen, flags any
    // rate mismatch itself.
    if (session == nullptr || deviceManager.getCurrentAudioDevice() == nullptr)
        return;

    auto setup = deviceManager.getAudioDeviceSetup();
    if (std::abs (setup.sampleRate - session->getSampleRate()) < 0.5)
        return;

    setup.sampleRate = session->getSampleRate();
    const auto error = deviceManager.setAudioDeviceSetup (setup, true);
    if (error.isNotEmpty() || std::abs (deviceManager.getAudioDeviceSetup().sampleRate - session->getSampleRate()) > 0.5)
        reportProblem ("Sample Rate",
                       "This session is " + juce::String (session->getSampleRate()) + " Hz, but the audio device could not be "
                       "switched to that rate. Playback will be at the wrong speed until you choose a device that supports it "
                       "in Setup > Playback Engine.");
}

juce::Result AppController::saveSession()
{
    if (session == nullptr)
        return juce::Result::fail ("No session is open.");

    bridge->storePluginStates();
    juce::String error;
    return session->save (error) ? juce::Result::ok() : juce::Result::fail (error);
}

//==============================================================================
// Transport

bool AppController::isRolling() const noexcept   { return engine.getTransport().isRolling(); }
bool AppController::isRecording() const noexcept { return bridge->isRecording(); }

juce::Result AppController::transportPlay()
{
    if (session == nullptr)
        return juce::Result::fail ("No session is open.");
    if (isBouncing())
        return juce::Result::fail ("A bounce is in progress; playback resumes when it finishes.");
    if (! engine.isDeviceRunning())
        return juce::Result::fail ("No audio device is running. Choose one in Setup > Playback Engine.");
    if (! isRolling())
        engine.getTransport().play();
    return juce::Result::ok();
}

void AppController::transportStop()
{
    if (bridge->isRecording())
        bridge->stopRecording();
    else
        engine.getTransport().stop();
}

void AppController::togglePlay()
{
    if (isRolling())
    {
        transportStop();
    }
    else if (const auto result = transportPlay(); result.failed())
    {
        reportProblem ("Playback", result.getErrorMessage());
    }
}

juce::Result AppController::transportRecord()
{
    int hardwareLatency = 0;
    if (auto* device = deviceManager.getCurrentAudioDevice())
        hardwareLatency = device->getInputLatencyInSamples() + device->getOutputLatencyInSamples();
    return bridge->startRecording (hardwareLatency);
}

void AppController::returnToZero()
{
    locate (0);
}

void AppController::locate (juce::int64 samplePosition)
{
    engine.getTransport().locate (samplePosition);
}

//==============================================================================
// Editing

juce::String AppController::nextTrackName (const juce::String& baseName) const
{
    for (int n = 1;; ++n)
    {
        const auto candidate = baseName + " " + juce::String (n);
        if (! session->getTracksNode().getChildWithProperty (ids::name, candidate).isValid())
            return candidate;
    }
}

void AppController::addTracks (int count, int numChannels, const juce::String& baseName, Session::TrackKind kind)
{
    if (session == nullptr)
        return;

    const auto fallback = kind == Session::TrackKind::aux ? "Aux" : "Audio";
    auto& um = session->getUndoManager();
    um.beginNewTransaction (count == 1 ? "New Track" : "New Tracks");
    for (int i = 0; i < count; ++i)
        session->addTrack (nextTrackName (baseName.isNotEmpty() ? baseName : juce::String (fallback)), numChannels, &um, kind);
}

void AppController::deleteTrack (juce::ValueTree t)
{
    if (session == nullptr || ! t.isValid())
        return;

    bridge->storePluginStates(); // so undo brings the plug-ins back with their settings
    auto& um = session->getUndoManager();
    um.beginNewTransaction ("Delete Track");

    // Routes into a deleted bus go with it, in the same undo step: sends to
    // it are removed and outputs to it fall back to the master.
    if (track::isAux (t))
    {
        const int busId = track::id (t);
        for (auto other : session->getTracksNode())
        {
            if (track::outputBus (other) == busId)
                other.setProperty (ids::outputBus, 0, &um);
            auto sends = track::sends (other);
            for (int i = sends.getNumChildren(); --i >= 0;)
                if (send::target (sends.getChild (i)) == busId)
                    sends.removeChild (i, &um);
        }
    }

    session->removeTrack (t, &um);
}

void AppController::importAudioFiles (const juce::Array<juce::File>& files, juce::int64 position, juce::ValueTree targetTrack)
{
    if (session == nullptr)
        return;

    const double sampleRate = session->getSampleRate();
    const int bitDepth = session->getBitDepth();
    const bool aiff = session->getFileFormat() == "aiff";
    const int generation = sessionGeneration;
    const bool useTarget = targetTrack.isValid() && files.size() == 1 && ! track::isAux (targetTrack); // auxes hold no clips

    for (const auto& source : files)
    {
        const auto destination = session->createUniqueAudioFile (source.getFileNameWithoutExtension());
        destination.create(); // reserve the name so parallel imports cannot collide
        ++runningJobs;

        jobs.addJob ([this, source, destination, sampleRate, bitDepth, aiff, generation, useTarget, targetTrack,
                      position, alive = std::weak_ptr<bool> (lifetime)]
        {
            auto result = importAudioFile (source, destination, sampleRate, bitDepth, aiff, formatManager,
                                           [this] (double) { return ! shuttingDown.load(); });

            juce::MessageManager::callAsync ([this, alive, result, source, destination, generation, useTarget, targetTrack, position]
            {
                if (alive.expired())
                    return;
                --runningJobs;

                if (! result.ok)
                {
                    destination.deleteFile();
                    if (! shuttingDown.load())
                        reportProblem ("Import Audio", result.error);
                    return;
                }
                if (session == nullptr || generation != sessionGeneration)
                    return;

                auto& um = session->getUndoManager();
                um.beginNewTransaction ("Import Audio");

                auto t = useTarget && targetTrack.getParent().isValid()
                             ? targetTrack
                             : session->addTrack (source.getFileNameWithoutExtension(), result.numChannels, &um);
                session->addClip (t, destination, position, 0, result.length, result.length,
                                  source.getFileNameWithoutExtension(), &um);
            });
        });
    }
}

void AppController::renderElasticAudio (juce::ValueTree c, double timeRatio, double pitchSemitones, bool preserveFormants)
{
    if (session == nullptr || ! c.isValid())
        return;

    if (! isTimeStretchAvailable())
    {
        reportProblem ("Elastic Audio", "This build has no time-stretching engine.");
        return;
    }

    TimeStretchRequest request;
    request.source = session->resolveStoredPath (c[ids::file].toString());
    request.sourceStart = clip::offset (c);
    request.sourceLength = clip::length (c);
    request.destination = session->createUniqueAudioFile (clip::name (c) + (std::abs (pitchSemitones) > 0.001 ? "_PS" : "_TCE"));
    request.destination.create();
    request.timeRatio = timeRatio;
    request.pitchSemitones = pitchSemitones;
    request.preserveFormants = preserveFormants;
    request.sampleRate = session->getSampleRate();
    request.bitDepth = session->getBitDepth();
    request.aiff = session->getFileFormat() == "aiff";

    const int generation = sessionGeneration;
    ++runningJobs;

    jobs.addJob ([this, request, c, timeRatio, generation, alive = std::weak_ptr<bool> (lifetime)]
    {
        auto result = renderTimeStretch (request, formatManager, [this] (double) { return ! shuttingDown.load(); });

        juce::MessageManager::callAsync ([this, alive, result, request, c, timeRatio, generation]
        {
            if (alive.expired())
                return;
            --runningJobs;

            if (! result.ok)
            {
                request.destination.deleteFile();
                if (! shuttingDown.load())
                    reportProblem ("Elastic Audio", result.error);
                return;
            }
            if (session == nullptr || generation != sessionGeneration || ! c.getParent().isValid())
                return; // session replaced or clip deleted meanwhile

            auto clipNode = c;
            auto& um = session->getUndoManager();
            um.beginNewTransaction ("Elastic Audio");
            clipNode.setProperty (ids::file, session->makeStoredPath (request.destination), &um);
            clipNode.setProperty (ids::offset, 0, &um);
            clipNode.setProperty (ids::length, result.length, &um);
            clipNode.setProperty (ids::sourceLength, result.length, &um);
            clipNode.setProperty (ids::fadeIn, (juce::int64) std::llround ((double) clip::fadeIn (clipNode) * timeRatio), &um);
            clipNode.setProperty (ids::fadeOut, (juce::int64) std::llround ((double) clip::fadeOut (clipNode) * timeRatio), &um);
        });
    });
}

//==============================================================================
// Bounce

juce::int64 AppController::getSessionEnd() const
{
    juce::int64 end = 0;
    if (session != nullptr)
        for (const auto& t : session->getTracksNode())
            for (const auto& c : track::clips (t))
                end = juce::jmax (end, clip::end (c));
    return end;
}

BounceRequest AppController::makeSessionBounceRequest() const
{
    BounceRequest request;
    if (session == nullptr)
        return request;

    request.start = 0;
    request.end = getSessionEnd() + (juce::int64) session->getSampleRate(); // one-second tail for decays
    request.sampleRate = session->getSampleRate();
    request.bitDepth = session->getBitDepth();
    request.aiff = session->getFileFormat() == "aiff";

    const auto folder = session->getFolder().getChildFile ("Bounced Files");
    folder.createDirectory();
    request.destination = folder.getNonexistentChildFile (juce::File::createLegalFileName (session->getName()),
                                                          request.aiff ? ".aif" : ".wav", false);
    return request;
}

std::unique_ptr<AudioEngine> AppController::createOfflineEngine (juce::String& error)
{
    if (session == nullptr)
    {
        error = "No session is open.";
        return nullptr;
    }
    if (offlineEngine != nullptr)
    {
        error = "A bounce is already running.";
        return nullptr;
    }

    transportStop();
    bridge->suspendLive(); // the offline render needs exclusive use of the plug-ins

    auto offline = std::make_unique<AudioEngine>();
    offline->setOfflineMode (true);
    offline->masterVolumeDb.store (engine.masterVolumeDb.load());
    auto& from = engine.getMasterBus();
    auto& to = offline->getMasterBus();
    to.consoleEnabled.store (from.consoleEnabled.load());
    to.driveDb.store (from.driveDb.load());
    to.character.store (from.character.load());
    to.curveType.store (from.curveType.load());

    // Plug-ins were prepared for the device's block size, so render in blocks no larger.
    const int blockSize = engine.getMaxBlockSize() > 0 ? engine.getMaxBlockSize() : 512;
    const double sampleRate = session->getSampleRate();
    offline->prepare (sampleRate, blockSize);
    offline->publishGraph (bridge->buildOfflineGraph (sampleRate, blockSize));
    return offline;
}

BounceResult AppController::bounceNow (const BounceRequest& request)
{
    juce::String error;
    auto offline = createOfflineEngine (error);
    if (offline == nullptr)
        return { false, error };

    auto result = renderBounce (*offline, request, {});
    offline.reset();
    bridge->resumeLive();
    lastBounce = result;
    return result;
}

juce::Result AppController::startBounce (const BounceRequest& request, std::function<void (const BounceResult&)> onDone)
{
    juce::String error;
    offlineEngine = createOfflineEngine (error);
    if (offlineEngine == nullptr)
        return juce::Result::fail (error);

    bounceProgress.store (0.0);
    ++runningJobs;

    jobs.addJob ([this, request, onDone, engineForJob = offlineEngine.get(), alive = std::weak_ptr<bool> (lifetime)]
    {
        auto result = renderBounce (*engineForJob, request, [this] (double p)
        {
            bounceProgress.store (p);
            return ! shuttingDown.load();
        });

        juce::MessageManager::callAsync ([this, alive, result, onDone]
        {
            if (alive.expired())
                return;
            --runningJobs;
            offlineEngine.reset(); // graphs and plug-in references released on the message thread
            bridge->resumeLive();
            lastBounce = result;
            if (onDone)
                onDone (result);
        });
    });

    return juce::Result::ok();
}

//==============================================================================
// Remote control

juce::Result AppController::startRemoteControl()
{
    int port = getSettings().getIntValue (remotePortKey, RemoteControlServer::defaultPort);
    if (const auto env = juce::SystemStats::getEnvironmentVariable ("MERIDIAN_REMOTE_PORT", {}); env.isNotEmpty())
        port = env.getIntValue();

    remote::ws::HandshakePolicy policy;
    policy.requiredToken = juce::SystemStats::getEnvironmentVariable ("MERIDIAN_REMOTE_TOKEN",
                                                                      getSettings().getValue (remoteTokenKey)).toStdString();
    for (const auto& origin : juce::StringArray::fromTokens (getSettings().getValue (remoteOriginsKey), ";", {}))
        if (origin.trim().isNotEmpty())
            policy.allowedOrigins.push_back (origin.trim().toStdString());

    juce::String error;
    if (! remoteServer.start (port, std::move (policy), error))
        return juce::Result::fail (error);

    getSettings().setValue (remoteEnabledKey, true);
    return juce::Result::ok();
}

void AppController::stopRemoteControl()
{
    remoteServer.stop();
    getSettings().setValue (remoteEnabledKey, false);
}

juce::String AppController::describePosition() const
{
    return timeformat::minSecs (engine.getTransport().getPosition(), engine.getSampleRate());
}

remote::ActionResult AppController::play()
{
    if (isRecording())
        return remote::ActionResult::failure ("Recording is in progress; send STOP first.");
    if (isRolling())
        return remote::ActionResult::success ("Already playing at " + describePosition().toStdString());

    const auto result = transportPlay();
    if (result.failed())
        return remote::ActionResult::failure (result.getErrorMessage().toStdString());
    return remote::ActionResult::success ("Playing from " + describePosition().toStdString());
}

remote::ActionResult AppController::stop()
{
    const bool wasRecording = isRecording();
    const bool wasRolling = isRolling();
    transportStop();
    if (! wasRolling)
        return remote::ActionResult::success ("Already stopped");
    return remote::ActionResult::success (wasRecording ? "Recording stopped; takes are being added to their tracks"
                                                       : "Stopped");
}

remote::ActionResult AppController::record()
{
    if (isRecording())
        return remote::ActionResult::success ("Already recording");

    const auto result = transportRecord();
    if (result.failed())
        return remote::ActionResult::failure (result.getErrorMessage().toStdString());

    int armed = 0;
    for (const auto& t : session->getTracksNode())
        armed += track::recordArm (t) ? 1 : 0;
    return remote::ActionResult::success ("Recording on " + std::to_string (armed) + " track" + (armed == 1 ? "" : "s"));
}

remote::ActionResult AppController::toggleWindow()
{
    if (! toggleWindowHandler)
        return remote::ActionResult::failure ("No window is open.");
    return remote::ActionResult::success (toggleWindowHandler() == WindowKind::mix ? "Mix window" : "Edit window");
}

remote::ActionResult AppController::toggleTrackMute (int trackNumber)
{
    if (session == nullptr)
        return remote::ActionResult::failure ("No session is open.");

    auto t = session->getTrack (trackNumber - 1);
    if (! t.isValid())
        return remote::ActionResult::failure ("There is no track " + std::to_string (trackNumber) + "; the session has "
                                              + std::to_string (session->getNumTracks()) + ".");

    const bool nowMuted = ! track::mute (t);
    auto& um = session->getUndoManager();
    um.beginNewTransaction ("Remote: Mute");
    t.setProperty (ids::mute, nowMuted, &um); // -> TrackRealtimeState::muted, ramped by the engine

    return remote::ActionResult::success ((trackLabel (trackNumber, t) + (nowMuted ? " muted" : " unmuted")).toStdString());
}

remote::ActionResult AppController::setTrackVolume (int trackNumber, double decibels)
{
    if (session == nullptr)
        return remote::ActionResult::failure ("No session is open.");

    auto t = session->getTrack (trackNumber - 1);
    if (! t.isValid())
        return remote::ActionResult::failure ("There is no track " + std::to_string (trackNumber) + "; the session has "
                                              + std::to_string (session->getNumTracks()) + ".");

    const double stored = std::isfinite (decibels) ? juce::jmax (faderFloorDb, decibels) : faderFloorDb;
    auto& um = session->getUndoManager();
    um.beginNewTransaction ("Remote: Volume");
    t.setProperty (ids::volumeDb, stored, &um); // -> engine ramps to it over 30 ms

    const auto level = stored <= faderFloorDb ? juce::String ("-inf") : juce::String (stored, 1);
    return remote::ActionResult::success ((trackLabel (trackNumber, t) + " ramping to " + level + " dB").toStdString());
}

remote::ActionResult AppController::bounce()
{
    if (session == nullptr)
        return remote::ActionResult::failure ("No session is open.");

    const auto request = makeSessionBounceRequest();
    if (const auto started = startBounce (request); started.failed())
        return remote::ActionResult::failure (started.getErrorMessage().toStdString());

    return remote::ActionResult::success ("Bouncing " + timeformat::minSecs (request.end - request.start, request.sampleRate).toStdString()
                                          + " to " + request.destination.getFullPathName().toStdString()
                                          + ". Send STATUS to follow progress.");
}

std::string AppController::statusJson()
{
    const auto& transport = engine.getTransport();
    const char* state = transport.getState() == Transport::State::recording ? "recording"
                        : transport.getState() == Transport::State::playing  ? "playing"
                                                                             : "stopped";

    remote::JsonObject status;
    status.addString ("transport", state)
        .addNumber ("positionSeconds", (double) transport.getPosition() / juce::jmax (1.0, engine.getSampleRate()), 3)
        .addNumber ("sampleRate", engine.getSampleRate(), 0)
        .addString ("window", activeWindowQuery && activeWindowQuery() == WindowKind::mix ? "mix" : "edit")
        .addBool ("audioDeviceRunning", engine.isDeviceRunning())
        .addInt ("pluginLatencySamples", engine.getPluginLatencySamples())
        .addBool ("delayCompensation", bridge->isDelayCompensationEnabled());

    remote::JsonObject bounceInfo;
    bounceInfo.addBool ("running", isBouncing());
    if (isBouncing())
        bounceInfo.addNumber ("progress", getBounceProgress(), 3);
    if (lastBounce.ok)
        bounceInfo.addString ("lastFile", lastBounce.file.getFullPathName().toStdString())
            .addNumber ("lastPeakDbLeft", dsp::gainToDb (lastBounce.left.peak), 2)
            .addNumber ("lastPeakDbRight", dsp::gainToDb (lastBounce.right.peak), 2)
            .addNumber ("lastSeconds", (double) lastBounce.length / juce::jmax (1.0, engine.getSampleRate()), 3);
    status.addRaw ("bounce", bounceInfo.toString());

    if (session == nullptr)
        return status.addRaw ("session", "null").toString();

    status.addString ("session", session->getName().toStdString());

    std::string tracks = "[";
    for (int i = 0; i < session->getNumTracks(); ++i)
    {
        const auto t = session->getTrack (i);
        const double db = track::volumeDb (t);
        remote::JsonObject entry;
        const int output = track::outputBus (t);
        const auto outputTrack = output != 0 ? session->getTrackById (output) : juce::ValueTree();
        entry.addInt ("number", i + 1)
            .addString ("name", track::name (t).toStdString())
            .addString ("kind", track::isAux (t) ? "aux" : "audio")
            .addString ("output", outputTrack.isValid() ? track::name (outputTrack).toStdString() : std::string ("master"))
            .addInt ("sends", track::sends (t).getNumChildren())
            .addInt ("channels", track::channels (t))
            .addNumber ("pan", track::pan (t), 2)
            .addBool ("muted", track::mute (t))
            .addBool ("soloed", track::solo (t))
            .addBool ("recordArmed", track::recordArm (t))
            .addInt ("clips", track::clips (t).getNumChildren());
        if (db <= faderFloorDb)
            entry.addRaw ("volumeDb", "null");
        else
            entry.addNumber ("volumeDb", db, 1);
        tracks += (i > 0 ? "," : "") + entry.toString();
    }
    tracks += "]";

    return status.addRaw ("tracks", tracks).toString();
}

} // namespace meridian
