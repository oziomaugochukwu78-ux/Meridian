#pragma once

#include "AutomationCurve.h"

#include <cstdint>
#include <vector>

namespace meridian::dsp
{

/** How a control's moves are recorded while the transport plays.
      read:  play the lane back; moves are not recorded
      touch: record only while the control is held; on release, glide back
             to the existing automation
      latch: record from the first touch until the transport stops
      write: record the whole pass, touched or not */
enum class AutomationMode { off, read, touch, latch, write };

/** Records one control during one pass of the transport and merges the
    result into its lane.

    Feed it the control's value regularly (a UI timer, every 10-30 ms) with
    the playhead position and whether the control is being held. finish()
    returns the new lane: the existing points outside the recorded stretches,
    the recorded moves inside them (thinned: the fewest points that stay
    within `tolerance` of every value received), and joins at each end so the
    curve outside a recorded stretch is unchanged.

    Pure logic, no threads: the caller decides when to sample. */
class AutomationWriter
{
public:
    /** baseValue: the control's value where the lane has no points (its
        static setting), so a first recording joins it correctly.
        glideSamples: touch mode returns to the existing curve over this long. */
    AutomationWriter (AutomationMode, std::vector<AutomationPoint> existingLane, double baseValue, double tolerance,
                      int64_t glideSamples = 0);

    /** position: playhead (timeline samples), increasing during a pass. */
    void update (int64_t position, double value, bool touched);

    /** The merged lane. stopPosition: where the transport stopped. */
    std::vector<AutomationPoint> finish (int64_t stopPosition);

    /** True once anything was recorded (the lane will change). */
    bool hasRecorded() const noexcept { return ! stretches.empty(); }

    /** Fewest points such that linear interpolation between them stays within
        tolerance of every input point (Ramer-Douglas-Peucker). Keeps the ends. */
    static std::vector<AutomationPoint> thin (const std::vector<AutomationPoint>& points, double tolerance);

private:
    struct Stretch
    {
        std::vector<AutomationPoint> points; // as received
        bool open = true;
    };

    bool recording (bool touched);

    AutomationMode mode;
    std::vector<AutomationPoint> existingPoints;
    double baseValue;
    double tolerance;
    int64_t glide;
    std::vector<Stretch> stretches;
    bool latched = false;
    int64_t lastPosition = -1;
};

} // namespace meridian::dsp
