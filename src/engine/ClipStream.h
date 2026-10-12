#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <memory>

namespace meridian
{

/** Double-buffered disk stream for one audio file.

    The file is viewed as a grid of fixed-size "cells". The stream owns two
    buffers ("halves"), each holding one cell. While the audio thread plays
    from the cell under the playhead, the disk thread fills the other half
    with the next cell; when playback crosses into it, the old half is
    reassigned to the cell after that. Seeking simply reassigns both halves.

    Each half is controlled by one atomic 64-bit word packing a state and a
    cell index, so the hand-off is lock-free:

        Idle    --audio-->  Empty(cell)            (request a fill)
        Empty   --disk--->  Filling(cell)          (CAS; the disk thread owns the data)
        Filling --disk--->  Ready(cell)            (release: data is visible)
        Ready   --audio-->  Empty(other cell)      (reuse; the audio thread is done reading)

    The audio thread only ever reads a half's samples while it is Ready, and
    only the audio thread moves a half out of Ready. The disk thread only
    writes samples while it holds Filling. So the sample data never needs a
    lock: ownership is handed over by the state word with acquire/release
    ordering.
*/
class ClipStream
{
public:
    /** framesPerHalf is the cell size: the read-ahead window. */
    ClipStream (std::unique_ptr<juce::AudioFormatReader> reader, int framesPerHalf);

    int getNumChannels() const noexcept       { return numChannels; }
    juce::int64 getLength() const noexcept    { return length; }
    double getFileSampleRate() const noexcept { return fileSampleRate; }

    //==============================================================================
    // Audio thread

    /** Makes sure the cells around sourcePosition are loaded or being loaded.
        Cheap: a couple of atomic loads when nothing needs to change. */
    void cue (juce::int64 sourcePosition) noexcept;

    /** Copies numFrames starting at sourcePosition into dest (numDestChannels
        channels; mono/stereo are up/down-mixed as needed), then prefetches.
        Returns false if any frame was not yet loaded; those frames are silent. */
    bool read (juce::int64 sourcePosition, float* const* dest, int numDestChannels, int numFrames) noexcept;

    /** Offline renders only (never the audio thread): makes sure the cells
        covering [sourcePosition, sourcePosition + numFrames) are loaded,
        reading from disk on the calling thread. Only for streams that are NOT
        registered with the DiskStreamer, so the reader has a single user. */
    void loadBlocking (juce::int64 sourcePosition, int numFrames);

    //==============================================================================
    // Disk thread

    /** Fills any half that has been requested. Returns true if it did any work. */
    bool service();

    std::atomic<int> underruns { 0 };

private:
    enum class HalfState : juce::uint64 { idle = 0, empty = 1, filling = 2, ready = 3 };

    static constexpr int stateShift = 62;
    static constexpr juce::uint64 cellMask = (juce::uint64 (1) << stateShift) - 1;

    static juce::uint64 pack (HalfState s, juce::int64 cell) noexcept
    {
        return (static_cast<juce::uint64> (s) << stateShift) | (static_cast<juce::uint64> (cell) & cellMask);
    }
    static HalfState stateOf (juce::uint64 word) noexcept { return static_cast<HalfState> (word >> stateShift); }
    static juce::int64 cellOf (juce::uint64 word) noexcept { return static_cast<juce::int64> (word & cellMask); }

    struct Half
    {
        std::atomic<juce::uint64> word { pack (HalfState::idle, 0) };
        juce::AudioBuffer<float> data;
    };

    int findReadyHalf (juce::int64 cell) const noexcept;

    std::unique_ptr<juce::AudioFormatReader> reader; // disk thread only after construction
    const int numChannels;
    const juce::int64 length;
    const double fileSampleRate;
    const int halfSize;
    Half halves[2];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipStream)
};

} // namespace meridian
