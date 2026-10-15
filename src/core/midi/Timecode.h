#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace meridian::midi
{

/** SMPTE timecode: hours:minutes:seconds:frames at a video frame rate.

    23.976 and 29.97 run at 1000/1001 of 24 and 30 frames per second. Their
    frames are counted as at 24 and 30 (non-drop), so their timecode drifts
    from the clock by 0.1%, except for 29.97 drop-frame, which skips frame
    numbers 0 and 1 at the start of every minute not divisible by ten and so
    stays within a frame of the clock. Drop-frame is written with a ';'
    before the frames.

    A timeline position maps to the frame it falls in (rounded down), after
    an optional session start (e.g. 01:00:00:00 for post-production). */
enum class FrameRate { fps23_976, fps24, fps25, fps29_97, fps29_97drop, fps30 };

struct Timecode
{
    int hours = 0, minutes = 0, seconds = 0, frames = 0;
};

double framesPerSecond (FrameRate) noexcept;   ///< e.g. 30000 / 1001 for 29.97
int nominalFramesPerSecond (FrameRate) noexcept; ///< frames counted per timecode second: 24, 25 or 30
bool isDropFrame (FrameRate) noexcept;

/** "25", "29.97df" ...: the session's stored name, and back. */
const char* frameRateName (FrameRate) noexcept;
std::optional<FrameRate> frameRateFromName (const std::string&) noexcept;
/** For menus: "29.97 fps drop-frame". */
std::string frameRateLabel (FrameRate);

/** Frame count <-> timecode label (drop-frame numbering where it applies).
    Frame counts wrap at 24 hours. */
Timecode fromFrameCount (int64_t frames, FrameRate) noexcept;
int64_t toFrameCount (const Timecode&, FrameRate) noexcept;

/** The frame a timeline sample falls in, counted from the session start. */
int64_t frameAtSample (int64_t sample, double sampleRate, FrameRate, int64_t startFrames = 0) noexcept;
/** The first sample of a frame (inverse of frameAtSample, for the frame's start). */
int64_t sampleAtFrame (int64_t frame, double sampleRate, FrameRate, int64_t startFrames = 0) noexcept;

/** "01:00:00:00", or "00:01:00;02" for drop-frame. */
std::string toString (const Timecode&, FrameRate);
std::string format (int64_t sample, double sampleRate, FrameRate, int64_t startFrames = 0);

/** Reads "hh:mm:ss:ff" (or ';' / '.' before the frames). Nothing if malformed
    or out of range for the rate (including dropped frame numbers). */
std::optional<Timecode> parse (const std::string&, FrameRate);

} // namespace meridian::midi
