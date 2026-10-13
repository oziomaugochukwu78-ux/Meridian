#include "SessionRepair.h"

#include "Automation.h"
#include "MidiModel.h"
#include "Playlists.h"
#include "Routing.h"

#include <dsp/FaderTaper.h>

#include <cmath>
#include <set>
#include <tuple>

namespace meridian
{
namespace
{
/** A SUSTAIN list as midimodel::writeSustain leaves it: PEDALs only, each at least a
    tick long from zero on, sorted, with gaps between them (touching ones are merged). */
bool sustainIsValid (const juce::ValueTree& list)
{
    juce::int64 previousEnd = -1;
    for (const auto& p : list)
    {
        if (! p.hasType (ids::PEDAL) || ! p.hasProperty (ids::start) || ! p.hasProperty (ids::length))
            return false;
        const auto start = (juce::int64) p[ids::start], length = (juce::int64) p[ids::length];
        if (start < 0 || length < 1 || start <= previousEnd)
            return false;
        previousEnd = start + length;
    }
    return true;
}

class Repairer
{
public:
    explicit Repairer (Session& s) : session (s) {}

    juce::StringArray run()
    {
        auto state = session.getState();
        repairSessionProperties (state);
        removeChildrenNotOfType (session.getTracksNode(), ids::TRACK, "session");

        collectUsedIds();
        for (auto t : session.getTracksNode())
            repairTrack (t);

        if ((int) state[ids::nextId] <= maxId)
            set (state, ids::nextId, maxId + 1, "session", "next id");

        repairRouting();
        return notes;
    }

private:
    //==========================================================================
    void note (const juce::String& where, const juce::String& what)
    {
        notes.add (where + ": " + what);
    }

    void set (juce::ValueTree node, const juce::Identifier& property, const juce::var& value,
              const juce::String& where, const juce::String& what)
    {
        note (where, what + " " + (node.hasProperty (property) ? node[property].toString() : juce::String ("(missing)"))
                         + " -> " + value.toString());
        node.setProperty (property, value, nullptr);
    }

    /** Clamps an optional numeric property into [lo, hi]; non-finite values become `fallback`. */
    void clampNumber (juce::ValueTree node, const juce::Identifier& property, double lo, double hi, double fallback,
                      const juce::String& where)
    {
        if (! node.hasProperty (property))
            return;
        const double value = node[property];
        const double fixed = std::isfinite (value) ? juce::jlimit (lo, hi, value) : fallback;
        if (! juce::exactlyEqual (fixed, value)) // also true for NaN, which never equals itself
            set (node, property, fixed, where, property.toString());
    }

    void removeChildrenNotOfType (juce::ValueTree parent, const juce::Identifier& type, const juce::String& where)
    {
        for (int i = parent.getNumChildren(); --i >= 0;)
            if (! parent.getChild (i).hasType (type))
            {
                note (where, "removed an unknown <" + parent.getChild (i).getType().toString() + "> element");
                parent.removeChild (i, nullptr);
            }
    }

    static juce::String describe (const juce::ValueTree& t)
    {
        return (t.hasType (ids::CLIP) ? "clip \"" : "track \"") + t[ids::name].toString() + "\"";
    }

