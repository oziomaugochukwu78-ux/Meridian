#include "AppController.h"
#include "model/Groups.h"

#include "media/AudioImporter.h"
#include "media/TimeStretchEngine.h"
#include "model/MidiModel.h"
#include "model/SessionBackups.h"
#include "plugins/BuiltinEffects.h"
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
constexpr const char* backupMinutesKey = "autoBackupMinutes"; // 0 turns automatic backups off
constexpr const char* backupKeepKey = "autoBackupKeep";
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

    backupTimer.startTimer (30 * 1000); // checks twice a minute; writes only on the set interval
    automationRecorder = std::make_unique<AutomationRecorder> (*this);
}

AppController::~AppController()
{
    shuttingDown.store (true);
    *lifetime = false;
    automationRecorder.reset();
    backupTimer.stopTimer();
    if (session != nullptr)
        backups::markClosed (session->getSessionFile()); // a clean exit: nothing to recover next time

    remoteServer.stop();
    jobs.removeAllJobs (true, 10000);
    if (midiInputsEnabled)
        deviceManager.removeMidiInputDeviceCallback ({}, this);

    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (&engine);
    deviceManager.closeAudioDevice();

    bridge->attachSession (nullptr);
    groupLinker.attach (nullptr);
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
    {
        showProblem (title, message);
    }
    else
    {
        DBG (title << ": " << message); // braces: DBG is empty in Release builds
    }
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
        applyTemplate (*newSession, *sessionTemplate);

    installSession (std::move (newSession));

    // The template's effects need the engine, so they go in once the session is installed.
    if (sessionTemplate != nullptr)
    {
        const auto descriptions = BuiltinEffectFormat::getDescriptions();
        for (const auto& spec : sessionTemplate->tracks)
        {
            auto t = session->getTracksNode().getChildWithProperty (ids::name, spec.name);
            for (int slot = 0; slot < spec.inserts.size() && slot < Session::maxInserts; ++slot)
                for (const auto& d : descriptions)
                {
                    if (d.name != spec.inserts[slot])
                        continue;
                    juce::String insertError;
                    if (! bridge->insertPlugin (t, slot, d, insertError) || ! track::isAux (t))
                        continue;
                    // On a bus the effect is fully wet: the dry signal already reaches the mix.
                    if (const auto insert = bridge->getInsert (track::id (t), slot); insert != nullptr && insert->plugin != nullptr)
                        for (auto* p : insert->plugin->getParameters())
                            if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (p); hosted != nullptr && hosted->getParameterID() == "mix")
                                p->setValueNotifyingHost (1.0f);
                }
        }
        return saveSession();
    }
    return juce::Result::ok();
}

