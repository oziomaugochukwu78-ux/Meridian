#include "SessionRepair.h"

#include "Routing.h"

#include <dsp/FaderTaper.h>

#include <cmath>
#include <set>

namespace meridian
{
namespace
{
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
    }

    //==========================================================================
    // Ids: tracks and clips share one id space, allocated from nextId.

    void collectUsedIds()
    {
        for (const auto& t : session.getTracksNode())
        {
            maxId = juce::jmax (maxId, (int) t[ids::id]);
            for (const auto& c : track::clips (t))
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
        if (kind != "audio" && kind != "aux")
            set (t, ids::kind, "audio", where, "kind");
        const bool aux = track::isAux (t);

        const int channels = t.getProperty (ids::channels, 1);
        const int wantedChannels = aux ? 2 : juce::jlimit (1, 2, channels);
        if (channels != wantedChannels)
            set (t, ids::channels, wantedChannels, where, "channels");

        clampNumber (t, ids::volumeDb, dsp::FaderTaper::minDb, dsp::FaderTaper::maxDb, 0.0, where);
        clampNumber (t, ids::pan, -1.0, 1.0, 0.0, where);
        if (aux && track::inputChannel (t) != -1)
            set (t, ids::inputChannel, -1, where, "input (aux tracks have none)");
        else if (! aux && (track::inputChannel (t) < -1 || track::inputChannel (t) > 255))
            set (t, ids::inputChannel, 0, where, "input channel");

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

        repairClips (t, where);
        repairSlots (track::inserts (t), ids::INSERT, Session::maxInserts, where, "insert");
        repairSlots (track::sends (t), ids::SEND, Session::maxSends, where, "send");
        for (auto s : track::sends (t))
        {
            clampNumber (s, ids::levelDb, dsp::FaderTaper::minDb, dsp::FaderTaper::maxDb, 0.0, where + " send");
            clampNumber (s, ids::pan, -1.0, 1.0, 0.0, where + " send");
        }
        repairAutomation (track::automation (t), where);
    }

    void repairClips (juce::ValueTree t, const juce::String& trackName)
    {
        auto clips = track::clips (t);
        removeChildrenNotOfType (clips, ids::CLIP, trackName);

        for (int i = clips.getNumChildren(); --i >= 0;)
        {
            auto c = clips.getChild (i);
            const auto where = trackName + ", " + describe (c);

            if (c[ids::file].toString().isEmpty())
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
        for (auto lane : automation)
        {
            removeChildrenNotOfType (lane, ids::POINT, where + " automation");
            const bool volume = lane[ids::parameter].toString() == "volume";
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
                if (volume)
                    clampNumber (p, ids::value, dsp::FaderTaper::minDb, dsp::FaderTaper::maxDb, 0.0, where + " automation");
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
        if (kind != "audio" && kind != "aux")
            return name + " kind " + kind;
        const int channels = t.getProperty (ids::channels, 1);
        if (track::isAux (t) ? channels != 2 : (channels < 1 || channels > 2))
            return name + " channels " + juce::String (channels);
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

        for (const auto& c : track::clips (t))
        {
            if (! c.hasType (ids::CLIP))
                return name + " has a non-clip element";
            if (auto problem = claimId (c); problem.isNotEmpty())
                return problem;
            const auto clipName = name + " clip \"" + clip::name (c) + "\"";
            if (c[ids::file].toString().isEmpty())     return clipName + " has no file";
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

        for (const auto& lane : track::automation (t))
            for (const auto& p : lane)
                if (! std::isfinite ((double) p[ids::value]) || (juce::int64) p[ids::time] < 0)
                    return name + " has an invalid automation point";
    }

    if ((int) s[ids::nextId] <= maxId)
        return "next id " + s[ids::nextId].toString() + " would reuse id " + juce::String (maxId);
    return {};
}

} // namespace meridian