    //==========================================================================
    void repairSessionProperties (juce::ValueTree s)
    {
        const double rate = s.getProperty (ids::sampleRate, 48000.0);
        if (! (std::isfinite (rate) && rate >= 8000.0 && rate <= 384000.0))
            set (s, ids::sampleRate, 48000.0, "session", "sample rate");

        const int bits = s.getProperty (ids::bitDepth, 24);
        if (bits != 16 && bits != 24 && bits != 32)
            set (s, ids::bitDepth, 24, "session", "bit depth");

        const auto format = s.getProperty (ids::fileFormat, "wav").toString();
        if (format != "wav" && format != "aiff")
            set (s, ids::fileFormat, "wav", "session", "file format");

        clampNumber (s, ids::masterVolumeDb, dsp::FaderTaper::minDb, dsp::FaderTaper::maxDb, 0.0, "session");
        clampNumber (s, ids::consoleDrive, 0.0, 24.0, 6.0, "session");
        clampNumber (s, ids::consoleCharacter, 0.0, 1.0, 0.35, "session");
        if (s.hasProperty (ids::consoleCurve) && (int) s[ids::consoleCurve] != 0 && (int) s[ids::consoleCurve] != 1)
            set (s, ids::consoleCurve, 0, "session", "console curve");
        clampNumber (s, ids::tempo, 20.0, 999.0, 120.0, "session");
        clampNumber (s, ids::clickLevelDb, -60.0, 0.0, -6.0, "session");
        clampNumber (s, ids::countInBars, 0.0, 8.0, 0.0, "session");
        clampNumber (s, ids::loopStart, 0.0, 1.0e15, 0.0, "session");
        clampNumber (s, ids::loopEnd, 0.0, 1.0e15, 0.0, "session");
        if ((bool) s.getProperty (ids::loopEnabled, false)
            && (juce::int64) s.getProperty (ids::loopEnd, 0) <= (juce::int64) s.getProperty (ids::loopStart, 0))
            set (s, ids::loopEnabled, false, "session", "loop playback with an empty range");
        clampNumber (s, ids::punchIn, 0.0, 1.0e15, 0.0, "session");
        clampNumber (s, ids::punchOut, 0.0, 1.0e15, 0.0, "session");
        if ((bool) s.getProperty (ids::punchEnabled, false)
            && (juce::int64) s.getProperty (ids::punchOut, 0) <= (juce::int64) s.getProperty (ids::punchIn, 0))
            set (s, ids::punchEnabled, false, "session", "punch recording with an empty range");

        // Tempo and meter changes: positions at or after the start, values in range.
        if (auto tempos = s.getChildWithName (ids::TEMPO_MAP); tempos.isValid())
        {
            removeChildrenNotOfType (tempos, ids::TEMPO, "tempo map");
            for (int i = tempos.getNumChildren(); --i >= 0;)
            {
                auto t = tempos.getChild (i);
                const double tick = t[ids::tick], bpm = t[ids::bpm];
                if (! (std::isfinite (tick) && tick > 0.0 && tick < 1.0e15 && std::isfinite (bpm)))
                {
                    note ("tempo map", "removed a tempo change with an invalid position or tempo");
                    tempos.removeChild (i, nullptr);
                    continue;
                }
                clampNumber (t, ids::bpm, 20.0, 999.0, 120.0, "tempo map");
            }
        }
        if (auto meters = s.getChildWithName (ids::METER_MAP); meters.isValid())
        {
            removeChildrenNotOfType (meters, ids::METER, "meter map");
            for (int i = meters.getNumChildren(); --i >= 0;)
            {
                auto m = meters.getChild (i);
                const double bar = m[ids::bar];
                const int numerator = m[ids::numerator], denominator = m[ids::denominator];
                const bool powerOfTwo = denominator >= 1 && denominator <= 32 && (denominator & (denominator - 1)) == 0;
                if (! (std::isfinite (bar) && bar >= 0.0 && bar < 1.0e12) || numerator < 1 || numerator > 32 || ! powerOfTwo)
                {
                    note ("meter map", "removed an invalid time signature");
                    meters.removeChild (i, nullptr);
                }
            }
        }

        auto markers = s.getChildWithName (ids::MARKERS);
        if (! markers.isValid())
            return;
        removeChildrenNotOfType (markers, ids::MARKER, "markers");
        std::set<int> numbers;
        int highest = 0;
        for (const auto& m : markers)
            highest = juce::jmax (highest, (int) m[ids::number]);
        for (int i = 0; i < markers.getNumChildren(); ++i)
        {
            auto m = markers.getChild (i);
            const double start = m[ids::start];
            if (! (std::isfinite (start) && start >= 0.0 && start <= 1.0e15))
            {
                note ("markers", "removed marker \"" + m[ids::name].toString() + "\" with an invalid position");
                markers.removeChild (i--, nullptr);
                continue;
            }
            const int number = m[ids::number];
            if (number <= 0 || ! numbers.insert (number).second)
            {
                set (m, ids::number, ++highest, "marker \"" + m[ids::name].toString() + "\"", "invalid or duplicate number");
                numbers.insert (highest);
            }
        }
    }

    //==========================================================================
    // Ids: tracks and clips share one id space, allocated from nextId.

    void collectUsedIds()
    {
        for (const auto& t : session.getTracksNode())
        {
            maxId = juce::jmax (maxId, (int) t[ids::id]);
            for (const auto& list : track::allClipLists (t))
                for (const auto& c : list)
                    maxId = juce::jmax (maxId, (int) c[ids::id]);
        }
    }

    void ensureUniqueId (juce::ValueTree node)
    {
        const int current = node[ids::id];
        if (current > 0 && usedIds.insert (current).second)
            return;
        set (node, ids::id, ++maxId, describe (node), current > 0 ? "duplicate id" : "invalid id");
        usedIds.insert (maxId);
    }

