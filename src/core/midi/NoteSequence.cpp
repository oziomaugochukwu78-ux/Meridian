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
    return (size_t) (notes.insert (at, n) - notes.begin());
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

} // namespace meridian::midi
