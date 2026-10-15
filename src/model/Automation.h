#pragma once

#include "Session.h"

#include <optional>
#include <vector>

namespace meridian::automation
{

/** A breakpoint of an automation lane: timeline sample and value in the
    lane's unit. The engine interpolates linearly between breakpoints (for
    volume and sends, linearly in dB), so a straight line drawn is what plays. */
struct Point
{
    juce::int64 time = 0;
    double value = 0.0;
};

/** Lane parameters, as stored in LANE.parameter:
      "volume"             fader, dB (-144 = silence .. +12)
      "pan"                -1 (left) .. +1 (right)
      "mute"               0 (on) or 1 (muted); steps, no ramps between points
      "send:<slot>"        send A..E (slot 0..4) level, dB (-144 .. +12)
      "plugin:<slot>:<i>"  insert A..J (slot 0..9), parameter index i, 0..1 (normalised) */
struct Range
{
    double min = 0.0, max = 1.0;
    bool stepped = false;
};

/** The value range of a lane parameter, or nothing if the name is not valid. */
std::optional<Range> rangeOf (const juce::String& parameter);
juce::String sendParameter (int slot);
juce::String pluginParameter (int slot, int parameterIndex);
/** For "plugin:<slot>:<i>": slot and index; otherwise {-1, -1}. */
std::pair<int, int> pluginTarget (const juce::String& parameter);
/** For "send:<slot>": the slot; otherwise -1. */
int sendTarget (const juce::String& parameter);

juce::ValueTree lane (const juce::ValueTree& track, const juce::String& parameter);
juce::ValueTree getOrCreateLane (juce::ValueTree track, const juce::String& parameter, juce::UndoManager*);
/** Removes a lane entirely (the control's own setting applies again). */
void clearLane (juce::ValueTree track, const juce::String& parameter, juce::UndoManager*);

/** The track's volume lane, or an invalid tree if it has none. */
juce::ValueTree volumeLane (const juce::ValueTree& track);
juce::ValueTree getOrCreateVolumeLane (juce::ValueTree track, juce::UndoManager*);

/** Breakpoints sorted by time. */
std::vector<Point> points (const juce::ValueTree& lane);

/** Adds a breakpoint, replacing any at the same time. The value is clamped
    to the lane's range (and rounded for stepped lanes such as mute). */
void addPoint (juce::ValueTree lane, juce::int64 time, double value, juce::UndoManager*);

/** Replaces the whole lane with these points (e.g. after recording a pass). */
void setPoints (juce::ValueTree lane, const std::vector<Point>&, juce::UndoManager*);

/** Removes breakpoints with from <= time <= to. */
void removeRange (juce::ValueTree lane, juce::int64 from, juce::int64 to, juce::UndoManager*);

/** Removes the breakpoint nearest `time` if it is within `tolerance` samples. */
bool removeNear (juce::ValueTree lane, juce::int64 time, juce::int64 tolerance, juce::UndoManager*);

/** Removes the volume lane. */
void clear (juce::ValueTree track, juce::UndoManager*);

/** The automated value at `time` (held flat outside the breakpoints). */
double valueAt (const std::vector<Point>&, juce::int64 time);

/** The lane an Edit-window track shows: `wanted` if the track has that control
    ("volume", "pan", "mute", or "send:<slot>" while the send exists), else "volume". */
juce::String shownParameter (const juce::ValueTree& track, const juce::String& wanted);

/** Short lane name for the track header: "volume", "pan", "mute", "send A". */
juce::String shortName (const juce::String& parameter);

/** The control's own setting, which plays while it has no automation: fader dB,
    pan, mute (0/1) or send level dB. */
double staticValue (const juce::ValueTree& track, const juce::String& parameter);

} // namespace meridian::automation
