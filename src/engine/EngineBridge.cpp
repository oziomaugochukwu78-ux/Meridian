#include "EngineBridge.h"

#include <algorithm>
#include <cmath>

namespace meridian
{
namespace
{
using Interval = std::pair<juce::int64, juce::int64>;

/** Removes [a, b) from a set of disjoint intervals. */
void subtract (std::vector<Interval>& intervals, juce::int64 a, juce::int64 b)
{
    std::vector<Interval> out;
    out.reserve (intervals.size() + 1);
    for (const auto& [s, e] : intervals)
    {
        if (b <= s || a >= e)
        {
            out.emplace_back (s, e);
            continue;
        }
        if (a > s) out.emplace_back (s, a);
        if (b < e) out.emplace_back (b, e);
    }
    intervals.swap (out);
}

double decibelsToGain (double db)
{
    return db <= -144.0 ? 0.0 : std::pow (10.0, db / 20.0);
}

int trackIdOfInsert (const juce::ValueTree& insertNode)
{
    return track::id (insertNode.getParent().getParent());
}
} // namespace

EngineBridge::EngineBridge (AudioEngine& e, DiskStreamer& d, juce::AudioFormatManager& f, PluginHost& p)
    : engine (e), diskStreamer (d), formatManager (f), pluginHost (p)
{
    recordWriterThread.startThread (juce::Thread::Priority::high);
    engine.setListener (this);
}

EngineBridge::~EngineBridge()
{
    engine.setListener (nullptr);
    attachSession (nullptr);
    stopTimer();
    for (auto& take : takesToFinalise)
        take->writer.reset();
    recordWriterThread.stopThread (4000);
}

void EngineBridge::attachSession (Session* newSession)
{
    if (isRecording())
        stopRecording();

    if (session != nullptr)
        session->getState().removeListener (this);

    {
        const juce::ScopedLock sl (pluginLock);
        insertSlots.clear();
    }
    trackStates.clear();
    reportedMissingFiles.clear();

    session = newSession;

    if (session != nullptr)
    {
        session->getState().addListener (this);
        for (const auto& t : session->getTracksNode())
            addTrackResources (t);
    }

    rebuildNow();
}

//==============================================================================
std::shared_ptr<TrackRealtimeState> EngineBridge::getTrackState (int trackId) const
{
    const auto it = trackStates.find (trackId);
    return it != trackStates.end() ? it->second : nullptr;
}

std::shared_ptr<TrackRealtimeState> EngineBridge::ensureTrackState (const juce::ValueTree& t)
{
    auto& state = trackStates[track::id (t)];
    if (state == nullptr)
        state = std::make_shared<TrackRealtimeState> (track::id (t));
    syncTrackState (t);
    return state;
}

void EngineBridge::syncTrackState (const juce::ValueTree& t)
{
    const auto it = trackStates.find (track::id (t));
    if (it == trackStates.end())
        return;

    auto& s = *it->second;
    s.volumeDb.store (track::volumeDb (t));
    s.pan.store (track::pan (t));
    s.muted.store (track::mute (t));
    s.soloed.store (track::solo (t));
    s.recordArmed.store (track::recordArm (t));
    s.automationRead.store (track::automationRead (t));
}

void EngineBridge::addTrackResources (const juce::ValueTree& t)
{
    ensureTrackState (t);
    for (const auto& insert : track::inserts (t))
        instantiateInsert (insert);
}

void EngineBridge::removeTrackResources (int trackId)
{
    trackStates.erase (trackId);

    const juce::ScopedLock sl (pluginLock);
    for (auto it = insertSlots.begin(); it != insertSlots.end();)
        it = it->first.first == trackId ? insertSlots.erase (it) : std::next (it);
    // Instances still referenced by the live graph are destroyed when that
    // graph is retired, on the message thread.
}

//==============================================================================
// Inserts

std::shared_ptr<InsertSlot> EngineBridge::getInsert (int trackId, int slot) const
{
    const juce::ScopedLock sl (pluginLock);
    const auto it = insertSlots.find ({ trackId, slot });
    return it != insertSlots.end() ? it->second : nullptr;
}

juce::ValueTree EngineBridge::getInsertNode (const juce::ValueTree& t, int slot) const
{
    return track::inserts (t).getChildWithProperty (ids::slot, slot);
}

void EngineBridge::instantiateInsert (const juce::ValueTree& node)
{
    const int trackId = trackIdOfInsert (node);
    const int slotIndex = node[ids::slot];
    const auto t = node.getParent().getParent();

    auto slot = std::make_shared<InsertSlot>();
    slot->bypassed.store ((bool) node.getProperty (ids::bypass, false));

    const auto xml = juce::parseXML (node[ids::description].toString());
    if (xml == nullptr || ! slot->description.loadFromXml (*xml))
    {
        if (onProblem)
            onProblem ("A plug-in on track \"" + track::name (t) + "\" has an unreadable description and was skipped.");
        return;
    }

    const double sampleRate = engine.getSampleRate();
    const int blockSize = engine.getMaxBlockSize() > 0 ? engine.getMaxBlockSize() : 512;

    juce::String error;
    const juce::ScopedLock sl (pluginLock);

    std::unique_ptr<juce::AudioPluginInstance> instance;
    if (preparedInstance != nullptr && preparedKey == std::make_pair (trackId, slotIndex))
        instance = std::move (preparedInstance); // already created and validated by insertPlugin()
    else
        instance = pluginHost.createInstance (slot->description, track::channels (t), sampleRate, blockSize, error);
    preparedInstance.reset();

    if (instance == nullptr)
    {
        // Keep the empty slot so the UI can show the plug-in as missing.
        if (onProblem)
            onProblem ("Track \"" + track::name (t) + "\": " + error);
    }
    else
    {
        juce::MemoryBlock stateData;
        if (stateData.fromBase64Encoding (node[ids::state].toString()) && stateData.getSize() > 0)
            instance->setStateInformation (stateData.getData(), (int) stateData.getSize());
        slot->plugin = std::move (instance);
    }

    insertSlots[{ trackId, slotIndex }] = std::move (slot);
}

bool EngineBridge::insertPlugin (juce::ValueTree t, int slot, const juce::PluginDescription& description, juce::String& error)
{
    if (session == nullptr)
        return false;

    // Create the plug-in before touching the document, so a failure leaves no
    // trace. The listener that reacts to the new INSERT node adopts this instance.
    {
        const juce::ScopedLock sl (pluginLock);
        preparedInstance = pluginHost.createInstance (description, track::channels (t), engine.getSampleRate(),
                                                      juce::jmax (512, engine.getMaxBlockSize()), error);
        if (preparedInstance == nullptr)
            return false;
        preparedKey = { track::id (t), slot };
    }

    auto& um = session->getUndoManager();
    um.beginNewTransaction ("Insert " + description.name);

    if (auto existing = getInsertNode (t, slot); existing.isValid())
        track::inserts (t).removeChild (existing, &um);

    juce::ValueTree node (ids::INSERT);
    node.setProperty (ids::slot, slot, nullptr);
    node.setProperty (ids::description,
                      description.createXml()->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader()),
                      nullptr);
    node.setProperty (ids::bypass, false, nullptr);
    track::inserts (t).appendChild (node, &um); // the listener instantiates it
    return true;
}