juce::Result AppController::openSession (const juce::File& file)
{
    juce::String error;
    auto loaded = Session::load (file, error);
    if (loaded == nullptr)
        return juce::Result::fail (error);

    // Before installSession marks the session open again: was it left open?
    // (Re-opening the session this process has open is not a crash: the marker is ours.)
    const bool openHere = session != nullptr && session->getSessionFile() == file;
    const auto recovery = openHere ? juce::File() : backups::findRecovery (file);

    const auto repairs = loaded->getLoadRepairs();
    installSession (std::move (loaded));

    recoveryBackup = recovery;
    if (recovery.existsAsFile() && onRecoveryAvailable)
        onRecoveryAvailable (recovery);

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

juce::ValueTree AppController::groupTracks (const juce::String& name, const juce::Array<int>& trackIds, bool mix, bool edit)
{
    if (session == nullptr)
        return {};
    auto& um = session->getUndoManager();
    um.beginNewTransaction ("Group Tracks");
    return groups::create (*session, name, trackIds, mix, edit, &um);
}

void AppController::installSession (std::unique_ptr<Session> newSession)
{
    transportStop();

    // Background jobs for the old session keep running; their results are
    // discarded on arrival because the generation no longer matches.
    bridge->attachSession (nullptr);
    groupLinker.attach (nullptr);
    if (session != nullptr)
    {
        session->getState().removeListener (this);
        backups::markClosed (session->getSessionFile());
    }

    session = std::move (newSession);
    ++sessionGeneration;
    recoveryBackup = juce::File();
    lastBackupTime = juce::Time::getCurrentTime();
    backupProblemReported = false;

    if (session != nullptr)
    {
        session->getState().addListener (this);
        groupLinker.attach (session.get());
        backups::markOpen (session->getSessionFile());
        // A freshly loaded session has nothing to back up; a recovered one does.
        changeCountAtLastBackup = session->hasUnsavedChanges() ? ~juce::uint64 (0) : session->getChangeCount();
    }

    engine.getTransport().locate (0);
    syncDeviceToSession();
    bridge->attachSession (session.get());
    applyMasterSettings();
    applyTransportSettings();

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

    if (tree.hasType (ids::SESSION)
        && (property == ids::loopEnabled || property == ids::loopStart || property == ids::loopEnd || property == ids::tempo
            || property == ids::punchEnabled || property == ids::punchIn || property == ids::punchOut
            || property == ids::clickEnabled || property == ids::clickLevelDb))
        applyTransportSettings();
}

void AppController::applyTransportSettings()
{
    if (session == nullptr)
    {
        engine.getTransport().setLoop (false, 0, 0);
        engine.clickEnabled.store (false);
        return;
    }
    const auto& s = session->getState();
    engine.getTransport().setLoop (s.getProperty (ids::loopEnabled, false), (juce::int64) s.getProperty (ids::loopStart, 0),
                                   (juce::int64) s.getProperty (ids::loopEnd, 0));
    engine.getTransport().setPunch (s.getProperty (ids::punchEnabled, false), (juce::int64) s.getProperty (ids::punchIn, 0),
                                    (juce::int64) s.getProperty (ids::punchOut, 0));
    engine.clickEnabled.store (s.getProperty (ids::clickEnabled, false));
    engine.clickLevelDb.store (s.getProperty (ids::clickLevelDb, -6.0));
    // The steady grid used for a count-in: the tempo and meter at the playhead.
    // (While rolling, the click follows the full tempo and meter maps.)
    const auto [bpm, beats] = countInGrid();
    engine.clickBpm.store (bpm);
    engine.clickBeatsPerBar.store (beats);
}

std::pair<double, int> AppController::countInGrid() const
{
    if (session == nullptr)
        return { 120.0, 4 };
    const auto tempo = midimodel::tempoMap (*session);
    const auto meter = midimodel::meterMap (*session);
    const double rate = engine.getSampleRate() > 0.0 ? engine.getSampleRate() : session->getSampleRate();
    const auto tick = tempo.samplesToTicks (engine.getTransport().getPosition(), rate);
    const auto& m = meter.meterAt (meter.positionOf (tick).bar);
    // Beats of the meter (eighths in 6/8): the quarter-note tempo scaled to them.
    return { juce::jlimit (20.0, 400.0, tempo.bpmAt (tick) * m.denominator / 4.0), m.numerator };
}

void AppController::setLoop (bool enabled, juce::int64 start, juce::int64 end)
{
    if (session == nullptr)
        return;
    auto s = session->getState();
    start = std::max<juce::int64> (0, start);
    s.setProperty (ids::loopStart, start, nullptr);
    s.setProperty (ids::loopEnd, std::max (start, end), nullptr);
    s.setProperty (ids::loopEnabled, enabled && end > start, nullptr);
}

void AppController::setPunch (bool enabled, juce::int64 in, juce::int64 out)
{
    if (session == nullptr)
        return;
    auto s = session->getState();
    in = std::max<juce::int64> (0, in);
    s.setProperty (ids::punchIn, in, nullptr);
    s.setProperty (ids::punchOut, std::max (in, out), nullptr);
    s.setProperty (ids::punchEnabled, enabled && out > in, nullptr);
}

void AppController::setClick (bool enabled, double levelDb)
{
    if (session == nullptr)
        return;
    auto s = session->getState();
    s.setProperty (ids::clickEnabled, enabled, nullptr);
    s.setProperty (ids::clickLevelDb, std::isfinite (levelDb) ? juce::jlimit (-60.0, 0.0, levelDb) : -6.0, nullptr);
}

void AppController::setCountInBars (int bars)
{
    if (session != nullptr)
        session->getState().setProperty (ids::countInBars, juce::jlimit (0, 8, bars), nullptr);
}

juce::int64 AppController::getCountInSamples() const
{
    if (session == nullptr)
        return 0;
    const int bars = juce::jlimit (0, 8, (int) session->getState().getProperty (ids::countInBars, 0));
    // The same grid as the click (dsp::Metronome::beatStart), so the count-in ends on a beat.
    const double rate = engine.getSampleRate() > 0.0 ? engine.getSampleRate() : session->getSampleRate();
    const auto [bpm, beats] = countInGrid();
    return (juce::int64) std::llround ((double) (bars * beats) * 60.0 * rate / bpm);
}

juce::ValueTree AppController::addMarker (const juce::String& name, juce::int64 position)
{
    if (session == nullptr)
        return {};
    auto& um = session->getUndoManager();
    um.beginNewTransaction ("Add Marker");
    return session->addMarker (name, position, &um);
}

bool AppController::locateToMarker (int number)
{
    if (session == nullptr)
        return false;
    const auto m = session->getMarkerByNumber (number);
    if (! m.isValid())
        return false;
    locate ((juce::int64) m[ids::start]);
    return true;
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
    if (! session->save (error))
        return juce::Result::fail (error);
    backups::markOpen (session->getSessionFile()); // the file is current as of now: older backups are not offered
    return juce::Result::ok();
}

juce::Result AppController::saveSessionAs (const juce::File& newFile)
{
    if (session == nullptr)
        return juce::Result::fail ("No session is open.");

    const auto oldFile = session->getSessionFile();
    bridge->storePluginStates();
    juce::String error;
    if (! session->saveAs (newFile, error))
        return juce::Result::fail (error);

    backups::markClosed (oldFile);
    backups::markOpen (session->getSessionFile());
    if (rememberRecentSessions)
    {
        recentSessions.addFile (session->getSessionFile());
        getSettings().setValue (recentSessionsKey, recentSessions.toString());
    }
    sendChangeMessage(); // the title and anything showing the file name
    return juce::Result::ok();
}

juce::Result AppController::recoverFromBackup (const juce::File& backup)
{
    if (session == nullptr)
        return juce::Result::fail ("No session is open.");

    juce::String error;
    auto recovered = Session::loadBackup (backup, session->getSessionFile(), error);
    if (recovered == nullptr)
        return juce::Result::fail (error);
    installSession (std::move (recovered));
    return juce::Result::ok();
}

bool AppController::backupIfChanged()
{
    if (session == nullptr)
        return false;

    bridge->storePluginStates(); // so plug-in settings changed since are captured too
    if (session->getChangeCount() == changeCountAtLastBackup)
        return false;

    const int keep = juce::jlimit (1, 100, getSettings().getIntValue (backupKeepKey, backups::defaultKeep));
    if (const auto result = backups::write (*session, keep); result.failed())
    {
        if (! backupProblemReported) // once per session, not every interval
            reportProblem ("Backup", "Could not write a session backup: " + result.getErrorMessage());
        backupProblemReported = true;
        return false;
    }
    changeCountAtLastBackup = session->getChangeCount();
    lastBackupTime = juce::Time::getCurrentTime();
    return true;
}

void AppController::autoBackup()
{
    const int minutes = getSettings().getIntValue (backupMinutesKey, 5);
    if (session == nullptr || minutes <= 0 || isBouncing())
        return;
    if (juce::Time::getCurrentTime() - lastBackupTime >= juce::RelativeTime::minutes (minutes))
        backupIfChanged();
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
    applyTransportSettings(); // the count-in grid at the playhead, where recording starts
    int hardwareLatency = 0, outputLatency = 0;
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        outputLatency = device->getOutputLatencyInSamples();
        hardwareLatency = device->getInputLatencyInSamples() + outputLatency;
    }
    return bridge->startRecording (hardwareLatency, getCountInSamples(), outputLatency);
}

