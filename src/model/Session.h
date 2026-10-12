#pragma once

#include "SessionIds.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h> // juce::Colour for track colours (no GUI dependency)

namespace meridian
{

/** A session on disk:  <folder>/<name>.meridian  plus  <folder>/Audio Files/.

    Owns the document tree and its undo history. Message thread only. */
class Session : private juce::ValueTree::Listener
{
public:
    static constexpr const char* fileExtension = ".meridian";
    static constexpr const char* audioFolderName = "Audio Files";
    static constexpr int formatVersion = 1;
    static constexpr int maxInserts = 10;
    static constexpr int maxSends = 5;   // A..E

    struct Settings
    {
        juce::String name { "Untitled" };
        double sampleRate = 48000.0;
        int bitDepth = 24;                 ///< 16, 24 or 32 (32 = float)
        juce::String fileFormat { "wav" }; ///< "wav" or "aiff"
    };

    /** Creates <parentFolder>/<name>/ with an empty session and saves it. */
    static std::unique_ptr<Session> createNew (const juce::File& parentFolder, const Settings&, juce::String& error);

    /** Parses the file and repairs anything outside the schema's invariants
        (see SessionRepair.h); getLoadRepairs() lists what was fixed. */
    static std::unique_ptr<Session> load (const juce::File& sessionFile, juce::String& error);

    const juce::StringArray& getLoadRepairs() const noexcept { return loadRepairs; }

    ~Session() override;

    bool save (juce::String& error);

    juce::ValueTree& getState() noexcept               { return state; }
    const juce::ValueTree& getState() const noexcept   { return state; }
    juce::UndoManager& getUndoManager() noexcept       { return undoManager; }

    const juce::File& getSessionFile() const noexcept  { return sessionFile; }
    juce::File getFolder() const                       { return sessionFile.getParentDirectory(); }
    juce::File getAudioFolder() const                  { return getFolder().getChildFile (audioFolderName); }

    juce::String getName() const;
    double getSampleRate() const;
    int getBitDepth() const;
    juce::String getFileFormat() const;

    bool hasUnsavedChanges() const noexcept            { return dirty; }

    /** Session-unique id for new tracks and clips. */
    int allocateId();

    //==============================================================================
    juce::ValueTree getTracksNode() const;
    int getNumTracks() const;
    juce::ValueTree getTrack (int index) const;           ///< 0-based; invalid tree if out of range
    juce::ValueTree getTrackById (int trackId) const;
    int indexOfTrack (const juce::ValueTree& track) const;

    /** drums = an instrument track whose engine is the built-in drum machine. */
    enum class TrackKind { audio, aux, instrument, drums };

    /** Aux and instrument tracks are always stereo and have no audio input.
        Instrument tracks get a SYNTH node with the default sound. */
    juce::ValueTree addTrack (const juce::String& name, int numChannels, juce::UndoManager*, TrackKind = TrackKind::audio);

    /** An empty MIDI clip on an instrument track (timeline samples). */
    juce::ValueTree addMidiClip (juce::ValueTree track, juce::int64 timelineStart, juce::int64 length,
                                 const juce::String& name, juce::UndoManager*);

    /** Session tempo in BPM (one tempo for the whole session for now). */
    double getTempo() const;
    void removeTrack (const juce::ValueTree& track, juce::UndoManager*);

    juce::ValueTree addClip (juce::ValueTree track, const juce::File& audioFile, juce::int64 timelineStart,
                             juce::int64 sourceOffset, juce::int64 length, juce::int64 sourceLength,
                             const juce::String& name, juce::UndoManager*);

    /** Splits a clip in two at a timeline position strictly inside it.
        Returns the new right-hand clip, or an invalid tree if nothing was split. */
    juce::ValueTree splitClip (juce::ValueTree clip, juce::int64 timelinePosition, juce::UndoManager*);

    /** Clip file paths are stored relative to the session folder when the file
        lives inside it, so a session folder can be moved or copied as a unit. */
    juce::String makeStoredPath (const juce::File&) const;
    juce::File resolveStoredPath (const juce::String&) const;

    /** A fresh, non-existing file name in the Audio Files folder. */
    juce::File createUniqueAudioFile (const juce::String& baseName) const;

private:
    Session (juce::File file, juce::ValueTree tree);

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { dirty = true; }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override             { dirty = true; }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override       { dirty = true; }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override               { dirty = true; }

    static juce::ValueTree createEmptyState (const Settings&);
    static void ensureStructure (juce::ValueTree& state);

    juce::File sessionFile;
    juce::ValueTree state;
    juce::UndoManager undoManager;
    juce::StringArray loadRepairs;
    bool dirty = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Session)
};

/** Typed accessors for TRACK and CLIP nodes, so call sites never repeat
    property names or default values. */