void EngineBridge::removePlugin (juce::ValueTree t, int slot)
{
    if (session == nullptr)
        return;

    auto node = getInsertNode (t, slot);
    if (! node.isValid())
        return;

    // Snapshot the plug-in's settings into the node first, so undo restores them.
    if (auto s = getInsert (track::id (t), slot); s != nullptr && s->plugin != nullptr)
    {
        juce::MemoryBlock data;
        s->plugin->getStateInformation (data);
        node.setProperty (ids::state, data.toBase64Encoding(), nullptr);
    }

    auto& um = session->getUndoManager();
    um.beginNewTransaction ("Remove Plug-in");
    track::inserts (t).removeChild (node, &um);
}

void EngineBridge::setPluginBypassed (juce::ValueTree t, int slot, bool bypassed)
{
    if (session == nullptr)
        return;
    if (auto node = getInsertNode (t, slot); node.isValid())
    {
        session->getUndoManager().beginNewTransaction (bypassed ? "Bypass Plug-in" : "Enable Plug-in");
        node.setProperty (ids::bypass, bypassed, &session->getUndoManager());
    }
}

void EngineBridge::storePluginStates()
{
    if (session == nullptr)
        return;

    const juce::ScopedLock sl (pluginLock);
    for (const auto& [key, slot] : insertSlots)
    {
        if (slot->plugin == nullptr)
            continue;
        auto node = getInsertNode (session->getTrackById (key.first), key.second);
        if (! node.isValid())
            continue;

        juce::MemoryBlock data;
        slot->plugin->getStateInformation (data);
        node.setProperty (ids::state, data.toBase64Encoding(), nullptr);
    }
}

