#include "Session.h"

#include "ConsolePresets.h"

#include "SessionRepair.h"

namespace meridian
{
namespace
{
// Track colours cycle through a palette in the spirit of classic DAW colour
// banks: saturated enough to tell tracks apart on a dark background.
const juce::uint32 trackPalette[] = {
    0xff5a8dd6, 0xff4fb3a9, 0xff8bc34a, 0xffd4b44a, 0xffd98a3d,
    0xffd0564f, 0xffc0559b, 0xff8a6fd1, 0xff4a9fd8, 0xff6fbf73,
};
} // namespace

Session::Session (juce::File file, juce::ValueTree tree)
    : sessionFile (std::move (file)), state (std::move (tree))
{
    ensureStructure (state);
    state.addListener (this);
}

Session::~Session()
{
    state.removeListener (this);
}

juce::ValueTree Session::createEmptyState (const Settings& settings)
{
    juce::ValueTree s (ids::SESSION);
    s.setProperty (ids::name, settings.name, nullptr);
    s.setProperty (ids::sampleRate, settings.sampleRate, nullptr);
    s.setProperty (ids::bitDepth, settings.bitDepth, nullptr);
    s.setProperty (ids::fileFormat, settings.fileFormat, nullptr);
    s.setProperty (ids::formatVersion, formatVersion, nullptr);
    s.setProperty (ids::nextId, 1, nullptr);
    s.setProperty (ids::tempo, 120.0, nullptr);
    s.appendChild (juce::ValueTree (ids::TRACKS), nullptr);
    return s;
}

void Session::ensureStructure (juce::ValueTree& s)
{
    if (! s.getChildWithName (ids::TRACKS).isValid())
        s.appendChild (juce::ValueTree (ids::TRACKS), nullptr);

    for (auto t : s.getChildWithName (ids::TRACKS))
        for (const auto* childType : { &ids::CLIPS, &ids::INSERTS, &ids::SENDS, &ids::AUTOMATION })
            if (! t.getChildWithName (*childType).isValid())
                t.appendChild (juce::ValueTree (*childType), nullptr);
}

std::unique_ptr<Session> Session::createNew (const juce::File& parentFolder, const Settings& settings, juce::String& error)
{
    const auto safeName = juce::File::createLegalFileName (settings.name.trim());
    if (safeName.isEmpty())
    {
        error = "Please enter a session name.";
        return nullptr;
    }

    const auto folder = parentFolder.getChildFile (safeName);
    if (folder.exists())
    {
        error = "A folder named \"" + safeName + "\" already exists in " + parentFolder.getFullPathName() + ".";
        return nullptr;
    }

    if (! folder.getChildFile (audioFolderName).createDirectory())
    {
        error = "Could not create the session folder " + folder.getFullPathName() + ".";
        return nullptr;
    }

    auto s = settings;
    s.name = safeName;
    std::unique_ptr<Session> session (new Session (folder.getChildFile (safeName + fileExtension), createEmptyState (s)));

    // A new session starts on the gentlest measured console colour (ROADMAP "smart
    // defaults"). Only here: sessions saved without console settings keep the old defaults.
    if (const auto* warm = consolepresets::find ("Warm Tape"))
        consolepresets::apply (session->getState(), *warm, nullptr);

    if (! session->save (error))
        return nullptr;
    return session;
}

namespace
{
juce::ValueTree readSessionTree (const juce::File& file, juce::String& error)
{
    auto xml = juce::parseXML (file);
    if (xml == nullptr || ! xml->hasTagName (ids::SESSION.toString()))
    {
        error = file.getFileName() + " is not a valid session file.";
        return {};
    }

    auto tree = juce::ValueTree::fromXml (*xml);
    if ((int) tree.getProperty (ids::formatVersion, 0) > Session::formatVersion)
    {
        error = file.getFileName() + " was saved by a newer version of this application.";
        return {};
    }
    return tree;
}
} // namespace

std::unique_ptr<Session> Session::load (const juce::File& file, juce::String& error)
{
    auto tree = readSessionTree (file, error);
    if (! tree.isValid())
        return nullptr;

    std::unique_ptr<Session> session (new Session (file, std::move (tree)));
    session->loadRepairs = repairSession (*session);
    session->dirty = ! session->loadRepairs.isEmpty(); // the repaired version is not on disk yet
    return session;
}

std::unique_ptr<Session> Session::loadBackup (const juce::File& backupFile, const juce::File& file, juce::String& error)
{
    auto tree = readSessionTree (backupFile, error);
    if (! tree.isValid())
        return nullptr;

    std::unique_ptr<Session> session (new Session (file, std::move (tree)));
    session->loadRepairs = repairSession (*session);
    session->dirty = true; // the recovered changes are not in the session file yet
    return session;
}

bool Session::writeTo (const juce::File& target, juce::String& error) const
{
    auto xml = state.createXml();
    if (xml == nullptr)
    {
        error = "Could not serialise the session.";
        return false;
    }

    // Write to a temporary file first and swap it in, so a crash or a full
    // disk mid-save can never leave a truncated session behind.
    juce::TemporaryFile temp (target);
    if (! xml->writeTo (temp.getFile()) || ! temp.overwriteTargetFileWithTemporary())
    {
        error = "Could not write " + target.getFullPathName() + ".";
        return false;
    }
    return true;
}

bool Session::save (juce::String& error)
{
    if (! writeTo (sessionFile, error))
        return false;
    dirty = false;
    return true;
}

bool Session::saveAs (const juce::File& newFile, juce::String& error)
{
    const auto target = newFile.withFileExtension (fileExtension);
    if (! target.getParentDirectory().isDirectory() && ! target.getParentDirectory().createDirectory())
    {
        error = "Could not create " + target.getParentDirectory().getFullPathName() + ".";
        return false;
    }

    // Resolve every clip's audio from the old folder before switching.
    std::vector<std::pair<juce::ValueTree, juce::File>> audio;
    for (const auto& t : getTracksNode())
        for (const auto& list : track::allClipLists (t)) // every playlist's clips, not only the active one
            for (const auto& c : list)
                if (const auto stored = c[ids::file].toString(); stored.isNotEmpty())
                    audio.emplace_back (c, resolveStoredPath (stored));

    const auto oldFile = sessionFile;
    const auto oldName = state[ids::name];
    sessionFile = target;

    // Not undoable: these describe where the session lives, not an edit.
    for (auto& [c, file] : audio)
        c.setProperty (ids::file, makeStoredPath (file), nullptr);
    state.setProperty (ids::name, target.getFileNameWithoutExtension(), nullptr);

    if (! save (error))
    {
        // Put everything back: the session still lives where it was.
        sessionFile = oldFile;
        for (auto& [c, file] : audio)
            c.setProperty (ids::file, makeStoredPath (file), nullptr);
        state.setProperty (ids::name, oldName, nullptr);
        return false;
    }
    return true;
}

juce::String Session::getName() const          { return state[ids::name].toString(); }
double Session::getSampleRate() const          { return state.getProperty (ids::sampleRate, 48000.0); }
int Session::getBitDepth() const               { return state.getProperty (ids::bitDepth, 24); }
juce::String Session::getFileFormat() const    { return state.getProperty (ids::fileFormat, "wav").toString(); }

int Session::allocateId()
{
    const int next = state.getProperty (ids::nextId, 1);
    // Not routed through the undo manager: ids must never be reused, even after undo.
    state.setProperty (ids::nextId, next + 1, nullptr);
    return next;
}

//==============================================================================
juce::ValueTree Session::getTracksNode() const { return state.getChildWithName (ids::TRACKS); }
int Session::getNumTracks() const              { return getTracksNode().getNumChildren(); }
juce::ValueTree Session::getTrack (int index) const { return getTracksNode().getChild (index); }

juce::ValueTree Session::getTrackById (int trackId) const
{
    return getTracksNode().getChildWithProperty (ids::id, trackId);
}

int Session::indexOfTrack (const juce::ValueTree& t) const
{
    return getTracksNode().indexOf (t);
}

juce::ValueTree Session::addTrack (const juce::String& name, int numChannels, juce::UndoManager* um, TrackKind kind)
{
    const int newId = allocateId();
    const bool aux = kind == TrackKind::aux;
    const bool drums = kind == TrackKind::drums;
    const bool instrument = kind == TrackKind::instrument || drums;
    const auto colour = aux ? juce::Colour (0xff8f9aa6)          // aux tracks: neutral grey-blue
                      : drums ? juce::Colour (0xffd07a3a)        // drum tracks: orange
                      : instrument ? juce::Colour (0xff3fbf9f)   // instrument tracks: teal
                                   : juce::Colour (trackPalette[(size_t) getNumTracks() % std::size (trackPalette)]);

    juce::ValueTree t (ids::TRACK);
    t.setProperty (ids::id, newId, nullptr);
    t.setProperty (ids::name, name, nullptr);
    t.setProperty (ids::kind, aux ? "aux" : instrument ? "instrument" : "audio", nullptr);
    t.setProperty (ids::channels, aux || instrument ? 2 : juce::jlimit (1, 2, numChannels), nullptr);
    t.setProperty (ids::colour, colour.toString(), nullptr);
    t.setProperty (ids::volumeDb, 0.0, nullptr);
    t.setProperty (ids::pan, 0.0, nullptr);
    t.setProperty (ids::mute, false, nullptr);
    t.setProperty (ids::solo, false, nullptr);
    t.setProperty (ids::recordArm, false, nullptr);
    t.setProperty (ids::inputChannel, aux || instrument ? -1 : 0, nullptr);
    t.setProperty (ids::automationMode, "read", nullptr);
    t.setProperty (ids::outputBus, 0, nullptr);
    t.appendChild (juce::ValueTree (ids::CLIPS), nullptr);
    t.appendChild (juce::ValueTree (ids::INSERTS), nullptr);
    t.appendChild (juce::ValueTree (ids::SENDS), nullptr);
    t.appendChild (juce::ValueTree (ids::AUTOMATION), nullptr);

    if (instrument)
    {
        // A soft, warm default: filtered saw with a gentle envelope (or the drum machine).
        juce::ValueTree synth (ids::SYNTH);
        synth.setProperty (ids::engine, drums ? "drums" : "synth", nullptr);
        synth.setProperty (ids::tune, 0.0, nullptr);
        synth.setProperty (ids::decayScale, 1.0, nullptr);
        synth.setProperty (ids::tone, 0.5, nullptr);
        synth.setProperty (ids::waveform, "saw", nullptr);
        synth.setProperty (ids::detune, 7.0, nullptr);
        synth.setProperty (ids::attack, 0.005, nullptr);
        synth.setProperty (ids::decay, 0.6, nullptr);
        synth.setProperty (ids::sustain, 0.6, nullptr);
        synth.setProperty (ids::release, 0.4, nullptr);
        synth.setProperty (ids::cutoff, 1800.0, nullptr);
        synth.setProperty (ids::resonance, 0.15, nullptr);
        synth.setProperty (ids::filterEnv, 2.0, nullptr);
        synth.setProperty (ids::gainDb, drums ? -6.0 : -12.0, nullptr);
        t.appendChild (synth, nullptr);
    }

    getTracksNode().appendChild (t, um);
    return t;
}

juce::ValueTree Session::addMidiClip (juce::ValueTree t, juce::int64 timelineStart, juce::int64 length,
                                      const juce::String& name, juce::UndoManager* um)
{
    juce::ValueTree c (ids::CLIP);
    c.setProperty (ids::id, allocateId(), nullptr);
    c.setProperty (ids::kind, "midi", nullptr);
    c.setProperty (ids::name, name, nullptr);
    c.setProperty (ids::start, juce::jmax ((juce::int64) 0, timelineStart), nullptr);
    c.setProperty (ids::offset, 0, nullptr);
    c.setProperty (ids::length, juce::jmax ((juce::int64) 1, length), nullptr);
    c.setProperty (ids::sourceLength, 0, nullptr); // no source file: unlimited
    c.setProperty (ids::gainDb, 0.0, nullptr);
    c.setProperty (ids::fadeIn, 0, nullptr);
    c.setProperty (ids::fadeOut, 0, nullptr);
    c.appendChild (juce::ValueTree (ids::NOTES), nullptr);

    track::clips (t).appendChild (c, um);
    return c;
}

double Session::getTempo() const
{
    const double bpm = state.getProperty (ids::tempo, 120.0);
    return std::isfinite (bpm) ? juce::jlimit (20.0, 999.0, bpm) : 120.0;
}

juce::Array<juce::ValueTree> Session::getMarkers() const
{
    juce::Array<juce::ValueTree> result;
    for (const auto& m : state.getChildWithName (ids::MARKERS))
        if (m.hasType (ids::MARKER))
            result.add (m);
    std::stable_sort (result.begin(), result.end(), [] (const juce::ValueTree& a, const juce::ValueTree& b)
    {
        return (juce::int64) a[ids::start] < (juce::int64) b[ids::start];
    });
    return result;
}

juce::ValueTree Session::getMarkerByNumber (int number) const
{
    return state.getChildWithName (ids::MARKERS).getChildWithProperty (ids::number, number);
}

juce::ValueTree Session::addMarker (const juce::String& name, juce::int64 position, juce::UndoManager* um)
{
    auto markers = state.getChildWithName (ids::MARKERS);
    if (! markers.isValid())
    {
        markers = juce::ValueTree (ids::MARKERS);
        state.appendChild (markers, um);
    }

    int number = 1;
    while (markers.getChildWithProperty (ids::number, number).isValid())
        ++number;

    juce::ValueTree m (ids::MARKER);
    m.setProperty (ids::number, number, nullptr);
    m.setProperty (ids::name, name.trim().isNotEmpty() ? name.trim() : "Marker " + juce::String (number), nullptr);
    m.setProperty (ids::start, std::max<juce::int64> (0, position), nullptr);
    markers.appendChild (m, um);
    return m;
}

void Session::removeMarker (const juce::ValueTree& marker, juce::UndoManager* um)
{
    auto markers = state.getChildWithName (ids::MARKERS);
    markers.removeChild (marker, um);
}

void Session::removeTrack (const juce::ValueTree& t, juce::UndoManager* um)
{
    getTracksNode().removeChild (t, um);
}

void Session::moveTrack (const juce::ValueTree& t, int newIndex, juce::UndoManager* um)
{
    auto tracks = getTracksNode();
    const int from = tracks.indexOf (t);
    const int to = juce::jlimit (0, tracks.getNumChildren() - 1, newIndex);
    if (from >= 0 && from != to)
        tracks.moveChild (from, to, um);
}

juce::ValueTree Session::duplicateTrack (const juce::ValueTree& t, juce::UndoManager* um)
{
    auto tracks = getTracksNode();
    const int index = tracks.indexOf (t);
    if (index < 0)
        return {};

    // "Vocal.dup1", "Vocal.dup2"...: numbered from the original's own name.
    auto base = track::name (t);
    if (const auto dot = base.lastIndexOf (".dup"); dot > 0 && base.substring (dot + 4).containsOnly ("0123456789")
                                                    && base.substring (dot + 4).isNotEmpty())
        base = base.substring (0, dot);
    juce::String name;
    for (int n = 1;; ++n)
    {
        name = base + ".dup" + juce::String (n);
        if (! tracks.getChildWithProperty (ids::name, name).isValid())
            break;
    }

    auto copy = t.createCopy();
    copy.setProperty (ids::id, allocateId(), nullptr);
    copy.setProperty (ids::name, name, nullptr);
    copy.setProperty (ids::recordArm, false, nullptr); // one input, one armed track
    // A duplicate is the active playlist only, its playlist named after the new track.
    copy.removeProperty (ids::playlist, nullptr);
    if (auto alternates = track::playlists (copy); alternates.isValid())
        copy.removeChild (alternates, nullptr);
    for (auto c : track::clips (copy))
        c.setProperty (ids::id, allocateId(), nullptr);

    tracks.addChild (copy, index + 1, um);
    return copy;
}

juce::ValueTree Session::addClip (juce::ValueTree t, const juce::File& audioFile, juce::int64 timelineStart,
                                  juce::int64 sourceOffset, juce::int64 length, juce::int64 sourceLength,
                                  const juce::String& name, juce::UndoManager* um)
{
    juce::ValueTree c (ids::CLIP);
    c.setProperty (ids::id, allocateId(), nullptr);
    c.setProperty (ids::name, name, nullptr);
    c.setProperty (ids::file, makeStoredPath (audioFile), nullptr);
    c.setProperty (ids::start, timelineStart, nullptr);
    c.setProperty (ids::offset, sourceOffset, nullptr);
    c.setProperty (ids::length, length, nullptr);
    c.setProperty (ids::sourceLength, sourceLength, nullptr);
    c.setProperty (ids::gainDb, 0.0, nullptr);
    c.setProperty (ids::fadeIn, 0, nullptr);
    c.setProperty (ids::fadeOut, 0, nullptr);

    track::clips (t).appendChild (c, um);
    return c;
}

juce::ValueTree Session::splitClip (juce::ValueTree c, juce::int64 position, juce::UndoManager* um)
{
    const auto s = clip::start (c);
    const auto e = clip::end (c);
    if (position <= s || position >= e)
        return {};

    auto right = c.createCopy();
    right.setProperty (ids::id, allocateId(), nullptr);
    right.setProperty (ids::start, position, nullptr);
    right.setProperty (ids::offset, clip::offset (c) + (position - s), nullptr);
    right.setProperty (ids::length, e - position, nullptr);
    right.setProperty (ids::fadeIn, 0, nullptr);
    // A crossfade stays with the side that keeps its edge.
    right.removeProperty (ids::fadeInShape, nullptr);
    right.removeProperty (ids::crossfadeIn, nullptr);
    right.removeProperty (ids::extendedIn, nullptr);

    c.setProperty (ids::length, position - s, um);
    c.setProperty (ids::fadeOut, 0, um);
    c.removeProperty (ids::fadeOutShape, um);
    c.removeProperty (ids::extendedOut, um);

    // Insert directly above the original so the z-order of other clips is unchanged.
    auto parent = c.getParent();
    parent.addChild (right, parent.indexOf (c) + 1, um);
    return right;
}

juce::String Session::makeStoredPath (const juce::File& f) const
{
    if (f.isAChildOf (getFolder()))
        return f.getRelativePathFrom (getFolder()).replaceCharacter ('\\', '/');
    return f.getFullPathName();
}

juce::File Session::resolveStoredPath (const juce::String& stored) const
{
    if (juce::File::isAbsolutePath (stored))
        return juce::File (stored);
    return getFolder().getChildFile (stored);
}

juce::File Session::createUniqueAudioFile (const juce::String& baseName) const
{
    const auto extension = getFileFormat() == "aiff" ? ".aif" : ".wav";
    const auto legal = juce::File::createLegalFileName (baseName).replaceCharacter (' ', '_');
    return getAudioFolder().getNonexistentChildFile (legal.isNotEmpty() ? legal : juce::String ("Audio"), extension, false);
}

} // namespace meridian
