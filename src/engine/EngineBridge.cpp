#include "EngineBridge.h"

#include "model/Automation.h"
#include "model/MidiModel.h"
#include "model/Routing.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

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
    {
        // Instances may outlive this object inside retired graphs: stop them calling us.
        const juce::ScopedLock sl (pluginLock);
        for (auto& [key, slot] : insertSlots)
            if (slot->plugin != nullptr)
            {
                slot->plugin->removeListener (this);
                slot->latencyListener = nullptr;
            }
    }
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
    sendStates.clear();
    instruments.clear();
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

std::shared_ptr<SendRealtimeState> EngineBridge::ensureSendState (const juce::ValueTree& s)
{
    auto& state = sendStates[{ track::id (send::owningTrack (s)), send::slot (s) }];
    if (state == nullptr)
        state = std::make_shared<SendRealtimeState>();
    syncSendState (s);
    return state;
}

void EngineBridge::syncSendState (const juce::ValueTree& s)
{
    const auto it = sendStates.find ({ track::id (send::owningTrack (s)), send::slot (s) });
    if (it == sendStates.end())
        return;

    auto& state = *it->second;
    state.levelDb.store (send::levelDb (s));
    state.pan.store (send::pan (s));
    state.muted.store (send::mute (s));
    state.preFader.store (send::preFader (s));
}

void EngineBridge::removeTrackResources (int trackId)
{
    trackStates.erase (trackId);
    instruments.erase (trackId); // graphs still using it keep it alive until retired
    for (auto it = sendStates.begin(); it != sendStates.end();)
        it = it->first.first == trackId ? sendStates.erase (it) : std::next (it);
    for (auto it = delayLines.begin(); it != delayLines.end();)
        it = it->first.first == trackId ? delayLines.erase (it) : std::next (it);

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
        slot->plugin->addListener (this); // latency changes re-run delay compensation
        slot->latencyListener = this;
    }

    insertSlots[{ trackId, slotIndex }] = std::move (slot);
}

