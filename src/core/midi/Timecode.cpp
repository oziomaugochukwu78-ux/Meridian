#include "Timecode.h"

#include <cmath>
#include <cstdio>

namespace meridian::midi
{
namespace
{
constexpr int64_t framesPerDay (int nominal) noexcept { return (int64_t) nominal * 60 * 60 * 24; }

// 29.97 drop-frame: 2 frame numbers dropped per minute, except every tenth.
constexpr int64_t dropPerMinute = 2;
constexpr int64_t dfFramesPerMinute = 30 * 60 - dropPerMinute;            // 1798
constexpr int64_t dfFramesPer10Minutes = 10 * 30 * 60 - 9 * dropPerMinute; // 17982
constexpr int64_t dfFramesPerDay = 144 * dfFramesPer10Minutes;            // 2589408

int64_t wrap (int64_t frames, int64_t day) noexcept
{
    frames %= day;
    return frames < 0 ? frames + day : frames;
}
} // namespace

double framesPerSecond (FrameRate r) noexcept
{
    switch (r)
    {
        case FrameRate::fps23_976:    return 24000.0 / 1001.0;
        case FrameRate::fps24:        return 24.0;
        case FrameRate::fps25:        return 25.0;
        case FrameRate::fps29_97:
        case FrameRate::fps29_97drop: return 30000.0 / 1001.0;
        case FrameRate::fps30:        return 30.0;
    }
    return 25.0;
}

int nominalFramesPerSecond (FrameRate r) noexcept
{
    return r == FrameRate::fps23_976 || r == FrameRate::fps24 ? 24 : r == FrameRate::fps25 ? 25 : 30;
}

bool isDropFrame (FrameRate r) noexcept { return r == FrameRate::fps29_97drop; }

const char* frameRateName (FrameRate r) noexcept
{
    switch (r)
    {
        case FrameRate::fps23_976:    return "23.976";
        case FrameRate::fps24:        return "24";
        case FrameRate::fps25:        return "25";
        case FrameRate::fps29_97:     return "29.97";
        case FrameRate::fps29_97drop: return "29.97df";
        case FrameRate::fps30:        return "30";
    }
    return "25";
}

std::optional<FrameRate> frameRateFromName (const std::string& name) noexcept
{
    for (const auto r : { FrameRate::fps23_976, FrameRate::fps24, FrameRate::fps25, FrameRate::fps29_97,
                          FrameRate::fps29_97drop, FrameRate::fps30 })
        if (name == frameRateName (r))
            return r;
    return std::nullopt;
}

std::string frameRateLabel (FrameRate r)
{
    return std::string (frameRateName (r) == std::string ("29.97df") ? "29.97" : frameRateName (r))
           + (isDropFrame (r) ? " fps drop-frame" : " fps");
}

Timecode fromFrameCount (int64_t frames, FrameRate r) noexcept
{
    const int64_t nominal = nominalFramesPerSecond (r);
    if (isDropFrame (r))
    {
        frames = wrap (frames, dfFramesPerDay);
        const auto tens = frames / dfFramesPer10Minutes;
        const auto rest = frames % dfFramesPer10Minutes;
        // Add back the numbers skipped so far, then count as at 30.
        frames += 9 * dropPerMinute * tens + (rest > dropPerMinute ? dropPerMinute * ((rest - dropPerMinute) / dfFramesPerMinute) : 0);
    }
    else
    {
        frames = wrap (frames, framesPerDay ((int) nominal));
    }
    Timecode tc;
    tc.frames = (int) (frames % nominal);
    tc.seconds = (int) ((frames / nominal) % 60);
    tc.minutes = (int) ((frames / (nominal * 60)) % 60);
    tc.hours = (int) (frames / (nominal * 3600));
    return tc;
}

int64_t toFrameCount (const Timecode& tc, FrameRate r) noexcept
{
    const int64_t nominal = nominalFramesPerSecond (r);
    int64_t frames = ((int64_t) tc.hours * 3600 + (int64_t) tc.minutes * 60 + tc.seconds) * nominal + tc.frames;
    if (isDropFrame (r))
    {
        const int64_t totalMinutes = (int64_t) tc.hours * 60 + tc.minutes;
        frames -= dropPerMinute * (totalMinutes - totalMinutes / 10);
    }
    return frames;
}

int64_t frameAtSample (int64_t sample, double sampleRate, FrameRate r, int64_t startFrames) noexcept
{
    if (sampleRate <= 0.0)
        return startFrames;
    // A tiny epsilon keeps a frame's exact first sample in that frame despite rounding.
    return startFrames + (int64_t) std::floor ((double) sample / sampleRate * framesPerSecond (r) + 1.0e-9);
}

int64_t sampleAtFrame (int64_t frame, double sampleRate, FrameRate r, int64_t startFrames) noexcept
{
    return (int64_t) std::ceil ((double) (frame - startFrames) / framesPerSecond (r) * sampleRate - 1.0e-9);
}

std::string toString (const Timecode& tc, FrameRate r)
{
    char text[32];
    std::snprintf (text, sizeof (text), "%02d:%02d:%02d%c%02d", tc.hours, tc.minutes, tc.seconds, isDropFrame (r) ? ';' : ':', tc.frames);
    return text;
}

std::string format (int64_t sample, double sampleRate, FrameRate r, int64_t startFrames)
{
    return toString (fromFrameCount (frameAtSample (sample, sampleRate, r, startFrames), r), r);
}

std::optional<Timecode> parse (const std::string& text, FrameRate r)
{
    int values[4] = {};
    int count = 0, digits = 0;
    for (const char c : text)
    {
        if (c >= '0' && c <= '9')
        {
            if (count >= 4 || ++digits > 2)
                return std::nullopt;
            values[count] = values[count] * 10 + (c - '0');
        }
        else if (c == ':' || c == ';' || c == '.')
        {
            if (digits == 0 || ++count > 3)
                return std::nullopt;
            digits = 0;
        }
        else if (c != ' ')
        {
            return std::nullopt;
        }
    }
    if (count != 3 || digits == 0)
        return std::nullopt;

    const Timecode tc { values[0], values[1], values[2], values[3] };
    if (tc.hours > 23 || tc.minutes > 59 || tc.seconds > 59 || tc.frames >= nominalFramesPerSecond (r))
        return std::nullopt;
    if (isDropFrame (r) && tc.seconds == 0 && tc.frames < dropPerMinute && tc.minutes % 10 != 0)
        return std::nullopt; // a dropped number: it never appears
    return tc;
}

} // namespace meridian::midi
