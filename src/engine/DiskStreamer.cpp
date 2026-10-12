#include "DiskStreamer.h"

namespace meridian
{

DiskStreamer::DiskStreamer() : juce::Thread ("Disk Streamer")
{
    startThread (juce::Thread::Priority::high);
}

DiskStreamer::~DiskStreamer()
{
    stopThread (4000);
}

void DiskStreamer::setStreams (std::vector<std::shared_ptr<ClipStream>> newStreams)
{
    {
        const juce::ScopedLock sl (lock);
        streams.swap (newStreams);
    }
    generation.fetch_add (1, std::memory_order_release);
    notify();
    // newStreams now holds the previous set and is released here, on the
    // message thread, unless the disk thread still holds a reference.
}

void DiskStreamer::run()
{
    std::vector<std::shared_ptr<ClipStream>> local;
    juce::uint32 seenGeneration = generation.load() - 1;

    while (! threadShouldExit())
    {
        const auto currentGeneration = generation.load (std::memory_order_acquire);
        if (currentGeneration != seenGeneration)
        {
            const juce::ScopedLock sl (lock);
            local = streams;
            seenGeneration = currentGeneration;
        }

        bool didWork = false;
        for (auto& s : local)
            didWork = s->service() || didWork;

        // Idle: poll again shortly. The audio thread never signals this thread
        // (signalling can take a lock), so a short sleep bounds fill latency.
        if (! didWork)
            wait (2);
    }
}

} // namespace meridian