//==============================================================================
// Device

void EngineBridge::engineAboutToStart (double sampleRate, int maxBlockSize)
{
    {
        // Device thread, with no audio callbacks running: the one safe moment
        // to re-prepare plug-ins that the live graph is using.
        const juce::ScopedLock sl (pluginLock);
        for (auto& [key, slot] : insertSlots)
        {
            if (slot->plugin != nullptr)
            {
                slot->plugin->releaseResources();
                slot->plugin->prepareToPlay (sampleRate, maxBlockSize);
            }
        }
    }
    triggerAsyncUpdate(); // rebuild scratch buffers for the new block size
}

//==============================================================================
// Document changes

void EngineBridge::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree.hasType (ids::TRACK))
    {
        if (property == ids::channels)
        {
            // Plug-ins were configured for the old channel count: rebuild them.
            removeTrackResources (track::id (tree));
            addTrackResources (tree);
            triggerAsyncUpdate();
        }
        else if (property == ids::inputChannel)
        {
            triggerAsyncUpdate();
        }
        else
        {
            syncTrackState (tree); // mixer controls: no rebuild needed
        }
        return;
    }

    if (tree.hasType (ids::INSERT))
    {
        if (property == ids::bypass)
            if (auto slot = getInsert (trackIdOfInsert (tree), tree[ids::slot]))
                slot->bypassed.store ((bool) tree[ids::bypass]);
        return;
    }

    if (tree.hasType (ids::SESSION) || (tree.hasType (ids::CLIP) && property == ids::name))
        return;

    triggerAsyncUpdate(); // clips, automation
}

void EngineBridge::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child)
{
    if (child.hasType (ids::TRACK))
        addTrackResources (child);
    else if (child.hasType (ids::INSERT))
        instantiateInsert (child);

    triggerAsyncUpdate();
}

void EngineBridge::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (child.hasType (ids::TRACK))
    {
        removeTrackResources (track::id (child));
    }
    else if (child.hasType (ids::INSERT))
    {
        const juce::ScopedLock sl (pluginLock);
        insertSlots.erase ({ track::id (parent.getParent()), (int) child[ids::slot] });
    }

    triggerAsyncUpdate();
}

void EngineBridge::valueTreeChildOrderChanged (juce::ValueTree&, int, int)
{
    triggerAsyncUpdate();
}

void EngineBridge::handleAsyncUpdate()
{
    rebuildGraph();
}

void EngineBridge::rebuildNow()
{
    cancelPendingUpdate();
    rebuildGraph();
}

//==============================================================================
// Graph construction

std::shared_ptr<ClipStream> EngineBridge::getOrCreateStream (const juce::String& key, const juce::File& file)
{
    if (const auto it = streams.find (key); it != streams.end())
        return it->second;

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
    {
        if (! reportedMissingFiles.contains (file.getFullPathName()))
        {
            reportedMissingFiles.add (file.getFullPathName());
            if (onProblem)
                onProblem ("Audio file missing or unreadable:\n" + file.getFullPathName());
        }
        return nullptr;
    }

    if (session != nullptr && std::abs (reader->sampleRate - session->getSampleRate()) > 0.5
        && ! reportedMissingFiles.contains (file.getFullPathName()))
    {
        reportedMissingFiles.add (file.getFullPathName());
        if (onProblem)
            onProblem (file.getFileName() + " is " + juce::String (reader->sampleRate) + " Hz but the session runs at "
                       + juce::String (session->getSampleRate()) + " Hz, so it will play at the wrong speed. "
                       "Import it again to convert it.");
    }

    return std::make_shared<ClipStream> (std::move (reader), diskCacheFrames);
}

