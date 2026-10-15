#pragma once

#include "Session.h"

/** Fades and crossfades on audio clips (Edit > Fade/Crossfade).

    A crossfade joins two butted clips. The outgoing clip is extended past the
    join into its source material (its "handle") and fades out; the incoming
    clip is extended back before the join and fades in, over the same span.
    The incoming clip sits above (a later sibling) and marks its fade-in as a
    crossfade, so it does not cover the clip under it there: both play,
    each through its fade. The extensions are stored (extendedIn on the
    incoming clip, extendedOut on the outgoing one), so a crossfade can be
    removed again, giving back the butted clips exactly.

    Fade shapes: equal power (sine/cosine quarter waves: for different
    material, the level stays even) or equal gain (linear: the two gains sum
    to exactly 1, so a crossfade inside one continuous take is seamless). */
namespace meridian::fades
{

enum class Shape { equalPower, equalGain };

struct Result
{
    int crossfades = 0, fadeIns = 0, fadeOuts = 0;
    int shortened = 0;   ///< crossfades made shorter than asked: not enough audio past the clip edges
    int impossible = 0;  ///< joins with no audio past either edge: no crossfade
};

/** Fade/Crossfade over the selected range [start, end) of a track.

    One clip edge in the range: the range is the fade (a crossfade centred
    wherever the range lies over the join, a fade-in from the clip start to
    the range end, a fade-out from the range start to the clip end).
    Several edges (batch fades): each join gets a crossfade of `batchLength`
    centred on it, each free clip start a fade-in and each free end a
    fade-out of that length. MIDI clips are left alone. */
Result apply (juce::ValueTree track, juce::int64 start, juce::int64 end, Shape, juce::int64 batchLength, juce::UndoManager*);

/** Crossfades the butted clips `outgoing` (ending at the join) and `incoming`
    (starting there) over [from, to), from < join < to or touching it,
    shortened to the audio available. False if no crossfade was possible. */
bool crossfade (juce::ValueTree outgoing, juce::ValueTree incoming, juce::int64 from, juce::int64 to, Shape,
                juce::UndoManager*, bool* wasShortened = nullptr);

/** Removes the clip's fades, and any crossfade it is part of (the clips go
    back to butting at the join). True if anything changed. */
bool removeFades (juce::ValueTree clip, juce::UndoManager*);

/** True if the clip's fade-in is a crossfade with the clip under it. */
bool isCrossfadeIn (const juce::ValueTree& clip);

/** Default length of batch fades, in seconds. */
constexpr double defaultBatchSeconds = 0.010;

} // namespace meridian::fades
