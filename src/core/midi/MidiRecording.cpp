#include "MidiRecording.h"

#include <algorithm>
#include <array>

namespace meridian::midi
{

RecordedTake buildTake (std::vector<RecordedEvent> events, int64_t recordStart, int64_t recordEnd,
                        int64_t compensation, const TempoMap& tempo, double sampleRate)
{
    RecordedTake take;
    take.clipStart = std::max<int64_t> (0, recordStart - compensation);
    const auto clipEnd = std::max (take.clipStart + 1, recordEnd - compensation);
    take.clipLength = clipEnd - take.clipStart;

    std::stable_sort (events.begin(), events.end(), [] (const RecordedEvent& a, const RecordedEvent& b) { return a.time < b.time; });

    // Ticks from the clip start under the tempo there: the exact inverse of how
    // playback places notes (TempoMap::samplesFromOrigin).
    auto ticksAt = [&] (int64_t time) { return tempo.ticksFromOrigin (take.clipStart, time, sampleRate); };

    struct Held { int64_t since = -1; float velocity = 0.0f; };
    std::array<Held, 128> held {};

    auto close = [&] (int pitch, int64_t at)
    {
        auto& h = held[(size_t) pitch];
        if (h.since < 0)
            return;
        const auto start = ticksAt (h.since);
        take.notes.push_back ({ pitch, h.velocity, start, std::max<int64_t> (1, ticksAt (at) - start) });
        h.since = -1;
    };

    int64_t pedalSince = -1;
    auto liftPedal = [&] (int64_t at)
    {
        if (pedalSince < 0)
            return;
        const auto start = ticksAt (pedalSince);
        take.sustain.push_back ({ start, std::max<int64_t> (1, ticksAt (at) - start) });
        pedalSince = -1;
    };

    for (const auto& e : events)
    {
        const auto at = std::clamp (e.time - compensation, take.clipStart, clipEnd);
        if (e.pitch == sustainPedal)
        {
            if (e.on && pedalSince < 0)
                pedalSince = at;
            else if (! e.on)
                liftPedal (at);
            continue;
        }
        if (e.pitch == pitchBend || e.pitch == modWheel)
        {
            (e.pitch == pitchBend ? take.pitchBend : take.modWheel).push_back ({ ticksAt (at), e.velocity });
            continue;
        }
        if (e.pitch < 0 || e.pitch > 127)
            continue;
        if (e.on && e.velocity > 0.0f)
        {
            close (e.pitch, at); // retriggered without a note-off
            held[(size_t) e.pitch] = { at, std::clamp (e.velocity, 0.0f, 1.0f) };
        }
        else
        {
            close (e.pitch, at);
        }
    }

    for (int pitch = 0; pitch < 128; ++pitch)
        close (pitch, clipEnd); // still held when recording stopped
    liftPedal (clipEnd);
    take.sustain = normalisedPedal (std::move (take.sustain));
    take.pitchBend = normalisedControl (std::move (take.pitchBend), -1.0f, 1.0f);
    take.modWheel = normalisedControl (std::move (take.modWheel), 0.0f, 1.0f);

    std::sort (take.notes.begin(), take.notes.end(), [] (const Note& a, const Note& b)
    {
        return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
    });
    return take;
}

} // namespace meridian::midi