namespace track
{
    inline int id (const juce::ValueTree& t)                  { return t[ids::id]; }
    inline juce::String name (const juce::ValueTree& t)       { return t[ids::name].toString(); }
    inline bool isAux (const juce::ValueTree& t)              { return t.getProperty (ids::kind, "audio").toString() == "aux"; }
    inline bool isInstrument (const juce::ValueTree& t)       { return t.getProperty (ids::kind, "audio").toString() == "instrument"; }
    inline bool isAudio (const juce::ValueTree& t)            { return ! isAux (t) && ! isInstrument (t); }
    inline juce::ValueTree synth (const juce::ValueTree& t)   { return t.getChildWithName (ids::SYNTH); }
    inline bool isDrumKit (const juce::ValueTree& t)          { return isInstrument (t) && synth (t).getProperty (ids::engine, "synth").toString() == "drums"; }
    inline int outputBus (const juce::ValueTree& t)           { return t.getProperty (ids::outputBus, 0); } ///< aux track id, 0 = master
    inline juce::ValueTree sends (const juce::ValueTree& t)   { return t.getChildWithName (ids::SENDS); }
    inline int channels (const juce::ValueTree& t)            { return juce::jlimit (1, 2, (int) t.getProperty (ids::channels, 1)); }
    inline juce::Colour colour (const juce::ValueTree& t)     { return juce::Colour::fromString (t.getProperty (ids::colour, "ff5a8dd6").toString()); }
    inline double volumeDb (const juce::ValueTree& t)         { return t.getProperty (ids::volumeDb, 0.0); }
    inline double pan (const juce::ValueTree& t)              { return t.getProperty (ids::pan, 0.0); }
    inline bool mute (const juce::ValueTree& t)               { return t.getProperty (ids::mute, false); }
    inline bool solo (const juce::ValueTree& t)               { return t.getProperty (ids::solo, false); }
    inline bool recordArm (const juce::ValueTree& t)          { return t.getProperty (ids::recordArm, false); }
    inline int inputChannel (const juce::ValueTree& t)        { return t.getProperty (ids::inputChannel, 0); } ///< first device input, -1 = none
    inline bool automationRead (const juce::ValueTree& t)     { return t.getProperty (ids::automationMode, "read").toString() == "read"; }
    inline juce::ValueTree clips (const juce::ValueTree& t)   { return t.getChildWithName (ids::CLIPS); }
    inline juce::ValueTree inserts (const juce::ValueTree& t) { return t.getChildWithName (ids::INSERTS); }
    inline juce::ValueTree automation (const juce::ValueTree& t) { return t.getChildWithName (ids::AUTOMATION); }
} // namespace track

namespace send
{
    inline int slot (const juce::ValueTree& s)                { return s[ids::slot]; }
    inline int target (const juce::ValueTree& s)              { return s[ids::target]; }  ///< aux track id
    inline double levelDb (const juce::ValueTree& s)          { return s.getProperty (ids::levelDb, 0.0); }
    inline double pan (const juce::ValueTree& s)              { return s.getProperty (ids::pan, 0.0); }
    inline bool preFader (const juce::ValueTree& s)           { return s.getProperty (ids::preFader, false); }
    inline bool mute (const juce::ValueTree& s)               { return s.getProperty (ids::mute, false); }
    inline juce::ValueTree owningTrack (const juce::ValueTree& s) { return s.getParent().getParent(); }
} // namespace send

namespace clip
{
    inline int id (const juce::ValueTree& c)                  { return c[ids::id]; }
    inline juce::String name (const juce::ValueTree& c)       { return c[ids::name].toString(); }
    inline juce::int64 start (const juce::ValueTree& c)       { return (juce::int64) c[ids::start]; }
    inline juce::int64 length (const juce::ValueTree& c)      { return (juce::int64) c[ids::length]; }
    inline juce::int64 end (const juce::ValueTree& c)         { return start (c) + length (c); }
    inline juce::int64 offset (const juce::ValueTree& c)      { return (juce::int64) c[ids::offset]; }
    inline juce::int64 sourceLength (const juce::ValueTree& c){ return (juce::int64) c[ids::sourceLength]; }
    inline double gainDb (const juce::ValueTree& c)           { return c.getProperty (ids::gainDb, 0.0); }
    inline juce::int64 fadeIn (const juce::ValueTree& c)      { return (juce::int64) c.getProperty (ids::fadeIn, 0); }
    inline juce::int64 fadeOut (const juce::ValueTree& c)     { return (juce::int64) c.getProperty (ids::fadeOut, 0); }
    inline juce::ValueTree owningTrack (const juce::ValueTree& c) { return c.getParent().getParent(); }
    inline bool isMidi (const juce::ValueTree& c)             { return c.getProperty (ids::kind, "audio").toString() == "midi"; }
    inline juce::ValueTree notes (const juce::ValueTree& c)   { return c.getChildWithName (ids::NOTES); }
    /** Timeline sample where the clip's content (and its note ticks) begin. */
    inline juce::int64 contentOrigin (const juce::ValueTree& c) { return start (c) - offset (c); }
} // namespace clip

} // namespace meridian
