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
        returns there, the classic "return to start" transport behaviour. */
    void play (State newState = State::playing) noexcept
    {
        if (getState() == State::stopped)
            playStart.store (getPosition(), std::memory_order_relaxed);
        state.store (newState, std::memory_order_release);
    }

    void stop() noexcept
    {
        const bool wasRolling = isRolling();
        state.store (State::stopped, std::memory_order_release);
        if (wasRolling)
            locate (playStart.load (std::memory_order_relaxed));
    }

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

    void setAudioRunning (bool running) noexcept { audioRunning.store (running, std::memory_order_release); }

private:
    std::atomic<State> state { State::stopped };
    std::atomic<int64_t> playhead { 0 };
    std::atomic<int64_t> playStart { 0 };
    std::atomic<int64_t> pendingLocate { -1 };
    std::atomic<bool> audioRunning { false };
};

} // namespace meridian
