#include "ClipStream.h"

namespace meridian
{

ClipStream::ClipStream (std::unique_ptr<juce::AudioFormatReader> r, int framesPerHalf)
    : reader (std::move (r)),
      numChannels (juce::jlimit (1, 2, (int) reader->numChannels)),
      length (reader->lengthInSamples),
      fileSampleRate (reader->sampleRate),
      halfSize (juce::jmax (1024, framesPerHalf))
{
    for (auto& h : halves)
        h.data.setSize (numChannels, halfSize);
    pinned.data.setSize (numChannels, halfSize);
}

void ClipStream::pin (juce::int64 sourcePosition) noexcept
{
    if (length <= 0)
        return;
    const auto start = juce::jlimit<juce::int64> (0, length - 1, sourcePosition);
    auto w = pinned.word.load (std::memory_order_acquire);
    const auto state = stateOf (w);
    if (state != HalfState::idle && cellOf (w) == start)
        return; // already loaded or on its way
    if (state == HalfState::filling)
        return; // the disk thread owns it; asked again next block

    const auto request = pack (HalfState::empty, start);
    if (state == HalfState::ready)
        pinned.word.store (request, std::memory_order_release); // only this thread moves it out of Ready
    else
        pinned.word.compare_exchange_strong (w, request, std::memory_order_acq_rel);
}

bool ClipStream::isPinnedReady (juce::int64 sourcePosition) const noexcept
{
    const auto w = pinned.word.load (std::memory_order_acquire);
    return stateOf (w) == HalfState::ready && cellOf (w) == juce::jlimit<juce::int64> (0, juce::jmax<juce::int64> (0, length - 1), sourcePosition);
}

void ClipStream::fill (juce::AudioFormatReader& source, Half& h, juce::int64 startFrame, juce::int64 fileLength,
                       int channels, int size)
{
    const int frames = (int) juce::jlimit<juce::int64> (0, size, fileLength - startFrame);
    if (frames > 0)
        source.read (h.data.getArrayOfWritePointers(), channels, startFrame, frames);
    if (frames < size)
        h.data.clear (frames, size - frames);
}

int ClipStream::findReadyHalf (juce::int64 cell) const noexcept
{
    for (int h = 0; h < 2; ++h)
    {
        const auto w = halves[h].word.load (std::memory_order_acquire);
        if (stateOf (w) == HalfState::ready && cellOf (w) == cell)
            return h;
    }
    return -1;
}

void ClipStream::cue (juce::int64 sourcePosition) noexcept
{
    const juce::int64 current = std::max<juce::int64> (0, sourcePosition) / halfSize;
    const juce::int64 lastCell = length > 0 ? (length - 1) / halfSize : -1;

    juce::uint64 words[2] = { halves[0].word.load (std::memory_order_acquire),
                              halves[1].word.load (std::memory_order_acquire) };

    // Cells past the end of the file need no loading: treat them as covered.
    bool haveCurrent = current > lastCell;
    bool haveNext = current + 1 > lastCell;

    for (const auto w : words)
    {
        if (stateOf (w) == HalfState::idle)
            continue;
        if (cellOf (w) == current)      haveCurrent = true;
        if (cellOf (w) == current + 1)  haveNext = true;
    }

    for (int h = 0; h < 2 && ! (haveCurrent && haveNext); ++h)
    {
        const auto state = stateOf (words[h]);
        const auto cell = cellOf (words[h]);

        if (state == HalfState::filling)
            continue; // the disk thread owns it; re-evaluated once it is Ready
        if (state != HalfState::idle && (cell == current || cell == current + 1))
            continue; // already holds a cell we want

        const juce::int64 wanted = haveCurrent ? current + 1 : current;
        const auto request = pack (HalfState::empty, wanted);

        if (state == HalfState::ready)
        {
            // Only this thread moves a half out of Ready, so a plain store is safe.
            halves[h].word.store (request, std::memory_order_release);
        }
        else if (! halves[h].word.compare_exchange_strong (words[h], request, std::memory_order_acq_rel))
        {
            continue; // the disk thread just claimed it
        }

        (wanted == current ? haveCurrent : haveNext) = true;
    }
}

bool ClipStream::read (juce::int64 sourcePosition, float* const* dest, int numDestChannels, int numFrames) noexcept
{
    bool complete = true;
    int done = 0;

    while (done < numFrames)
    {
        const juce::int64 frame = sourcePosition + done;
        int count = numFrames - done;
        const juce::AudioBuffer<float>* source = nullptr;
        int offsetInCell = 0;

        if (frame >= 0 && frame < length)
        {
            const juce::int64 cell = frame / halfSize;
            offsetInCell = (int) (frame - cell * halfSize);
            count = (int) std::min<juce::int64> ({ count, halfSize - offsetInCell, length - frame });
            if (const int half = findReadyHalf (cell); half >= 0)
            {
                source = &halves[half].data;
            }
            else
            {
                // Not streamed yet (just after a loop wrap): the pinned buffer may have it.
                const auto pw = pinned.word.load (std::memory_order_acquire);
                const auto pinStart = cellOf (pw);
                if (stateOf (pw) == HalfState::ready && frame >= pinStart && frame < pinStart + halfSize)
                {
                    source = &pinned.data;
                    offsetInCell = (int) (frame - pinStart);
                    count = (int) std::min<juce::int64> (count, pinStart + halfSize - frame);
                }
                else
                {
                    complete = false;
                }
            }
        }
        else if (frame < 0)
        {
            count = (int) std::min<juce::int64> (count, -frame);
        }

        if (source == nullptr)
        {
            for (int ch = 0; ch < numDestChannels; ++ch)
                juce::FloatVectorOperations::clear (dest[ch] + done, count);
        }
        else
        {
            const auto& data = *source;
            const float* left = data.getReadPointer (0, offsetInCell);
            const float* right = numChannels > 1 ? data.getReadPointer (1, offsetInCell) : left;

            if (numDestChannels == 1 && numChannels == 2)
            {
                juce::FloatVectorOperations::copyWithMultiply (dest[0] + done, left, 0.5f, count);
                juce::FloatVectorOperations::addWithMultiply (dest[0] + done, right, 0.5f, count);
            }
            else
            {
                juce::FloatVectorOperations::copy (dest[0] + done, left, count);
                if (numDestChannels > 1)
                    juce::FloatVectorOperations::copy (dest[1] + done, right, count);
            }
        }

        done += count;
    }

    if (! complete)
        underruns.fetch_add (1, std::memory_order_relaxed);

    cue (sourcePosition + numFrames);
    return complete;
}

void ClipStream::loadBlocking (juce::int64 sourcePosition, int numFrames)
{
    const auto first = std::max<juce::int64> (0, sourcePosition);
    const auto last = juce::jmin (length - 1, sourcePosition + numFrames - 1);
    if (last < first)
        return;

    for (int attempt = 0; attempt < 1000; ++attempt)
    {
        cue (first);
        if (findReadyHalf (first / halfSize) >= 0 && findReadyHalf (last / halfSize) >= 0)
            return;

        // The calling thread is this stream's only disk reader: offline graphs
        // build their own streams and never register them with the disk thread
        // (AudioFormatReader is not thread-safe).
        if (! service())
            juce::Thread::sleep (1);
    }
}

bool ClipStream::service()
{
    bool didWork = false;

    for (auto& h : halves)
    {
        auto w = h.word.load (std::memory_order_acquire);
        if (stateOf (w) != HalfState::empty)
            continue;

        const auto cell = cellOf (w);
        if (! h.word.compare_exchange_strong (w, pack (HalfState::filling, cell), std::memory_order_acq_rel))
            continue;

        fill (*reader, h, cell * halfSize, length, numChannels, halfSize);
        h.word.store (pack (HalfState::ready, cell), std::memory_order_release);
        didWork = true;
    }

    // The pinned loop-start buffer: its word holds a start frame, not a cell.
    auto pw = pinned.word.load (std::memory_order_acquire);
    if (stateOf (pw) == HalfState::empty)
    {
        const auto start = cellOf (pw);
        if (pinned.word.compare_exchange_strong (pw, pack (HalfState::filling, start), std::memory_order_acq_rel))
        {
            fill (*reader, pinned, start, length, numChannels, halfSize);
            pinned.word.store (pack (HalfState::ready, start), std::memory_order_release);
            didWork = true;
        }
    }

    return didWork;
}

} // namespace meridian