void EngineBridge::buildSegments (const juce::ValueTree& t, TrackRender& render,
                                  std::vector<std::shared_ptr<ClipStream>>& usedStreams,
                                  std::map<juce::String, std::shared_ptr<ClipStream>>& keep)
{
    const auto clipsNode = track::clips (t);
    const int numClips = clipsNode.getNumChildren();

    for (int i = 0; i < numClips; ++i)
    {
        const auto c = clipsNode.getChild (i);
        const auto start = clip::start (c);
        const auto end = clip::end (c);
        if (end <= start)
            continue;

        // Later siblings sit on top: subtract them to find what of this clip is audible.
        std::vector<Interval> visible { { start, end } };
        for (int j = i + 1; j < numClips && ! visible.empty(); ++j)
        {
            const auto above = clipsNode.getChild (j);
            subtract (visible, clip::start (above), clip::end (above));
        }

        const auto file = session->resolveStoredPath (c[ids::file].toString());
        for (const auto& [s, e] : visible)
        {
            const auto sourceOffset = clip::offset (c) + (s - start);

            // One stream per audible segment: two segments of the same clip
            // read different file regions and must not fight over buffers.
            const auto key = juce::String (clip::id (c)) + "|" + juce::String (sourceOffset) + "|"
                             + juce::String (diskCacheFrames) + "|" + file.getFullPathName();
            auto stream = getOrCreateStream (key, file);
            if (stream == nullptr)
                continue;

            keep[key] = stream;
            usedStreams.push_back (stream);

            ClipSegment seg;
            seg.start = s;
            seg.length = e - s;
            seg.sourceOffset = sourceOffset;
            seg.gain = (float) decibelsToGain (clip::gainDb (c));
            seg.clipStart = start;
            seg.clipEnd = end;
            seg.fadeIn = juce::jlimit<juce::int64> (0, end - start, clip::fadeIn (c));
            seg.fadeOut = juce::jlimit<juce::int64> (0, end - start, clip::fadeOut (c));
            seg.stream = std::move (stream);
            render.segments.push_back (std::move (seg));
        }
    }

    std::sort (render.segments.begin(), render.segments.end(),
               [] (const ClipSegment& a, const ClipSegment& b) { return a.start < b.start; });
}

void EngineBridge::rebuildGraph()
{
    auto graph = std::make_unique<RenderGraph>();
    graph->sampleRate = engine.getSampleRate();
    graph->maxBlockSize = engine.getMaxBlockSize() > 0 ? engine.getMaxBlockSize() : 4096;

    std::vector<std::shared_ptr<ClipStream>> usedStreams;
    std::map<juce::String, std::shared_ptr<ClipStream>> keep;

    if (session != nullptr)
    {
        for (const auto& t : session->getTracksNode())
        {
            TrackRender render;
            render.state = ensureTrackState (t);
            render.numChannels = track::channels (t);
            render.inputChannel = track::inputChannel (t);

            buildSegments (t, render, usedStreams, keep);

            for (int slot = 0; slot < Session::maxInserts; ++slot)
                if (auto s = getInsert (track::id (t), slot); s != nullptr && s->plugin != nullptr)
                    render.inserts.push_back (std::move (s));

            if (const auto it = activeTakes.find (track::id (t)); it != activeTakes.end())
                render.take = it->second;

            for (const auto& lane : track::automation (t))
            {
                if (lane[ids::parameter].toString() != "volume")
                    continue;
                std::vector<dsp::AutomationPoint> points;
                for (const auto& p : lane)
                    points.push_back ({ (juce::int64) p[ids::time], decibelsToGain ((double) p[ids::value]) });
                render.volumeAutomation = dsp::AutomationCurve (std::move (points));
            }

            render.buffer.setSize (render.numChannels, graph->maxBlockSize);
            render.clipScratch.setSize (2, graph->maxBlockSize);
            render.midi.ensureSize (4096);
            graph->tracks.push_back (std::move (render));
        }
    }

    streams.swap (keep); // streams no longer referenced are released (or kept alive by the old graph)
    diskStreamer.setStreams (std::move (usedStreams));
    engine.publishGraph (std::move (graph));
}

void EngineBridge::setDiskCacheFrames (int frames)
{
    diskCacheFrames = juce::jlimit (4096, 1 << 20, frames);
    rebuildNow();
}

//==============================================================================
// Recording

