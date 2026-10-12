#include "SessionTemplates.h"

#include "Session.h"

namespace meridian
{

const std::vector<SessionTemplate>& builtInTemplates()
{
    static const std::vector<SessionTemplate> templates {
        { "Music", "Band Tracking", "Drums, bass, guitars and vocals ready to record",
          { { "Kick", 1 }, { "Snare", 1 }, { "OH", 2 }, { "Room", 2 }, { "Bass DI", 1 },
            { "Gtr L", 1 }, { "Gtr R", 1 }, { "Keys", 2 }, { "Lead Vox", 1 }, { "BV", 1 } } },
        { "Music", "Singer-Songwriter", "Vocal and acoustic guitar",
          { { "Vocal", 1 }, { "Acoustic", 1 }, { "Acoustic Amb", 2 } } },
        { "Mixing", "Stereo Stems", "Eight stereo stem tracks for mixing",
          { { "Drums", 2 }, { "Bass", 2 }, { "Guitars", 2 }, { "Keys", 2 },
            { "Synths", 2 }, { "Vocals", 2 }, { "BVs", 2 }, { "FX", 2 } } },
        { "Post", "Podcast", "Two hosts, a guest and a music bed",
          { { "Host 1", 1 }, { "Host 2", 1 }, { "Guest", 1 }, { "Music Bed", 2 } } },
        { "Post", "Voice-Over", "Single narrator with room tone",
          { { "Narration", 1 }, { "Room Tone", 1 } } },
    };
    return templates;
}

juce::StringArray templateGroups()
{
    juce::StringArray groups;
    for (const auto& t : builtInTemplates())
        groups.addIfNotAlreadyThere (t.group);
    return groups;
}

void applyTemplate (Session& session, const SessionTemplate& t)
{
    for (const auto& spec : t.tracks)
        session.addTrack (spec.name, spec.channels, nullptr);
}

} // namespace meridian