bool EngineBridge::insertPlugin (juce::ValueTree t, int slot, const juce::PluginDescription& description, juce::String& error)
{
    if (session == nullptr)
        return false;
    if (description.isInstrument && (! track::isInstrument (t) || slot != 0))
    {
        error = description.name + " is an instrument: it goes in insert A of an instrument track, where it plays the track's MIDI.";
        return false;
    }

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
        else if (property == ids::inputChannel || property == ids::outputBus || property == ids::kind)
        {
            triggerAsyncUpdate(); // routing changed: new render order
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
        {
            if (auto slot = getInsert (trackIdOfInsert (tree), tree[ids::slot]))
                slot->bypassed.store ((bool) tree[ids::bypass]);
            triggerAsyncUpdate(); // a bypassed plug-in adds no latency: recompensate
        }
        return;
    }

    if (tree.hasType (ids::SEND))
    {
        if (property == ids::target || property == ids::slot)
            triggerAsyncUpdate();
        else
            syncSendState (tree); // level, pan, mute, pre/post: no rebuild
        return;
    }

    if (tree.hasType (ids::SESSION) && property == ids::tempo)
    {
        triggerAsyncUpdate(); // MIDI notes are positioned in beats: they move with the tempo
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

std::shared_ptr<ClipStream> EngineBridge::getOrCreateStream (const juce::String& key, const juce::File& file, bool shared)
{
    if (shared)
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

void EngineBridge::buildSegments (const juce::ValueTree& t, TrackRender& render, bool sharedStreams,
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
        if (end <= start || clip::isMidi (c)) // MIDI clips have no audio file
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
            auto stream = getOrCreateStream (key, file, sharedStreams);
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

void EngineBridge::buildMidiEvents (const juce::ValueTree& t, TrackRender& render) const
{
    // Notes are in ticks from each clip's content origin, timed by the tempo map
    // from there (TempoMap::samplesFromOrigin); the timeline is in samples at the
    // session rate. A note sounds if it starts inside its clip, and is cut off at
    // the clip's end.
    const auto tempo = midimodel::tempoMap (*session);
    const double rate = session->getSampleRate();

    for (const auto& c : track::clips (t))
    {
        if (! clip::isMidi (c))
            continue;
        const auto origin = clip::contentOrigin (c);
        const auto start = clip::start (c), end = clip::end (c);

        // Keep the sequence in a variable: iterating readNotes(c).getNotes() directly
        // would loop over a destroyed temporary (no lifetime extension through a call).
        const auto notes = midimodel::readNotes (c);
        for (const auto& n : notes.getNotes())
        {
            const auto on = (juce::int64) tempo.samplesFromOrigin (origin, n.start, rate); // int64_t is long on Linux
            if (on < start || on >= end)
                continue;
            const auto off = juce::jlimit (on + 1, end, (juce::int64) tempo.samplesFromOrigin (origin, n.end(), rate));
            render.events.push_back ({ on, n.pitch, n.velocity, true });
            render.events.push_back ({ off, n.pitch, 0.0f, false });
        }

        // The sustain pedal: down and up inside the clip. A range already under way where
        // the clip starts goes down at its start; one still held at its end lifts there.
        for (const auto& range : midimodel::readSustain (c))
        {
            const auto down = std::max (start, (juce::int64) tempo.samplesFromOrigin (origin, range.start, rate));
            const auto up = std::min (end, (juce::int64) tempo.samplesFromOrigin (origin, range.end(), rate));
            if (down >= up)
                continue;
            render.events.push_back ({ down, midi::sustainPedal, 0.0f, true });
            render.events.push_back ({ up, midi::sustainPedal, 0.0f, false });
        }

        // The wheels (the event's velocity is the value): every move inside the clip at
        // its time; a value set before the clip starts applies at its start; at its end
        // the wheel returns to rest. That reset is an "off", so it sorts before the
        // next clip's own value at the same time.
        for (const int control : { midi::pitchBend, midi::modWheel })
        {
            const auto points = midimodel::readControl (c, control);
            if (points.empty())
                continue;
            bool carried = false;
            float carriedValue = 0.0f;
            for (const auto& p : points)
            {
                const auto at = (juce::int64) tempo.samplesFromOrigin (origin, p.tick, rate);
                if (at < start)
                {
                    carried = true;
                    carriedValue = p.value;
                }
                else if (at < end)
                {
                    render.events.push_back ({ at, control, p.value, true });
                }
            }
            if (carried)
                render.events.push_back ({ start, control, carriedValue, true });
            render.events.push_back ({ end, control, 0.0f, false });
        }
    }

    std::sort (render.events.begin(), render.events.end(), [] (const MidiEvent& a, const MidiEvent& b)
    {
        return a.time != b.time ? a.time < b.time : (! a.on && b.on); // offs first at equal times
    });
}

std::unique_ptr<RenderGraph> EngineBridge::buildGraph (double sampleRate, int maxBlockSize, bool sharedStreams,
                                                       std::vector<std::shared_ptr<ClipStream>>& usedStreams,
                                                       std::map<juce::String, std::shared_ptr<ClipStream>>& keep)
{
    auto graph = std::make_unique<RenderGraph>();
    graph->sampleRate = sampleRate;
    graph->maxBlockSize = maxBlockSize;

    if (session != nullptr)
    {
        graph->clickGrid = std::make_shared<const midi::ClickGrid> (midimodel::tempoMap (*session), midimodel::meterMap (*session),
                                                                    sampleRate > 0.0 ? sampleRate : session->getSampleRate());
        // Render order: audio tracks first, then aux tracks sorted so that every
        // aux comes after all auxes feeding it (Kahn's algorithm). The model
        // refuses cycles; if a hand-edited file contains one anyway, the auxes
        // involved are appended and their backward routes ignored below.
        std::vector<juce::ValueTree> order, auxes;
        for (const auto& t : session->getTracksNode())
            (track::isAux (t) ? auxes : order).push_back (t);

        std::map<int, int> pendingInputs; // aux id -> number of aux tracks feeding it
        for (const auto& a : auxes)
            pendingInputs[track::id (a)] = 0;
        for (const auto& a : auxes)
            for (const int target : routing::directDestinations (*session, a))
                ++pendingInputs[target];

        std::vector<juce::ValueTree> sortedAux;
        std::set<int> placed;
        for (bool progress = true; progress && sortedAux.size() < auxes.size();)
        {
            progress = false;
            for (const auto& a : auxes)
            {
                const int id = track::id (a);
                if (placed.count (id) != 0 || pendingInputs[id] != 0)
                    continue;
                sortedAux.push_back (a);
                placed.insert (id);
                for (const int target : routing::directDestinations (*session, a))
                    --pendingInputs[target];
                progress = true;
            }
        }
        if (sortedAux.size() < auxes.size())
        {
            for (const auto& a : auxes)
                if (placed.count (track::id (a)) == 0)
                    sortedAux.push_back (a);
            if (onProblem)
                onProblem ("The session's bus routing contains a loop. The looping routes are ignored.");
        }

        std::map<int, int> busOfAux, positionOf;
        for (size_t i = 0; i < sortedAux.size(); ++i)
            busOfAux[track::id (sortedAux[i])] = (int) i;
        order.insert (order.end(), sortedAux.begin(), sortedAux.end());
        for (size_t i = 0; i < order.size(); ++i)
            positionOf[track::id (order[i])] = (int) i;

        graph->buses.resize (sortedAux.size());
        for (auto& b : graph->buses)
            for (int ch = 0; ch < 2; ++ch)
            {
                b.sum[ch].assign ((size_t) maxBlockSize, 0.0);
                b.comp[ch].assign ((size_t) maxBlockSize, 0.0);
            }

        // A route is usable if it targets an existing aux rendered later than its source.
        auto busFor = [&] (const juce::ValueTree& source, int targetId) -> int
        {
            const auto bus = busOfAux.find (targetId);
            if (targetId == 0 || bus == busOfAux.end() || positionOf[targetId] <= positionOf[track::id (source)])
                return -1;
            return bus->second;
        };

        for (const auto& t : order)
        {
            TrackRender render;
            render.state = ensureTrackState (t);
            render.numChannels = track::channels (t);
            render.inputChannel = track::inputChannel (t);
            render.isAux = track::isAux (t);
            render.ownBus = render.isAux ? busOfAux[track::id (t)] : -1;
            render.outputBus = busFor (t, track::outputBus (t));

            for (const auto& s : track::sends (t))
                if (const int bus = busFor (t, send::target (s)); bus >= 0)
                    render.sends.push_back ({ ensureSendState (s), bus, send::slot (s), {}, {} });

            if (render.isAux)
                for (const auto& other : order)
                    if (other != t && routing::feeds (*session, track::id (other), track::id (t)))
                        render.upstream.push_back (ensureTrackState (other));

            if (track::isInstrument (t))
            {
                // Live graphs share one synth per track across rebuilds; an
                // offline render gets its own, so it never disturbs playback.
                auto& live = instruments[track::id (t)];
                if (live == nullptr)
                    live = std::make_shared<InstrumentState>();
                render.instrument = sharedStreams ? live : std::make_shared<InstrumentState>();
                render.drumKit = track::isDrumKit (t);
                render.synthParams = midimodel::readSynthParams (track::synth (t));
                render.drumParams = midimodel::readDrumParams (track::synth (t));
                render.synthParamsVersion = ++graphVersion;
                buildMidiEvents (t, render);
                if (auto first = getInsert (track::id (t), 0);
                    first != nullptr && first->plugin != nullptr && first->description.isInstrument && first->plugin->acceptsMidi())
                    render.pluginInstrument = true;
            }
            else if (! render.isAux)
            {
                buildSegments (t, render, sharedStreams, usedStreams, keep);
            }

            for (int slot = 0; slot < Session::maxInserts; ++slot)
                if (auto s = getInsert (track::id (t), slot); s != nullptr && s->plugin != nullptr)
                    render.inserts.push_back (std::move (s));

            if (const auto it = activeTakes.find (track::id (t)); it != activeTakes.end())
                render.take = it->second;

            for (const auto& lane : track::automation (t))
            {
                const auto parameter = lane[ids::parameter].toString();
                std::vector<dsp::AutomationPoint> points;
                for (const auto& p : automation::points (lane))
                    points.push_back ({ p.time, p.value }); // lane units; the engine converts

                // Stepped lanes (mute) hold each value until the next point: a point one
                // sample earlier with the previous value turns the linear curve into steps.
                if (const auto range = automation::rangeOf (parameter); range.has_value() && range->stepped)
                {
                    std::vector<dsp::AutomationPoint> steps;
                    for (size_t i = 0; i < points.size(); ++i)
                    {
                        if (i > 0 && points[i].time - 1 > points[i - 1].time)
                            steps.push_back ({ points[i].time - 1, points[i - 1].value });
                        steps.push_back (points[i]);
                    }
                    points = std::move (steps);
                }
                dsp::AutomationCurve curve (std::move (points));

                if (parameter == "volume")
                    render.volumeAutomation = std::move (curve);
                else if (parameter == "pan")
                    render.panAutomation = std::move (curve);
                else if (parameter == "mute")
                    render.muteAutomation = std::move (curve);
                else if (const int sendSlot = automation::sendTarget (parameter); sendSlot >= 0)
                {
                    for (auto& send : render.sends)
                        if (send.slot == sendSlot)
                            send.levelAutomation = std::move (curve);
                }
                else if (const auto [slot, index] = automation::pluginTarget (parameter); slot >= 0)
                {
                    if (auto s = getInsert (track::id (t), slot); s != nullptr && s->plugin != nullptr)
                        if (auto* p = s->plugin->getParameters()[index])
                            render.parameterAutomation.push_back ({ p, std::move (curve), -1.0f });
                }
            }

            render.buffer.setSize (render.numChannels, graph->maxBlockSize);
            render.clipScratch.setSize (2, graph->maxBlockSize);
            render.midi.ensureSize (4096);
            graph->tracks.push_back (std::move (render));
        }
    }

    assignDelayCompensation (*graph, sharedStreams);
    return graph;
}

PathDelay EngineBridge::delayFor (int trackId, int path, int samples, bool shared)
{
    if (samples <= 0)
        return {};

    // Offline renders get their own lines so they never share history with live playback.
    if (! shared)
        return { std::make_shared<dsp::DelayLine> (samples), samples };

    auto& line = delayLines[{ trackId, path }];
    if (line == nullptr || line->getMaximumDelay() < samples)
        line = std::make_shared<dsp::DelayLine> (juce::jmax (samples * 2, 4096)); // headroom: latency changes rarely reallocate
    return { line, samples };
}

void EngineBridge::assignDelayCompensation (RenderGraph& graph, bool sharedDelayLines)
{
    graph.compensatedLatency = 0;
    if (! delayCompensationEnabled)
        return;

    // Latency arriving at each summing point. The graph is in render order, so
    // every contribution to a bus is seen before the bus itself.
    std::vector<int> busInputLatency (graph.buses.size(), 0);
    struct Path { TrackRender* track; SendRender* send; int destination; int arrival; };
    std::vector<Path> paths;

    for (auto& t : graph.tracks)
    {
        // An instrument's own synth delays its output like a plug-in would.
        int pluginLatency = t.instrument == nullptr || t.pluginInstrument ? 0 // a plug-in instrument reports its own, below
                          : t.drumKit ? synth::DrumSynth::getLatencySamples() : synth::PolySynth::getLatencySamples();
        for (const auto& slot : t.inserts)
            if (! slot->bypassed.load())
                pluginLatency += juce::jmax (0, slot->plugin->getLatencySamples());

        const int arrival = (t.isAux ? busInputLatency[(size_t) t.ownBus] : 0) + pluginLatency;

        paths.push_back ({ &t, nullptr, t.outputBus, arrival });
        if (t.outputBus >= 0)
            busInputLatency[(size_t) t.outputBus] = juce::jmax (busInputLatency[(size_t) t.outputBus], arrival);

        for (auto& s : t.sends)
        {
            paths.push_back ({ &t, &s, s.targetBus, arrival });
            busInputLatency[(size_t) s.targetBus] = juce::jmax (busInputLatency[(size_t) s.targetBus], arrival);
        }
    }

    for (const auto& p : paths)
        if (p.destination < 0)
            graph.compensatedLatency = juce::jmax (graph.compensatedLatency, p.arrival);

    // Delay every path up to the slowest arrival at its destination.
    for (const auto& p : paths)
    {
        const int target = p.destination < 0 ? graph.compensatedLatency : busInputLatency[(size_t) p.destination];
        const int trackId = p.track->state->trackId;
        if (p.send == nullptr)
            p.track->outputCompensation = delayFor (trackId, -1, target - p.arrival, sharedDelayLines);
        else
            p.send->compensation = delayFor (trackId, p.send->slot, target - p.arrival, sharedDelayLines);
    }
}

void EngineBridge::setDelayCompensationEnabled (bool enabled)
{
    delayCompensationEnabled = enabled;
    rebuildNow();
}

void EngineBridge::rebuildGraph()
{
    const int maxBlock = engine.getMaxBlockSize() > 0 ? engine.getMaxBlockSize() : 4096;

    if (liveSuspended)
    {
        // An offline render owns the plug-ins: the live engine plays nothing.
        auto silent = std::make_unique<RenderGraph>();
        silent->maxBlockSize = maxBlock;
        engine.publishGraph (std::move (silent));
        return;
    }

    std::vector<std::shared_ptr<ClipStream>> usedStreams;
    std::map<juce::String, std::shared_ptr<ClipStream>> keep;
    auto graph = buildGraph (engine.getSampleRate(), maxBlock, true, usedStreams, keep);

    streams.swap (keep); // streams no longer referenced are released (or kept alive by the old graph)
    diskStreamer.setStreams (std::move (usedStreams));
    engine.publishGraph (std::move (graph));
}

std::unique_ptr<RenderGraph> EngineBridge::buildOfflineGraph (double sampleRate, int maxBlockSize)
{
    std::vector<std::shared_ptr<ClipStream>> unusedList;
    std::map<juce::String, std::shared_ptr<ClipStream>> unusedKeep;
    return buildGraph (sampleRate, maxBlockSize, false, unusedList, unusedKeep);
}

bool EngineBridge::suspendLive()
{
    liveSuspended = true;
    rebuildNow(); // publishes a silent graph

    // Wait (briefly) until the audio thread has adopted it, i.e. has stopped
    // touching the session's plug-ins.
    for (int i = 0; i < 500 && engine.isDeviceRunning() && engine.hasPendingGraph(); ++i)
        juce::Thread::sleep (2);
    const bool suspended = ! (engine.isDeviceRunning() && engine.hasPendingGraph());

    // A bounce starts from silence: no reverb tail, delay repeats or open gate
    // left over from earlier playback, so the same session always renders the same.
    if (suspended)
    {
        const juce::ScopedLock sl (pluginLock);
        for (auto& [key, slot] : insertSlots)
            if (slot->plugin != nullptr)
                slot->plugin->reset();
    }
    return suspended;
}

void EngineBridge::resumeLive()
{
    liveSuspended = false;
    rebuildNow();
}

void EngineBridge::setDiskCacheFrames (int frames)
{
    diskCacheFrames = juce::jlimit (4096, 1 << 20, frames);
    rebuildNow();
}

//==============================================================================
// Recording

juce::Result EngineBridge::startRecording (int hardwareLatency, juce::int64 countInSamples, int outputLatency)
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
    std::vector<int> midiTracks;
    const bool aiff = session->getFileFormat() == "aiff";
    const int bits = session->getBitDepth();

    for (const auto& t : session->getTracksNode())
    {
        if (! track::recordArm (t))
            continue;
        if (track::isInstrument (t))
        {
            midiTracks.push_back (track::id (t)); // records the keyboard, not an audio input
            continue;
        }
        if (track::inputChannel (t) < 0)
            return juce::Result::fail ("Track \"" + track::name (t) + "\" is armed but has no input. Choose one in its I/O section.");

        auto take = std::make_shared<RecordTake>();
        take->trackId = track::id (t);
        take->numChannels = track::channels (t);
        take->inputChannel = track::inputChannel (t);
        take->latencyCompensation = juce::jmax (0, hardwareLatency) + engine.getTotalLatencySamples();
        lastRecordingCompensation = take->latencyCompensation;
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

    if (takes.empty() && midiTracks.empty())
        return juce::Result::fail ("No tracks are record-armed.");

    // MIDI: forget anything captured before this take; place notes as heard
    // (output latency + the engine's latency + the one block live input waits).
    AudioEngine::RecordedMidi stale;
    while (engine.popRecordedMidi (stale)) {}
    capturedMidi.clear();
    midiTakeTracks = std::move (midiTracks);
    midiRecordStart = engine.getTransport().getPosition();
    midiCompensation = juce::jmax (0, outputLatency) + engine.getTotalLatencySamples() + engine.getMaxBlockSize();

    activeTakes = std::move (takes);
    rebuildNow();
    engine.getTransport().play (Transport::State::recording, countInSamples);
    if (! midiTakeTracks.empty())
        startTimer (50); // drains the captured notes while recording
    return juce::Result::ok();
}

void EngineBridge::setControlTouched (int trackId, uint32_t bit, bool isTouched)
{
    const auto it = trackStates.find (trackId);
    if (it == trackStates.end())
        return;
    if (isTouched)
        it->second->touched.fetch_or (bit, std::memory_order_relaxed);
    else
        it->second->touched.fetch_and (~bit, std::memory_order_relaxed);
}

void EngineBridge::drainRecordedMidi()
{
    AudioEngine::RecordedMidi m;
    while (engine.popRecordedMidi (m))
        capturedMidi[m.trackId].push_back ({ m.time, m.pitch, m.velocity, m.on });
}

void EngineBridge::finaliseMidiTakes (juce::int64 recordEnd)
{
    drainRecordedMidi();
    const auto trackIds = std::exchange (midiTakeTracks, {});
    auto captured = std::exchange (capturedMidi, {});
    if (session == nullptr)
        return;

    auto& um = session->getUndoManager();
    bool began = false;
    for (const int id : trackIds)
    {
        auto t = session->getTrackById (id);
        auto& events = captured[id];
        if (! t.isValid() || events.empty())
            continue;
        const auto take = midi::buildTake (std::move (events), midiRecordStart, recordEnd, midiCompensation,
                                           midimodel::tempoMap (*session), session->getSampleRate());
        if (take.notes.empty())
            continue;
        if (! began)
            um.beginNewTransaction ("Record MIDI");
        began = true;
        auto clip = session->addMidiClip (t, take.clipStart, take.clipLength, "MIDI Take", &um);
        for (const auto& note : take.notes)
            midimodel::addNote (clip, note, &um);
        midimodel::writeSustain (clip, take.sustain, &um);
        midimodel::writeControl (clip, midi::pitchBend, take.pitchBend, &um);
        midimodel::writeControl (clip, midi::modWheel, take.modWheel, &um);
    }
}

void EngineBridge::stopRecording()
{
    const auto recordEnd = engine.getTransport().getPosition();
    engine.getTransport().stop();
    if (! midiTakeTracks.empty())
        finaliseMidiTakes (recordEnd);
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
    if (! midiTakeTracks.empty())
        drainRecordedMidi();

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

    if (takesToFinalise.empty() && midiTakeTracks.empty())
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

    // Place the take where the performer played it: the sound they heard was
    // late by the round-trip latency, so everything they recorded is too.
    auto clipStart = start - take.latencyCompensation;
    juce::int64 offset = 0;
    if (clipStart < 0)
    {
        offset = -clipStart; // recorded before the session start: trim the head
        clipStart = 0;
    }
    if (length - offset <= 0)
    {
        take.file.deleteFile();
        return;
    }

    auto& um = session->getUndoManager();
    um.beginNewTransaction ("Record");
    session->addClip (t, take.file, clipStart, offset, length - offset, length, take.file.getFileNameWithoutExtension(), &um);

    if (take.overflowed.load() && onProblem)
        onProblem ("The disk could not keep up while recording \"" + track::name (t)
                   + "\". Part of the take was lost. Try a larger hardware buffer or a faster drive.");
}

} // namespace meridian
