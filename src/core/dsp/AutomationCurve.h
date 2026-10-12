#pragma once

#include <cstdint>
#include <vector>

namespace meridian::dsp
{

struct AutomationPoint
{
    int64_t time = 0;   ///< Timeline position in samples.
    double value = 0.0; ///< Parameter value in the lane's native unit (linear gain for volume).
};

/** An immutable breakpoint envelope evaluated with sample accuracy.

    Built on the message thread, then handed to the audio thread inside a render
    snapshot and only ever read there. Values are linearly interpolated between
    breakpoints and held flat before the first and after the last point. */
class AutomationCurve
{
public:
    AutomationCurve() = default;

    /** Points need not be sorted; duplicates at the same time keep the later one. */
    explicit AutomationCurve (std::vector<AutomationPoint> points);

    bool isEmpty() const noexcept { return points.empty(); }
    size_t size() const noexcept  { return points.size(); }

    double valueAt (int64_t time) const noexcept;

    /** dst[i] = valueAt (startTime + i). O(log P + numSamples). */
    void render (int64_t startTime, double* dst, int numSamples) const noexcept;

private:
    std::vector<AutomationPoint> points;
};

} // namespace meridian::dsp
