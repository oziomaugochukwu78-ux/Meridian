#pragma once

#include "ClipStream.h"

#include <juce_core/juce_core.h>

#include <memory>
#include <vector>

namespace meridian
{

/** The disk thread: services every active ClipStream, reading audio files
    ahead of the playhead so the audio thread never waits on storage. */
class DiskStreamer final : private juce::Thread
{
public:
    DiskStreamer();
    ~DiskStreamer() override;

    /** Message thread: replaces the set of streams to service. */
    void setStreams (std::vector<std::shared_ptr<ClipStream>> newStreams);

private:
    void run() override;

    juce::CriticalSection lock; // guards `streams`; never taken by the audio thread
    std::vector<std::shared_ptr<ClipStream>> streams;
    std::atomic<juce::uint32> generation { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DiskStreamer)
};

} // namespace meridian