juce::Result EngineBridge::startRecording()
{
    if (session == nullptr)
        return juce::Result::fail ("No session is open.");
    if (isRecording())
        return juce::Result::ok();
    if (! engine.isDeviceRunning())
        return juce::Result::fail ("No audio device is running. Choose one in Setup > Playback Engine.");

    const double sampleRate = engine.getSampleRate();
    if (std::abs (sampleRate - session->getSampleRate()) > 0.5)
        return juce::Result::fail ("The audio device runs at " + juce::String (sampleRate) + " Hz but the session is "
                                   + juce::String (session->getSampleRate()) + " Hz. Change the sample rate in Setup > Playback Engine.");

    std::map<int, std::shared_ptr<RecordTake>> takes;
    const bool aiff = session->getFileFormat() == "aiff";
    const int bits = session->getBitDepth();

    for (const auto& t : session->getTracksNode())
    {
        if (! track::recordArm (t))
            continue;
        if (track::inputChannel (t) < 0)
            return juce::Result::fail ("Track \"" + track::name (t) + "\" is armed but has no input. Choose one in its I/O section.");

        auto take = std::make_shared<RecordTake>();
        take->trackId = track::id (t);
        take->numChannels = track::channels (t);
        take->inputChannel = track::inputChannel (t);
        take->file = session->createUniqueAudioFile (track::name (t));

        std::unique_ptr<juce::FileOutputStream> fileStream = take->file.createOutputStream();
        if (fileStream == nullptr || ! fileStream->openedOk())
            return juce::Result::fail ("Could not create " + take->file.getFullPathName() + ".");
        std::unique_ptr<juce::OutputStream> stream = std::move (fileStream);

        auto options = juce::AudioFormatWriterOptions{}
                           .withSampleRate (sampleRate)
                           .withNumChannels (take->numChannels)
                           .withBitsPerSample (bits);
        if (bits == 32)
            options = options.withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

        juce::WavAudioFormat wav;
        juce::AiffAudioFormat aiffFormat;
        juce::AudioFormat& format = aiff ? static_cast<juce::AudioFormat&> (aiffFormat) : wav;
        auto writer = format.createWriterFor (stream, options);
        if (writer == nullptr)
            return juce::Result::fail ("Could not create a " + format.getFormatName() + " writer for " + take->file.getFileName() + ".");

        // A 4-second FIFO between the audio thread and the disk.
        take->writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer.release(), recordWriterThread,
                                                                                (int) (sampleRate * 4.0));
        takes[take->trackId] = std::move (take);
    }

    if (takes.empty())
        return juce::Result::fail ("No tracks are record-armed.");

    activeTakes = std::move (takes);
    rebuildNow();
    engine.getTransport().play (Transport::State::recording);
    return juce::Result::ok();
}

void EngineBridge::stopRecording()
{
    engine.getTransport().stop();
    if (activeTakes.empty())
        return;

    for (auto& [id, take] : activeTakes)
        takesToFinalise.push_back (take);
    activeTakes.clear();

    rebuildNow();     // the next graph no longer references the takes
    startTimer (50);  // finalise once the audio thread has released them
}

void EngineBridge::timerCallback()
{
    for (auto it = takesToFinalise.begin(); it != takesToFinalise.end();)
    {
        // use_count() == 1: only this list still holds the take, so the graph
        // that recorded into it has been retired and the audio thread is done.
        if (it->use_count() == 1 || ! engine.isDeviceRunning())
        {
            finaliseTake (**it);
            it = takesToFinalise.erase (it);
        }
        else
        {
            ++it;
        }
    }

    if (takesToFinalise.empty())
        stopTimer();
}

void EngineBridge::finaliseTake (RecordTake& take)
{
    take.writer.reset(); // flushes the FIFO and closes the file

    const auto start = take.startSample.load();
    const auto written = take.samplesWritten.load();

    juce::int64 length = 0;
    if (start >= 0 && written > 0)
        if (std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (take.file)); reader != nullptr)
            length = juce::jmin (written, reader->lengthInSamples);

    if (length <= 0)
    {
        take.file.deleteFile();
        return;
    }

    if (session == nullptr)
        return; // the session was closed mid-take; the file stays in its Audio Files folder

    auto t = session->getTrackById (take.trackId);
    if (! t.isValid())
    {
        if (onProblem)
            onProblem ("The track was deleted while recording. The take was kept at:\n" + take.file.getFullPathName());
        return;
    }

    auto& um = session->getUndoManager();
    um.beginNewTransaction ("Record");
    session->addClip (t, take.file, start, 0, length, length, take.file.getFileNameWithoutExtension(), &um);

    if (take.overflowed.load() && onProblem)
        onProblem ("The disk could not keep up while recording \"" + track::name (t)
                   + "\". Part of the take was lost. Try a larger hardware buffer or a faster drive.");
}

} // namespace meridian