    //==========================================================================
    void repairTrack (juce::ValueTree t)
    {
        ensureUniqueId (t);
        const auto where = describe (t);

        const auto kind = t.getProperty (ids::kind, "audio").toString();
        if (kind != "audio" && kind != "aux" && kind != "instrument")
            set (t, ids::kind, "audio", where, "kind");
        const bool aux = track::isAux (t), instrument = track::isInstrument (t);

        const int channels = t.getProperty (ids::channels, 1);
        const int wantedChannels = aux || instrument ? 2 : juce::jlimit (1, 2, channels);
        if (channels != wantedChannels)
            set (t, ids::channels, wantedChannels, where, "channels");

        clampNumber (t, ids::volumeDb, dsp::FaderTaper::minDb, dsp::FaderTaper::maxDb, 0.0, where);
        clampNumber (t, ids::pan, -1.0, 1.0, 0.0, where);
        if ((aux || instrument) && track::inputChannel (t) != -1)
            set (t, ids::inputChannel, -1, where, "input (aux and instrument tracks have no audio input)");
        else if (! aux && ! instrument && (track::inputChannel (t) < -1 || track::inputChannel (t) > 255))
            set (t, ids::inputChannel, 0, where, "input channel");
        if (aux && track::recordArm (t)) // (instrument tracks arm to record MIDI)
            set (t, ids::recordArm, false, where, "record-arm (an aux track cannot record)");
        if (t.hasProperty (ids::automationMode)
            && ! juce::StringArray { "off", "read", "touch", "latch", "write" }.contains (track::automationMode (t)))
            set (t, ids::automationMode, "read", where, "automation mode");

        repairSynth (t, where, instrument);

        for (const auto* type : { &ids::CLIPS, &ids::INSERTS, &ids::SENDS, &ids::AUTOMATION })
        {
            // Exactly one of each container.
            juce::ValueTree first;
            for (int i = 0; i < t.getNumChildren(); ++i)
            {
                if (! t.getChild (i).hasType (*type))
                    continue;
                if (! first.isValid())
                {
                    first = t.getChild (i);
                    continue;
                }
                note (where, "removed a duplicate <" + type->toString() + "> list");
                t.removeChild (i--, nullptr);
            }
        }

        repairClips (t, track::clips (t), where);
        repairPlaylists (t, where);
        repairSlots (track::inserts (t), ids::INSERT, Session::maxInserts, where, "insert");
        repairSlots (track::sends (t), ids::SEND, Session::maxSends, where, "send");
        for (auto s : track::sends (t))
        {
            clampNumber (s, ids::levelDb, dsp::FaderTaper::minDb, dsp::FaderTaper::maxDb, 0.0, where + " send");
            clampNumber (s, ids::pan, -1.0, 1.0, 0.0, where + " send");
        }
        repairAutomation (track::automation (t), where);
    }

    /** Instrument tracks: exactly one SYNTH with valid settings. Other tracks: none. */
    void repairSynth (juce::ValueTree t, const juce::String& where, bool instrument)
    {
        juce::ValueTree first;
        for (int i = 0; i < t.getNumChildren(); ++i)
        {
            if (! t.getChild (i).hasType (ids::SYNTH))
                continue;
            if (instrument && ! first.isValid())
            {
                first = t.getChild (i);
                continue;
            }
            note (where, instrument ? "removed a duplicate <SYNTH>" : "removed a <SYNTH> from a non-instrument track");
            t.removeChild (i--, nullptr);
        }
        if (! instrument)
            return;

        if (! first.isValid())
        {
            first = juce::ValueTree (ids::SYNTH);
            t.appendChild (first, nullptr);
            note (where, "added the missing instrument settings (<SYNTH>)");
        }
        const auto waveform = first.getProperty (ids::waveform, "saw").toString();
        if (waveform != "saw" && waveform != "square" && waveform != "sine")
            set (first, ids::waveform, "saw", where + " synth", "waveform");
        const auto engine = first.getProperty (ids::engine, "synth").toString();
        if (engine != "synth" && engine != "drums")
            set (first, ids::engine, "synth", where + " synth", "engine");
        const auto synthWhere = where + " synth";
        clampNumber (first, ids::tune, -12.0, 12.0, 0.0, synthWhere);
        clampNumber (first, ids::decayScale, 0.25, 4.0, 1.0, synthWhere);
        clampNumber (first, ids::tone, 0.0, 1.0, 0.5, synthWhere);
        clampNumber (first, ids::detune, 0.0, 50.0, 7.0, synthWhere);
        clampNumber (first, ids::attack, 0.0005, 10.0, 0.005, synthWhere);
        clampNumber (first, ids::decay, 0.001, 10.0, 0.25, synthWhere);
        clampNumber (first, ids::sustain, 0.0, 1.0, 0.7, synthWhere);
        clampNumber (first, ids::release, 0.001, 20.0, 0.3, synthWhere);
        clampNumber (first, ids::cutoff, 20.0, 20000.0, 2500.0, synthWhere);
        clampNumber (first, ids::resonance, 0.0, 1.0, 0.2, synthWhere);
        clampNumber (first, ids::filterEnv, -8.0, 8.0, 2.0, synthWhere);
        clampNumber (first, ids::gainDb, -60.0, 12.0, -12.0, synthWhere);
    }