//==============================================================================
// MIDI input

void AppController::enableMidiInputs()
{
    if (midiInputsEnabled)
        return;
    midiInputsEnabled = true;
    deviceManager.addMidiInputDeviceCallback ({}, this); // every enabled input
    openAllMidiInputs();
    // A keyboard plugged in later is opened too.
    midiDeviceListConnection = juce::MidiDeviceListConnection::make ([this] { openAllMidiInputs(); });
}

void AppController::openAllMidiInputs()
{
    for (const auto& device : juce::MidiInput::getAvailableDevices())
        if (! deviceManager.isMidiInputDeviceEnabled (device.identifier))
            deviceManager.setMidiInputDeviceEnabled (device.identifier, true);
}

juce::StringArray AppController::getOpenMidiInputs() const
{
    juce::StringArray names;
    for (const auto& device : juce::MidiInput::getAvailableDevices())
        if (deviceManager.isMidiInputDeviceEnabled (device.identifier))
            names.add (device.name);
    return names;
}

void AppController::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& message)
{
    // MIDI device thread. Notes, the sustain pedal (CC 64: 64 and above is down),
    // the pitch wheel and the mod wheel (CC 1); other controllers are not used yet.
    // JUCE stamps incoming messages in Time::getMillisecondCounter() seconds.
    if (message.isNoteOn())
        engine.pushLiveMidi (message.getNoteNumber(), message.getFloatVelocity(), true, message.getTimeStamp());
    else if (message.isNoteOff())
        engine.pushLiveMidi (message.getNoteNumber(), 0.0f, false, message.getTimeStamp());
    else if (message.isSustainPedalOn())
        engine.pushLiveSustain (true, message.getTimeStamp());
    else if (message.isSustainPedalOff())
        engine.pushLiveSustain (false, message.getTimeStamp());
    else if (message.isPitchWheel())
    {
        // 14-bit, 8192 = centre: both ends map to exactly -1 and +1.
        const int offset = message.getPitchWheelValue() - 8192;
        engine.pushLiveControl (midi::pitchBend, (float) offset / (offset >= 0 ? 8191.0f : 8192.0f), message.getTimeStamp());
    }
    else if (message.isControllerOfType (1))
    {
        engine.pushLiveControl (midi::modWheel, (float) message.getControllerValue() / 127.0f, message.getTimeStamp());
    }
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

juce::ValueTree AppController::duplicateTrack (juce::ValueTree t)
{
    if (session == nullptr || ! t.isValid())
        return {};
    bridge->storePluginStates(); // the copy gets the plug-in settings as they sound now
    auto& um = session->getUndoManager();
    um.beginNewTransaction ("Duplicate Track");
    return session->duplicateTrack (t, &um);
}

void AppController::moveTrack (juce::ValueTree t, int newIndex)
{
    if (session == nullptr || ! t.isValid())
        return;
    auto& um = session->getUndoManager();
    um.beginNewTransaction ("Move Track");
    session->moveTrack (t, newIndex, &um);
}

void AppController::importAudioFiles (const juce::Array<juce::File>& files, juce::int64 position, juce::ValueTree targetTrack)
{
    if (session == nullptr)
        return;

    const double sampleRate = session->getSampleRate();
    const int bitDepth = session->getBitDepth();
    const bool aiff = session->getFileFormat() == "aiff";
    const int generation = sessionGeneration;
    const bool useTarget = targetTrack.isValid() && files.size() == 1 && track::isAudio (targetTrack); // only audio tracks hold audio clips

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
    request.format = request.aiff ? "aiff" : "wav";
    request.dither = request.bitDepth == 16; // the audible case; 24-bit rounding error is far below hearing

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
    {
        BounceResult failed;
        failed.error = error;
        return failed;
    }

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
            .addString ("kind", track::isAux (t) ? "aux" : track::isInstrument (t) ? "instrument" : "audio")
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
