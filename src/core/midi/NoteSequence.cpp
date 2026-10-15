#include "NoteSequence.h"

#include <algorithm>
#include <cmath>

namespace meridian::midi
{

Note NoteSequence::sanitised (Note n) noexcept
{
    n.pitch = std::clamp (n.pitch, 0, 127);
    n.velocity = std::isfinite (n.velocity) ? std::clamp (n.velocity, 1.0f / 127.0f, 1.0f) : 0.8f;
    n.start = std::max<int64_t> (0, n.start);
    n.length = std::max<int64_t> (1, n.length);
    return n;
}

void NoteSequence::sort()
{
    std::stable_sort (notes.begin(), notes.end(), [] (const Note& a, const Note& b)
    {
        return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
    });
}

size_t NoteSequence::add (Note n)
{
    n = sanitised (n);
    const auto at = std::upper_bound (notes.begin(), notes.end(), n, [] (const Note& a, const Note& b)
    {
        return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
    });
    // Two statements: the operands of '-' are unsequenced, and begin() read before
    // a reallocating insert would give a garbage index (it did, with GCC).
    const auto inserted = notes.insert (at, n);
    return (size_t) (inserted - notes.begin());
}

void NoteSequence::removeAt (size_t index)
{
    if (index < notes.size())
        notes.erase (notes.begin() + (std::ptrdiff_t) index);
}

void NoteSequence::quantize (int64_t grid, double strength)
{
    if (grid <= 0 || ! std::isfinite (strength))
        return;
    strength = std::clamp (strength, 0.0, 1.0);

    for (auto& n : notes)
    {
        const auto nearest = (int64_t) std::llround ((double) n.start / (double) grid) * grid;
        n.start = std::max<int64_t> (0, n.start + (int64_t) std::llround ((double) (nearest - n.start) * strength));
    }
    sort();
}

void NoteSequence::transpose (int semitones)
{
    for (auto& n : notes)
        n.pitch = std::clamp (n.pitch + semitones, 0, 127);
    sort();
}

int64_t NoteSequence::endTick() const noexcept
{
    int64_t end = 0;
    for (const auto& n : notes)
        end = std::max (end, n.end());
    return end;
}

std::vector<PedalRange> normalisedPedal (std::vector<PedalRange> ranges)
{
    for (auto& r : ranges)
    {
        if (r.start < 0)
        {
            r.length += r.start; // the part before zero is cut off
            r.start = 0;
        }
        r.length = std::max<int64_t> (1, r.length);
    }
    std::sort (ranges.begin(), ranges.end(), [] (const PedalRange& a, const PedalRange& b) { return a.start < b.start; });

    std::vector<PedalRange> merged;
    for (const auto& r : ranges)
    {
        if (! merged.empty() && r.start <= merged.back().end())
            merged.back().length = std::max (merged.back().end(), r.end()) - merged.back().start;
        else
            merged.push_back (r);
    }
    return merged;
}

std::vector<ControlPoint> normalisedControl (std::vector<ControlPoint> points, float low, float high)
{
    for (auto& p : points)
    {
        p.tick = std::max<int64_t> (0, p.tick);
        p.value = std::isfinite (p.value) ? std::clamp (p.value, low, high) : 0.0f;
    }
    std::stable_sort (points.begin(), points.end(), [] (const ControlPoint& a, const ControlPoint& b) { return a.tick < b.tick; });

    std::vector<ControlPoint> unique;
    for (const auto& p : points)
    {
        if (! unique.empty() && unique.back().tick == p.tick)
            unique.back() = p; // the later one wins
        else
            unique.push_back (p);
    }
    return unique;
}

float controlValueAt (const std::vector<ControlPoint>& points, int64_t tick, float rest) noexcept
{
    float value = rest;
    for (const auto& p : points)
    {
        if (p.tick > tick)
            break;
        value = p.value;
    }
    return value;
}

} // namespace meridian::midi
