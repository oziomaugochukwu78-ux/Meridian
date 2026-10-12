#pragma once

#include <dsp/LinearRamp.h>

#include <atomic>
#include <utility>

namespace meridian
{

/** The per-track values that cross the thread boundary.

    The message thread writes the mixer controls (from the session tree, the UI
    or a remote command); the audio thread reads them at the start of every
    block and ramps toward them. The audio thread writes meter peaks; the UI
    collects them. Every shared field is a lock-free atomic.

    One instance lives as long as its track, across any number of render-graph
    rebuilds, so ramps and meters never jump when the graph is swapped. */
struct TrackRealtimeState
{
    static_assert (std::atomic<double>::is_always_lock_free);
    static_assert (std::atomic<float>::is_always_lock_free);

    explicit TrackRealtimeState (int idOfTrack) : trackId (idOfTrack) {}

    const int trackId;

    // Message thread -> audio thread
    std::atomic<double> volumeDb { 0.0 };   ///< -infinity = fader fully down
    std::atomic<double> pan { 0.0 };        ///< -1 .. +1
    std::atomic<bool> muted { false };
    std::atomic<bool> soloed { false };
    std::atomic<bool> recordArmed { false };
    std::atomic<bool> automationRead { true };

    // Audio thread -> UI: peak since the UI last collected it (post-fader).
    std::atomic<float> peakLeft { 0.0f };
    std::atomic<float> peakRight { 0.0f };

    void publishPeaks (float left, float right) noexcept
    {
        raiseTo (peakLeft, left);
        raiseTo (peakRight, right);
    }

    /** UI thread: returns and resets the accumulated peaks. */
    std::pair<float, float> collectPeaks() noexcept
    {
        return { peakLeft.exchange (0.0f, std::memory_order_acq_rel),
                 peakRight.exchange (0.0f, std::memory_order_acq_rel) };
    }

    // Audio thread only (never touched elsewhere after construction)
    dsp::LinearRamp faderRamp;
    dsp::LinearRamp muteRamp;
    dsp::LinearRamp panLeftRamp;
    dsp::LinearRamp panRightRamp;
    bool rampsInitialised = false;

private:
    static void raiseTo (std::atomic<float>& slot, float value) noexcept
    {
        auto current = slot.load (std::memory_order_relaxed);
        while (value > current && ! slot.compare_exchange_weak (current, value, std::memory_order_acq_rel))
        {
        }
    }
};

} // namespace meridian
