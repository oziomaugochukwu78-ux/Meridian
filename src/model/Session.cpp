#include "Session.h"

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

    if (! session->save (error))
        return nullptr;
    return session;
}

std::unique_ptr<Session> Session::load (const juce::File& file, juce::String& error)
{
    auto xml = juce::parseXML (file);
    if (xml == nullptr || ! xml->hasTagName (ids::SESSION.toString()))
    {
        error = file.getFileName() + " is not a valid session file.";
        return nullptr;
    }

    auto tree = juce::ValueTree::fromXml (*xml);
    if ((int) tree.getProperty (ids::formatVersion, 0) > formatVersion)
    {
        error = file.getFileName() + " was saved by a newer version of this application.";
        return nullptr;
    }

    return std::unique_ptr<Session> (new Session (file, std::move (tree)));
}

bool Session::save (juce::String& error)
{
    auto xml = state.createXml();
    if (xml == nullptr)
    {
        error = "Could not serialise the session.";
        return false;
    }

    // Write to a temporary file first and swap it in, so a crash or a full
    // disk mid-save can never leave a truncated session behind.
    juce::TemporaryFile temp (sessionFile);
    if (! xml->writeTo (temp.getFile()) || ! temp.overwriteTargetFileWithTemporary())
    {
        error = "Could not write " + sessionFile.getFullPathName() + ".";
        return false;
    }

    dirty = false;
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
    const auto colour = aux ? juce::Colour (0xff8f9aa6)  // aux tracks: neutral grey-blue
                            : juce::Colour (trackPalette[(size_t) getNumTracks() % std::size (trackPalette)]);

    juce::ValueTree t (ids::TRACK);
    t.setProperty (ids::id, newId, nullptr);
    t.setProperty (ids::name, name, nullptr);
    t.setProperty (ids::kind, aux ? "aux" : "audio", nullptr);
    t.setProperty (ids::channels, aux ? 2 : juce::jlimit (1, 2, numChannels), nullptr);
    t.setProperty (ids::colour, colour.toString(), nullptr);
    t.setProperty (ids::volumeDb, 0.0, nullptr);
    t.setProperty (ids::pan, 0.0, nullptr);
    t.setProperty (ids::mute, false, nullptr);
    t.setProperty (ids::solo, false, nullptr);
    t.setProperty (ids::recordArm, false, nullptr);
    t.setProperty (ids::inputChannel, aux ? -1 : 0, nullptr);
    t.setProperty (ids::automationMode, "read", nullptr);
    t.setProperty (ids::outputBus, 0, nullptr);
    t.appendChild (juce::ValueTree (ids::CLIPS), nullptr);
    t.appendChild (juce::ValueTree (ids::INSERTS), nullptr);
    t.appendChild (juce::ValueTree (ids::SENDS), nullptr);
    t.appendChild (juce::ValueTree (ids::AUTOMATION), nullptr);

    getTracksNode().appendChild (t, um);
    return t;
}

void Session::removeTrack (const juce::ValueTree& t, juce::UndoManager* um)
{
    getTracksNode().removeChild (t, um);
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

    c.setProperty (ids::length, position - s, um);
    c.setProperty (ids::fadeOut, 0, um);

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
