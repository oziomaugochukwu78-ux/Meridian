#include "SessionTemplates.h"

#include "ConsolePresets.h"
#include "MidiModel.h"
#include "Routing.h"
#include "Session.h"

namespace meridian
{
namespace
{
using T = SessionTemplate::TrackSpec;

// Effect chains used across templates (built-in effects, by name).
const juce::StringArray vocalChain { "Meridian EQ", "Meridian Compressor", "Meridian De-Esser" };
const juce::StringArray voiceChain { "Meridian Gate", "Meridian EQ", "Meridian Compressor" };

T aux (const juce::String& name, const juce::String& effect)
{
    T t;
    t.name = name;
    t.channels = 2;
    t.kind = "aux";
    t.inserts = { effect };
    return t;
}
} // namespace

// The table below lists only the fields each track needs ({ "Drums", 2 }); the
// rest keep their defaults on purpose, which GCC's -Wextra would flag.
#if defined(__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
const std::vector<SessionTemplate>& builtInTemplates()
{
    static const std::vector<SessionTemplate> templates {
        { "Music", "Hip-Hop", "Drums, an 808, keys and a vocal chain with a reverb bus, at 90 BPM",
          { { "Drums", 2, "drums" },
            { "808", 2, "instrument", -3.0 },
            { "Keys", 2, "instrument", -6.0, {}, { { "Reverb", -16.0 } } },
            { "Lead Vox", 1, "audio", 0.0, vocalChain, { { "Reverb", -18.0 } } },
            { "Ad-libs", 1, "audio", -4.0, { "Meridian EQ", "Meridian Compressor" }, { { "Reverb", -14.0 } } },
            aux ("Reverb", "Meridian Reverb") },
          90.0, "Warm Tape" },
        { "Music", "Electronic", "Drums, bass, lead, pad and vocal, with reverb and delay buses, at 124 BPM",
          { { "Drums", 2, "drums" },
            { "Bass", 2, "instrument", -3.0 },
            { "Lead", 2, "instrument", -6.0, {}, { { "Reverb", -14.0 }, { "Delay", -16.0 } } },
            { "Pad", 2, "instrument", -9.0, {}, { { "Reverb", -10.0 } } },
            { "Vox", 1, "audio", 0.0, vocalChain, { { "Reverb", -16.0 }, { "Delay", -20.0 } } },
            aux ("Reverb", "Meridian Reverb"),
            aux ("Delay", "Meridian Delay") },
          124.0, "Vintage Console" },
        { "Music", "Band Tracking", "Drums, bass, guitars and vocals ready to record, with a reverb bus",
          { { "Kick", 1 }, { "Snare", 1 }, { "OH", 2, "audio", 0.0, {}, { { "Reverb", -20.0 } } }, { "Room", 2 }, { "Bass DI", 1 },
            { "Gtr L", 1 }, { "Gtr R", 1 }, { "Keys", 2, "audio", 0.0, {}, { { "Reverb", -18.0 } } },
            { "Lead Vox", 1, "audio", 0.0, vocalChain, { { "Reverb", -16.0 } } },
            { "BV", 1, "audio", -3.0, { "Meridian EQ", "Meridian Compressor" }, { { "Reverb", -14.0 } } },
            aux ("Reverb", "Meridian Reverb") },
          0.0, "Vintage Console" },
        { "Music", "Singer-Songwriter", "Vocal and acoustic guitar, with a reverb bus",
          { { "Vocal", 1, "audio", 0.0, vocalChain, { { "Reverb", -16.0 } } },
            { "Acoustic", 1, "audio", 0.0, { "Meridian EQ" }, { { "Reverb", -20.0 } } },
            { "Acoustic Amb", 2, "audio", -6.0 },
            aux ("Reverb", "Meridian Reverb") },
          0.0, "Warm Tape" },
        { "Mixing", "Stereo Stems", "Eight stereo stem tracks for mixing",
          { { "Drums", 2 }, { "Bass", 2 }, { "Guitars", 2 }, { "Keys", 2 },
            { "Synths", 2 }, { "Vocals", 2 }, { "BVs", 2 }, { "FX", 2 } } },
        { "Post", "Podcast", "Two hosts and a guest with gated, levelled voices, and a music bed",
          { { "Host 1", 1, "audio", 0.0, voiceChain }, { "Host 2", 1, "audio", 0.0, voiceChain },
            { "Guest", 1, "audio", 0.0, voiceChain }, { "Music Bed", 2, "audio", -12.0 } },
          0.0, "Clean" },
        { "Post", "Voice-Over", "Single narrator with room tone",
          { { "Narration", 1, "audio", 0.0, vocalChain }, { "Room Tone", 1, "audio", -6.0 } },
          0.0, "Clean" },
        { "Post", "Film (Dialogue / Music / Effects)", "Dialogue, music and effects tracks feeding DX, MX and FX stem buses",
          { { "Dialogue 1", 1, "audio", 0.0, { "Meridian EQ", "Meridian Compressor" }, {}, "DX" },
            { "Dialogue 2", 1, "audio", 0.0, { "Meridian EQ", "Meridian Compressor" }, {}, "DX" },
            { "Music", 2, "audio", -6.0, {}, {}, "MX" },
            { "Effects", 2, "audio", 0.0, {}, {}, "FX" },
            { "Ambience", 2, "audio", -9.0, {}, {}, "FX" },
            { "DX", 2, "aux" }, { "MX", 2, "aux" }, { "FX", 2, "aux" } },
          0.0, "Clean" },
    };
    return templates;
}
#if defined(__GNUC__)
 #pragma GCC diagnostic pop
#endif

juce::StringArray templateGroups()
{
    juce::StringArray groups;
    for (const auto& t : builtInTemplates())
        groups.addIfNotAlreadyThere (t.group);
    return groups;
}

void applyTemplate (Session& session, const SessionTemplate& t)
{
    auto kindOf = [] (const juce::String& kind)
    {
        return kind == "aux" ? Session::TrackKind::aux
             : kind == "instrument" ? Session::TrackKind::instrument
             : kind == "drums" ? Session::TrackKind::drums
                               : Session::TrackKind::audio;
    };

    // Tracks first (buses included), then the routes between them.
    for (const auto& spec : t.tracks)
    {
        auto track = session.addTrack (spec.name, spec.channels, nullptr, kindOf (spec.kind));
        if (! juce::exactlyEqual (spec.volumeDb, 0.0))
            track.setProperty (ids::volumeDb, spec.volumeDb, nullptr);
    }

    auto trackNamed = [&session] (const juce::String& name)
    {
        return session.getTracksNode().getChildWithProperty (ids::name, name);
    };
    for (const auto& spec : t.tracks)
    {
        auto track = trackNamed (spec.name);
        if (spec.output.isNotEmpty())
            if (const auto bus = trackNamed (spec.output); bus.isValid())
                routing::setOutput (session, track, track::id (bus), nullptr);
        for (int slot = 0; slot < (int) spec.sends.size() && slot < Session::maxSends; ++slot)
        {
            const auto& send = spec.sends[(size_t) slot];
            if (const auto bus = trackNamed (send.bus); bus.isValid() && routing::setSend (session, track, slot, track::id (bus), nullptr))
                routing::findSend (track, slot).setProperty (ids::levelDb, send.levelDb, nullptr);
        }
    }

    if (t.tempo > 0.0)
        midimodel::setTempoChange (session, 0, t.tempo, nullptr);
    if (const auto* preset = consolepresets::find (t.consolePreset))
        consolepresets::apply (session.getState(), *preset, nullptr);
}

} // namespace meridian