    void repairNotes (juce::ValueTree c, const juce::String& where)
    {
        // Exactly one NOTES list, holding only valid NOTEs; at most one SUSTAIN list.
        juce::ValueTree list, sustain;
        for (int i = 0; i < c.getNumChildren(); ++i)
        {
            if (c.getChild (i).hasType (ids::SUSTAIN))
            {
                if (sustain.isValid())
                {
                    note (where, "removed a duplicate <SUSTAIN> list");
                    c.removeChild (i--, nullptr);
                }
                else
                {
                    sustain = c.getChild (i);
                }
            }
            else if (! c.getChild (i).hasType (ids::NOTES))
            {
                note (where, "removed an unknown <" + c.getChild (i).getType().toString() + "> element");
                c.removeChild (i--, nullptr);
            }
            else if (! list.isValid())
            {
                list = c.getChild (i);
            }
            else
            {
                note (where, "removed a duplicate <NOTES> list");
                c.removeChild (i--, nullptr);
            }
        }
        if (sustain.isValid())
        {
            // Sorted, non-overlapping ranges of at least a tick: rewrite the list if it is not.
            removeChildrenNotOfType (sustain, ids::PEDAL, where);
            if (! sustainIsValid (sustain))
            {
                note (where, "sorted and merged the sustain-pedal ranges");
                midimodel::writeSustain (c, midimodel::readSustain (c), nullptr);
            }
        }

        if (! list.isValid())
        {
            note (where, "added the missing <NOTES> list");
            c.appendChild (juce::ValueTree (ids::NOTES), nullptr);
            return;
        }

        removeChildrenNotOfType (list, ids::NOTE, where);
        for (auto n : list)
        {
            const auto noteWhere = where + " note";
            clampNumber (n, ids::pitch, 0.0, 127.0, 60.0, noteWhere);
            if (! n.hasProperty (ids::pitch)) set (n, ids::pitch, 60, noteWhere, "pitch");
            if (! juce::exactlyEqual ((double) n[ids::pitch], std::floor ((double) n[ids::pitch])))
                set (n, ids::pitch, (int) n[ids::pitch], noteWhere, "pitch (whole numbers only)");
            clampNumber (n, ids::velocity, 1.0 / 127.0, 1.0, 0.8, noteWhere);
            if ((juce::int64) n[ids::start] < 0 || ! n.hasProperty (ids::start)) set (n, ids::start, 0, noteWhere, "start");
            if ((juce::int64) n[ids::length] < 1) set (n, ids::length, 1, noteWhere, "length");
        }
    }

    /** At most one PLAYLISTS list of PLAYLISTs, each with a unique name (not the active
        playlist's) and exactly one CLIPS list, whose clips are repaired like the active ones. */
    void repairPlaylists (juce::ValueTree t, const juce::String& where)
    {
        juce::ValueTree list;
        for (int i = 0; i < t.getNumChildren(); ++i)
        {
            if (! t.getChild (i).hasType (ids::PLAYLISTS))
                continue;
            if (! list.isValid())
            {
                list = t.getChild (i);
                continue;
            }
            note (where, "removed a duplicate <PLAYLISTS> list");
            t.removeChild (i--, nullptr);
        }
        if (! list.isValid())
            return;

        removeChildrenNotOfType (list, ids::PLAYLIST, where);
        juce::StringArray taken { playlists::activeName (t) };
        for (auto p : list)
        {
            auto name = p[ids::name].toString().trim();
            if (name.isEmpty() || taken.contains (name))
            {
                const auto base = name.isEmpty() ? track::name (t) + " playlist" : name;
                auto fixed = base;
                for (int n = 2; taken.contains (fixed); ++n)
                    fixed = base + " (" + juce::String (n) + ")";
                set (p, ids::name, fixed, where, "playlist name (empty or taken)");
                name = fixed;
            }
            taken.add (name);

            juce::ValueTree clips;
            for (int i = 0; i < p.getNumChildren(); ++i)
            {
                if (p.getChild (i).hasType (ids::CLIPS) && ! clips.isValid())
                {
                    clips = p.getChild (i);
                    continue;
                }
                note (where + ", playlist " + name, "removed an extra <" + p.getChild (i).getType().toString() + ">");
                p.removeChild (i--, nullptr);
            }
            if (! clips.isValid())
            {
                note (where + ", playlist " + name, "added the missing <CLIPS> list");
                p.appendChild (juce::ValueTree (ids::CLIPS), nullptr);
                continue;
            }
            repairClips (t, clips, where + ", playlist " + name);
        }
    }

