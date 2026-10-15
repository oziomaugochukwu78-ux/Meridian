#pragma once

#include <cstdint>
#include <vector>

namespace meridian::midi
{

/** One note: a pitch (MIDI number, 60 = middle C), a velocity (0..1], and a
    start and length in ticks (see TempoMap). */
struct Note
{
    int pitch = 60;
    float velocity = 0.8f;
    int64_t start = 0;
    int64_t length = 960;

    int64_t end() const noexcept { return start + length; }
};

/** The notes of a MIDI clip, always kept valid and sorted (by start, then
    pitch): pitches 0..127, velocities in (0, 1], lengths of at least one tick,
    starts at or after zero. Every editing operation preserves that, so the
    renderer and the UI can rely on it. */
class NoteSequence
{
public:
    /** Adds a note (values are clamped into range). Returns its index. */
    size_t add (Note);

    void removeAt (size_t index);
    void clear() noexcept { notes.clear(); }

    const std::vector<Note>& getNotes() const noexcept { return notes; }
    size_t size() const noexcept { return notes.size(); }
    bool empty() const noexcept { return notes.empty(); }

    /** Calls fn(note) for every note sounding anywhere in [from, to). */
    template <typename Fn>
    void forEachOverlapping (int64_t from, int64_t to, Fn&& fn) const
    {
        for (const auto& n : notes)
        {
            if (n.start >= to)
                break; // sorted by start: nothing later can overlap
            if (n.end() > from)
                fn (n);
        }
    }

    /** Moves note starts toward the nearest multiple of `grid` ticks.
        strength 1 snaps fully, 0.5 halves the distance (keeps some feel). */
    void quantize (int64_t grid, double strength = 1.0);

    /** Shifts every pitch by `semitones`; notes pushed outside 0..127 are clamped. */
    void transpose (int semitones);

    int64_t endTick() const noexcept; ///< end of the last-ending note, 0 if empty

private:
    static Note sanitised (Note) noexcept;
    void sort();

    std::vector<Note> notes;
};

/** In the engine's event streams (live input, clip events, recording) a "pitch"
    of sustainPedal is the sustain pedal (MIDI CC 64): on = pedal down. */
inline constexpr int sustainPedal = -1;

/** A stretch of a clip with the sustain pedal held, in ticks like notes. */
struct PedalRange
{
    int64_t start = 0;
    int64_t length = 1;

    int64_t end() const noexcept { return start + length; }
};

/** Sorted, each at least a tick long, starting at or after zero, with
    overlapping or touching ranges merged (the pedal is either down or up). */
std::vector<PedalRange> normalisedPedal (std::vector<PedalRange>);

/** In the same event streams, the pitch wheel and the mod wheel (MIDI CC 1).
    The event's velocity carries the value: -1..+1 for the pitch wheel (0 =
    centre), 0..1 for the mod wheel. */
inline constexpr int pitchBend = -2;
inline constexpr int modWheel = -3;

/** A controller value from `tick` on (it holds until the next point). */
struct ControlPoint
{
    int64_t tick = 0;
    float value = 0.0f;
};

/** Sorted by tick, ticks at or after zero, values clamped to [low, high], and one
    point per tick (the last one given wins). */
std::vector<ControlPoint> normalisedControl (std::vector<ControlPoint>, float low, float high);

/** The value at `tick`: the last point at or before it, or `rest` before the first. */
float controlValueAt (const std::vector<ControlPoint>& normalised, int64_t tick, float rest) noexcept;

} // namespace meridian::midi
