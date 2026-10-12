#pragma once

#include <atomic>
#include <cstdint>

namespace meridian
{

/** Transport state shared by the message thread (requests) and the audio
    thread (execution). All members are lock-free atomics. */
class Transport
{
public:
    enum class State : int { stopped, playing, recording };

    static_assert (std::atomic<int64_t>::is_always_lock_free);

    State getState() const noexcept                 { return state.load (std::memory_order_acquire); }
    bool isRolling() const noexcept                 { return getState() != State::stopped; }
    int64_t getPosition() const noexcept            { return playhead.load (std::memory_order_acquire); }

    /** Message thread. Starting playback remembers where it began; stopping
        returns there, the classic "return to start" transport behaviour.
        countInSamples > 0: that long, only the click sounds and the playhead
        waits; then playback (or recording) starts. */
    void play (State newState = State::playing, int64_t countInSamples = 0) noexcept
    {
        if (getState() == State::stopped)
        {
            playStart.store (getPosition(), std::memory_order_relaxed);
            countInLength.store (countInSamples > 0 ? countInSamples : 0, std::memory_order_relaxed);
            countInRemaining.store (countInSamples > 0 ? countInSamples : 0, std::memory_order_relaxed);
        }
        state.store (newState, std::memory_order_release);
    }

    void stop() noexcept
    {
        const bool wasRolling = isRolling();
        state.store (State::stopped, std::memory_order_release);
        countInRemaining.store (0, std::memory_order_relaxed);
        if (wasRolling)
            locate (playStart.load (std::memory_order_relaxed));
    }

    /** Message thread. Loop playback between start and end (samples); only
        while playing, never while recording. An empty range turns it off. */
    void setLoop (bool enabled, int64_t start, int64_t end) noexcept
    {
        loopStart.store (start < 0 ? 0 : start, std::memory_order_relaxed);
        loopEnd.store (end, std::memory_order_relaxed);
        loopEnabled.store (enabled && end > start, std::memory_order_release);
    }

    bool isLoopEnabled() const noexcept   { return loopEnabled.load (std::memory_order_acquire); }
    int64_t getLoopStart() const noexcept { return loopStart.load (std::memory_order_relaxed); }
    int64_t getLoopEnd() const noexcept   { return loopEnd.load (std::memory_order_relaxed); }

    int64_t getCountInRemaining() const noexcept { return countInRemaining.load (std::memory_order_acquire); }
    int64_t getCountInLength() const noexcept    { return countInLength.load (std::memory_order_relaxed); }

    /** Message thread: move the playhead. Applied by the audio thread at the
        start of its next block so position changes are block-synchronous. */
    void locate (int64_t position) noexcept
    {
        pendingLocate.store (position < 0 ? 0 : position, std::memory_order_release);
        if (! audioRunning.load (std::memory_order_acquire))
            applyPendingLocate(); // no audio thread to do it
    }

    //==============================================================================
    // Audio thread

    void applyPendingLocate() noexcept
    {
        const auto target = pendingLocate.exchange (-1, std::memory_order_acq_rel);
        if (target >= 0)
            playhead.store (target, std::memory_order_release);
    }

    void advance (int numSamples) noexcept
    {
        playhead.store (playhead.load (std::memory_order_relaxed) + numSamples, std::memory_order_release);
    }

    /** Loop wrap: an immediate move, inside the block (not deferred like locate). */
    void wrapTo (int64_t position) noexcept { playhead.store (position, std::memory_order_release); }

    void consumeCountIn (int numSamples) noexcept
    {
        const auto left = countInRemaining.load (std::memory_order_relaxed) - numSamples;
        countInRemaining.store (left > 0 ? left : 0, std::memory_order_release);
    }

    void setAudioRunning (bool running) noexcept { audioRunning.store (running, std::memory_order_release); }

private:
    std::atomic<State> state { State::stopped };
    std::atomic<int64_t> playhead { 0 };
    std::atomic<int64_t> playStart { 0 };
    std::atomic<int64_t> pendingLocate { -1 };
    std::atomic<bool> audioRunning { false };
    std::atomic<bool> loopEnabled { false };
    std::atomic<int64_t> loopStart { 0 }, loopEnd { 0 };
    std::atomic<int64_t> countInRemaining { 0 }, countInLength { 0 };
};

} // namespace meridian