    void repairClips (juce::ValueTree t, juce::ValueTree clips, const juce::String& trackName)
    {
        removeChildrenNotOfType (clips, ids::CLIP, trackName);
        const bool instrument = track::isInstrument (t);

        for (int i = clips.getNumChildren(); --i >= 0;)
        {
            auto c = clips.getChild (i);
            const auto where = trackName + ", " + describe (c);

            const auto clipKind = c.getProperty (ids::kind, "audio").toString();
            if (clipKind != "audio" && clipKind != "midi")
            {
                note (where, "unknown clip kind \"" + clipKind + "\"; treated as audio");
                c.removeProperty (ids::kind, nullptr);
            }

            // MIDI clips belong on instrument tracks, audio clips everywhere else.
            if (clip::isMidi (c) != instrument)
            {
                note (where, clip::isMidi (c) ? "removed: a MIDI clip on a track that is not an instrument track"
                                               : "removed: an audio clip on an instrument track");
                clips.removeChild (i, nullptr);
                continue;
            }

            if (clip::isMidi (c))
                repairNotes (c, where);
            else if (c[ids::file].toString().isEmpty())
            {
                note (where, "removed: no audio file");
                clips.removeChild (i, nullptr);
                continue;
            }

            if (clip::start (c) < 0)  set (c, ids::start, 0, where, "start");
            if (clip::offset (c) < 0) set (c, ids::offset, 0, where, "offset");
            if (clip::sourceLength (c) < 0) set (c, ids::sourceLength, 0, where, "source length");

            // A clip may not read past the end of its source (0 = unknown length).
            if (const auto source = clip::sourceLength (c); source > 0 && clip::offset (c) + clip::length (c) > source)
                set (c, ids::length, juce::jmax ((juce::int64) 0, source - clip::offset (c)), where, "length (past the end of its file)");

            if (clip::length (c) <= 0)
            {
                note (where, "removed: no length");
                clips.removeChild (i, nullptr);
                continue;
            }

            ensureUniqueId (c);
            clampNumber (c, ids::gainDb, dsp::FaderTaper::minDb, 24.0, 0.0, where);

            auto fadeIn = juce::jmax ((juce::int64) 0, clip::fadeIn (c));
            auto fadeOut = juce::jmax ((juce::int64) 0, clip::fadeOut (c));
            if (const auto length = clip::length (c); fadeIn + fadeOut > length)
            {
                // Scale both down in proportion, so they meet instead of overlapping.
                const double scale = (double) length / (double) (fadeIn + fadeOut);
                fadeIn = (juce::int64) std::floor ((double) fadeIn * scale);
                fadeOut = juce::jmin (length - fadeIn, (juce::int64) std::floor ((double) fadeOut * scale));
            }
            if (fadeIn != clip::fadeIn (c))   set (c, ids::fadeIn, fadeIn, where, "fade-in");
            if (fadeOut != clip::fadeOut (c)) set (c, ids::fadeOut, fadeOut, where, "fade-out");
        }
    }

    /** INSERT and SEND lists: valid, unique slot numbers only. */
    void repairSlots (juce::ValueTree list, const juce::Identifier& type, int numSlots,
                      const juce::String& where, const juce::String& what)
    {
        removeChildrenNotOfType (list, type, where);
        std::set<int> used;
        for (int i = 0; i < list.getNumChildren(); ++i)
        {
            const int slot = list.getChild (i)[ids::slot];
            if (juce::isPositiveAndBelow (slot, numSlots) && used.insert (slot).second)
                continue;
            note (where, "removed " + what + " with " + (used.count (slot) > 0 ? "duplicate" : "invalid") + " slot " + juce::String (slot));
            list.removeChild (i--, nullptr);
        }
    }

    void repairAutomation (juce::ValueTree automation, const juce::String& where)
    {
        removeChildrenNotOfType (automation, ids::LANE, where);
        juce::StringArray seen;
        for (int l = 0; l < automation.getNumChildren(); ++l)
        {
            auto lane = automation.getChild (l);
            const auto parameter = lane[ids::parameter].toString();
            const auto range = automation::rangeOf (parameter);
            if (! range.has_value() || seen.contains (parameter))
            {
                note (where, range.has_value() ? "removed a second \"" + parameter + "\" automation lane"
                                               : "removed automation for an unknown parameter \"" + parameter + "\"");
                automation.removeChild (l--, nullptr);
                continue;
            }
            seen.add (parameter);

            removeChildrenNotOfType (lane, ids::POINT, where + " automation");
            for (int i = lane.getNumChildren(); --i >= 0;)
            {
                auto p = lane.getChild (i);
                const double value = p[ids::value];
                if (! std::isfinite (value) || ! p.hasProperty (ids::time))
                {
                    note (where, "removed an unusable automation point");
                    lane.removeChild (i, nullptr);
                    continue;
                }
                if ((juce::int64) p[ids::time] < 0)
                    set (p, ids::time, 0, where + " automation", "point time");
                clampNumber (p, ids::value, range->min, range->max, range->min, where + " " + parameter + " automation");
                if (range->stepped && ! juce::exactlyEqual ((double) p[ids::value], std::round ((double) p[ids::value])))
                    set (p, ids::value, std::round ((double) p[ids::value]), where + " " + parameter + " automation", "step value");
            }
        }
    }

    //==========================================================================
    // Routing: every output and send must name an existing aux track, and the
    // whole graph must be loop-free. Each route is taken out and only put
    // back if it closes no loop with the routes present at that moment. The
    // result is acyclic: in any loop that survived, the route checked last
    // would have seen all the others present and been refused.

    bool isValidTarget (const juce::ValueTree& source, int targetId) const
    {
        const auto target = session.getTrackById (targetId);
        return target.isValid() && track::isAux (target) && targetId != track::id (source);
    }

