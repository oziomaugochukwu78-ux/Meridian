#pragma once

#include "Session.h"

#include <vector>

namespace meridian::automation
{

/** A breakpoint of a track's volume automation: timeline sample and level in dB
    (-144 or below = silence). The engine interpolates linearly in dB between
    breakpoints, so a straight line drawn on screen is what plays. */
struct Point
{
    juce::int64 time = 0;
    double db = 0.0;
};

/** The track's volume lane, or an invalid tree if it has none. */
juce::ValueTree volumeLane (const juce::ValueTree& track);
juce::ValueTree getOrCreateVolumeLane (juce::ValueTree track, juce::UndoManager*);

/** Breakpoints sorted by time. */
std::vector<Point> points (const juce::ValueTree& lane);

/** Adds a breakpoint, replacing any at the same time. */
void addPoint (juce::ValueTree lane, juce::int64 time, double db, juce::UndoManager*);

/** Removes breakpoints with from <= time <= to. */
void removeRange (juce::ValueTree lane, juce::int64 from, juce::int64 to, juce::UndoManager*);

/** Removes the breakpoint nearest `time` if it is within `tolerance` samples. */
bool removeNear (juce::ValueTree lane, juce::int64 time, juce::int64 tolerance, juce::UndoManager*);

void clear (juce::ValueTree track, juce::UndoManager*);

/** The automated level at `time` (held flat outside the breakpoints). */
double valueAt (const std::vector<Point>&, juce::int64 time);

} // namespace meridian::automation