    void repairRouting()
    {
        for (auto t : session.getTracksNode())
        {
            const auto where = describe (t);
            const int output = track::outputBus (t);
            if (output != 0)
            {
                t.setProperty (ids::outputBus, 0, nullptr);
                if (isValidTarget (t, output) && ! routing::wouldCreateCycle (session, t, output))
                    t.setProperty (ids::outputBus, output, nullptr);
                else
                    note (where, "output to track id " + juce::String (output) + " was invalid or made a loop; now goes to the master");
            }

            auto sends = track::sends (t);
            for (int i = 0; i < sends.getNumChildren(); ++i)
            {
                auto s = sends.getChild (i);
                const int target = send::target (s);
                sends.removeChild (i, nullptr);
                if (isValidTarget (t, target) && ! routing::wouldCreateCycle (session, t, target))
                {
                    sends.addChild (s, i, nullptr);
                }
                else
                {
                    note (where, "removed send " + juce::String::charToString ((juce::juce_wchar) ('A' + send::slot (s)))
                                     + " to track id " + juce::String (target) + " (missing target or a loop)");
                    --i;
                }
            }
        }
    }

    Session& session;
    juce::StringArray notes;
    std::set<int> usedIds;
    int maxId = 0;
};
} // namespace

juce::StringArray repairSession (Session& session)
{
    return Repairer (session).run();
}

juce::String findSessionProblem (const Session& session)
{
    const auto& s = session.getState();
    auto finiteIn = [] (const juce::var& v, double lo, double hi)
    {
        const double d = v;
        return std::isfinite (d) && d >= lo && d <= hi;
    };

    if (! finiteIn (s.getProperty (ids::sampleRate, 48000.0), 8000.0, 384000.0))
        return "sample rate " + s[ids::sampleRate].toString();
    if (const int bits = s.getProperty (ids::bitDepth, 24); bits != 16 && bits != 24 && bits != 32)
        return "bit depth " + juce::String (bits);
    if (s.hasProperty (ids::masterVolumeDb) && ! finiteIn (s[ids::masterVolumeDb], dsp::FaderTaper::minDb, dsp::FaderTaper::maxDb))
        return "master volume " + s[ids::masterVolumeDb].toString();
    if (s.hasProperty (ids::tempo) && ! finiteIn (s[ids::tempo], 20.0, 999.0))
        return "tempo " + s[ids::tempo].toString();
    if (s.hasProperty (ids::clickLevelDb) && ! finiteIn (s[ids::clickLevelDb], -60.0, 0.0))
        return "click level " + s[ids::clickLevelDb].toString();
    if (s.hasProperty (ids::countInBars) && ! finiteIn (s[ids::countInBars], 0.0, 8.0))
        return "count-in " + s[ids::countInBars].toString();
    for (const auto* p : { &ids::loopStart, &ids::loopEnd, &ids::punchIn, &ids::punchOut })
        if (s.hasProperty (*p) && ! finiteIn (s[*p], 0.0, 1.0e15))
            return p->toString() + " " + s[*p].toString();
    if ((bool) s.getProperty (ids::punchEnabled, false)
        && (juce::int64) s.getProperty (ids::punchOut, 0) <= (juce::int64) s.getProperty (ids::punchIn, 0))
        return "punch recording with an empty range";
    if ((bool) s.getProperty (ids::loopEnabled, false)
        && (juce::int64) s.getProperty (ids::loopEnd, 0) <= (juce::int64) s.getProperty (ids::loopStart, 0))
        return "loop playback with an empty range";
    for (const auto& t : s.getChildWithName (ids::TEMPO_MAP))
        if (! t.hasType (ids::TEMPO) || ! finiteIn (t[ids::tick], 1.0, 1.0e15) || ! finiteIn (t[ids::bpm], 20.0, 999.0))
            return "invalid tempo change";
    for (const auto& m : s.getChildWithName (ids::METER_MAP))
    {
        const int denominator = m[ids::denominator];
        if (! m.hasType (ids::METER) || ! finiteIn (m[ids::bar], 0.0, 1.0e12) || ! finiteIn (m[ids::numerator], 1.0, 32.0)
            || denominator < 1 || denominator > 32 || (denominator & (denominator - 1)) != 0)
            return "invalid time signature";
    }
    {
        std::set<int> markerNumbers;
        for (const auto& m : s.getChildWithName (ids::MARKERS))
        {
            if (! m.hasType (ids::MARKER))
                return "non-marker element <" + m.getType().toString() + "> in the marker list";
            if (! finiteIn (m[ids::start], 0.0, 1.0e15))
                return "marker \"" + m[ids::name].toString() + "\" position " + m[ids::start].toString();
            if ((int) m[ids::number] <= 0 || ! markerNumbers.insert ((int) m[ids::number]).second)
                return "marker \"" + m[ids::name].toString() + "\" has invalid or duplicate number";
        }
    }

    std::set<int> seenIds;
    int maxId = 0;
    auto claimId = [&] (const juce::ValueTree& node) -> juce::String
    {
        const int id = node[ids::id];
        maxId = juce::jmax (maxId, id);
        if (id <= 0 || ! seenIds.insert (id).second)
            return node.getType().toString() + " \"" + node[ids::name].toString() + "\" has invalid or duplicate id " + juce::String (id);
        return {};
    };

    for (const auto& t : session.getTracksNode())
    {
        if (! t.hasType (ids::TRACK))
            return "non-track element <" + t.getType().toString() + "> in the track list";
        if (auto problem = claimId (t); problem.isNotEmpty())
            return problem;

        const auto name = "track \"" + track::name (t) + "\"";
        const auto kind = t.getProperty (ids::kind, "audio").toString();
        if (kind != "audio" && kind != "aux" && kind != "instrument")
            return name + " kind " + kind;
        const bool instrument = track::isInstrument (t);
        const int channels = t.getProperty (ids::channels, 1);
        if (track::isAux (t) || instrument ? channels != 2 : (channels < 1 || channels > 2))
            return name + " channels " + juce::String (channels);
        if ((track::isAux (t) || instrument) && track::inputChannel (t) != -1)
            return name + " has an audio input but cannot record audio";
        if (track::isAux (t) && track::recordArm (t))
            return name + " is record-armed but an aux track cannot record";
        if (t.hasProperty (ids::automationMode)
            && ! juce::StringArray { "off", "read", "touch", "latch", "write" }.contains (track::automationMode (t)))
            return name + " has an unknown automation mode";

        int synths = 0;
        for (const auto& child : t)
            synths += child.hasType (ids::SYNTH) ? 1 : 0;
        if (synths != (instrument ? 1 : 0))
            return name + " has " + juce::String (synths) + " <SYNTH> nodes";
        if (instrument)
        {
            const auto sy = track::synth (t);
            const auto wf = sy.getProperty (ids::waveform, "saw").toString();
            if (wf != "saw" && wf != "square" && wf != "sine")
                return name + " synth waveform " + wf;
            const auto engine = sy.getProperty (ids::engine, "synth").toString();
            if (engine != "synth" && engine != "drums")
                return name + " synth engine " + engine;
            for (const auto& [id, lo, hi] : { std::tuple { &ids::detune, 0.0, 50.0 }, { &ids::attack, 0.0005, 10.0 },
                                              { &ids::decay, 0.001, 10.0 }, { &ids::sustain, 0.0, 1.0 },
                                              { &ids::release, 0.001, 20.0 }, { &ids::cutoff, 20.0, 20000.0 },
                                              { &ids::resonance, 0.0, 1.0 }, { &ids::filterEnv, -8.0, 8.0 },
                                              { &ids::gainDb, -60.0, 12.0 }, { &ids::tune, -12.0, 12.0 },
                                              { &ids::decayScale, 0.25, 4.0 }, { &ids::tone, 0.0, 1.0 } })
                if (sy.hasProperty (*id) && ! finiteIn (sy[*id], lo, hi))
                    return name + " synth " + id->toString() + " " + sy[*id].toString();
        }
        if (t.hasProperty (ids::volumeDb) && ! finiteIn (t[ids::volumeDb], dsp::FaderTaper::minDb, dsp::FaderTaper::maxDb))
            return name + " volume " + t[ids::volumeDb].toString();
        if (t.hasProperty (ids::pan) && ! finiteIn (t[ids::pan], -1.0, 1.0))
            return name + " pan " + t[ids::pan].toString();

        for (const auto* type : { &ids::CLIPS, &ids::INSERTS, &ids::SENDS, &ids::AUTOMATION })
        {
            int count = 0;
            for (const auto& child : t)
                count += child.hasType (*type) ? 1 : 0;
            if (count != 1)
                return name + " has " + juce::String (count) + " <" + type->toString() + "> lists";
        }

        // Alternate playlists: one list, uniquely named, each with exactly one CLIPS.
        int playlistLists = 0;
        for (const auto& child : t)
            playlistLists += child.hasType (ids::PLAYLISTS) ? 1 : 0;
        if (playlistLists > 1)
            return name + " has " + juce::String (playlistLists) + " <PLAYLISTS> lists";
        juce::StringArray playlistNames { playlists::activeName (t) };
        for (const auto& p : track::playlists (t))
        {
            const auto playlistName = p[ids::name].toString();
            if (! p.hasType (ids::PLAYLIST) || playlistName.trim().isEmpty() || playlistNames.contains (playlistName))
                return name + " has an unnamed, duplicate or unknown playlist";
            playlistNames.add (playlistName);
            if (p.getNumChildren() != 1 || ! p.getChild (0).hasType (ids::CLIPS))
                return name + " playlist " + playlistName + " does not hold exactly one <CLIPS> list";
        }

        juce::Array<juce::ValueTree> everyClip; // every playlist's, the active one first
        for (const auto& clipList : track::allClipLists (t))
            for (const auto& c : clipList)
                everyClip.add (c);
        for (const auto& c : everyClip)
        {
            if (! c.hasType (ids::CLIP))
                return name + " has a non-clip element";
            if (auto problem = claimId (c); problem.isNotEmpty())
                return problem;
            const auto clipName = name + " clip \"" + clip::name (c) + "\"";
            const auto clipKind = c.getProperty (ids::kind, "audio").toString();
            if (clipKind != "audio" && clipKind != "midi")  return clipName + " kind " + clipKind;
            if (clip::isMidi (c) != instrument)             return clipName + " is the wrong kind of clip for its track";
            if (clip::isMidi (c))
            {
                int lists = 0, sustainLists = 0;
                for (const auto& child : c)
                {
                    if (child.hasType (ids::SUSTAIN))
                    {
                        ++sustainLists;
                        if (! sustainIsValid (child))
                            return clipName + " has invalid sustain-pedal ranges";
                        continue;
                    }
                    if (! child.hasType (ids::NOTES))
                        return clipName + " has an unknown <" + child.getType().toString() + "> element";
                    ++lists;
                }
                if (lists != 1)
                    return clipName + " has " + juce::String (lists) + " <NOTES> lists";
                if (sustainLists > 1)
                    return clipName + " has " + juce::String (sustainLists) + " <SUSTAIN> lists";
                for (const auto& n : clip::notes (c))
                {
                    if (! n.hasType (ids::NOTE))
                        return clipName + " has a non-note element";
                    if (! finiteIn (n[ids::pitch], 0.0, 127.0) || ! juce::exactlyEqual ((double) n[ids::pitch], std::floor ((double) n[ids::pitch]))
                        || (n.hasProperty (ids::velocity) && ! finiteIn (n[ids::velocity], 1.0 / 127.0, 1.0))
                        || (juce::int64) n[ids::start] < 0 || (juce::int64) n[ids::length] < 1)
                        return clipName + " has an invalid note";
                }
            }
            else if (c[ids::file].toString().isEmpty())
            {
                return clipName + " has no file";
            }
            if (clip::start (c) < 0)                   return clipName + " starts before zero";
            if (clip::offset (c) < 0)                  return clipName + " has a negative offset";
            if (clip::length (c) <= 0)                 return clipName + " has no length";
            if (clip::sourceLength (c) < 0)            return clipName + " has a negative source length";
            if (clip::sourceLength (c) > 0 && clip::offset (c) + clip::length (c) > clip::sourceLength (c))
                return clipName + " reads past the end of its file";
            if (clip::fadeIn (c) < 0 || clip::fadeOut (c) < 0 || clip::fadeIn (c) + clip::fadeOut (c) > clip::length (c))
                return clipName + " has fades longer than the clip";
            if (c.hasProperty (ids::gainDb) && ! finiteIn (c[ids::gainDb], dsp::FaderTaper::minDb, 24.0))
                return clipName + " gain " + c[ids::gainDb].toString();
        }

        for (const auto* listType : { &ids::INSERTS, &ids::SENDS })
        {
            const bool sends = listType == &ids::SENDS;
            std::set<int> slots;
            for (const auto& item : t.getChildWithName (*listType))
            {
                const int slot = item[ids::slot];
                if (! item.hasType (sends ? ids::SEND : ids::INSERT)
                    || ! juce::isPositiveAndBelow (slot, sends ? Session::maxSends : Session::maxInserts)
                    || ! slots.insert (slot).second)
                    return name + " has an invalid or duplicate " + (sends ? "send" : "insert") + " slot " + juce::String (slot);
                if (sends)
                {
                    const auto target = session.getTrackById (send::target (item));
                    if (! target.isValid() || ! track::isAux (target) || send::target (item) == track::id (t))
                        return name + " sends to missing or invalid track id " + juce::String (send::target (item));
                    if (! finiteIn (item.getProperty (ids::levelDb, 0.0), dsp::FaderTaper::minDb, dsp::FaderTaper::maxDb)
                        || ! finiteIn (item.getProperty (ids::pan, 0.0), -1.0, 1.0))
                        return name + " has a send level or pan out of range";
                }
            }
        }

        if (const int output = track::outputBus (t); output != 0)
        {
            const auto target = session.getTrackById (output);
            if (! target.isValid() || ! track::isAux (target) || output == track::id (t))
                return name + " outputs to missing or invalid track id " + juce::String (output);
        }
        if (routing::feeds (session, track::id (t), track::id (t)))
            return name + " is part of a routing loop";

        juce::StringArray lanes;
        for (const auto& lane : track::automation (t))
        {
            const auto parameter = lane[ids::parameter].toString();
            const auto range = automation::rangeOf (parameter);
            if (! lane.hasType (ids::LANE) || ! range.has_value() || lanes.contains (parameter))
                return name + " has an unknown or duplicate automation lane \"" + parameter + "\"";
            lanes.add (parameter);
            for (const auto& p : lane)
            {
                const double v = p[ids::value];
                if (! std::isfinite (v) || (juce::int64) p[ids::time] < 0 || v < range->min || v > range->max
                    || (range->stepped && ! juce::exactlyEqual (v, std::round (v))))
                    return name + " has an invalid " + parameter + " automation point";
            }
        }
    }

    if ((int) s[ids::nextId] <= maxId)
        return "next id " + s[ids::nextId].toString() + " would reuse id " + juce::String (maxId);
    return {};
}

} // namespace meridian
