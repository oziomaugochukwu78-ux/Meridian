#include "StripSilence.h"

#include <dsp/SilenceDetector.h>

namespace meridian::stripsilence
{

Result apply (Session& session, juce::AudioFormatManager& formats, juce::ValueTree t, juce::int64 start, juce::int64 end,
              const Params& params, juce::UndoManager* um)
{
    Result result;
    if (! t.isValid() || end <= start)
        return result;

    const double rate = session.getSampleRate();
    auto samples = [rate] (double seconds) { return (juce::int64) std::llround (std::max (0.0, seconds) * rate); };
    const auto threshold = (float) juce::Decibels::decibelsToGain (params.thresholdDb, -200.0);

    // Copied first: the edits below change the list.
    juce::Array<juce::ValueTree> targets;
    for (const auto& c : track::clips (t))
        if (! clip::isMidi (c) && clip::end (c) > start && clip::start (c) < end)
            targets.add (c);

    auto list = track::clips (t);
    for (auto c : targets)
    {
        const auto from = std::max (start, clip::start (c)), to = std::min (end, clip::end (c));
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (session.resolveStoredPath (c[ids::file].toString())));
        if (reader == nullptr)
        {
            result.unreadable.add (clip::name (c));
            continue;
        }
        ++result.clipsAnalysed;

        // The detector sees the clip's audio over [from, to), at its clip gain.
        dsp::SilenceDetector detector (threshold, samples (params.minSilenceSeconds), samples (params.padBeforeSeconds),
                                       samples (params.padAfterSeconds));
        const int channels = (int) juce::jlimit (1u, 8u, reader->numChannels);
        juce::AudioBuffer<float> block (channels, 65536);
        const auto gain = (float) juce::Decibels::decibelsToGain (clip::gainDb (c), -200.0);
        for (auto at = from; at < to;)
        {
            const int count = (int) std::min<juce::int64> (block.getNumSamples(), to - at);
            block.clear();
            reader->read (&block, 0, count, clip::offset (c) + (at - clip::start (c)), true, true);
            detector.feed (block.getArrayOfReadPointers(), channels, count, gain);
            at += count;
        }

        // Cut the analysed span out of the clip, then put back one clip per audible region.
        juce::ValueTree middle = c;
        if (from > clip::start (middle))
            middle = session.splitClip (middle, from, um);
        if (to < clip::end (middle))
            session.splitClip (middle, to, um);
        const auto model = middle.createCopy();
        list.removeChild (middle, um);

        juce::int64 kept = 0;
        for (const auto& region : detector.regions())
        {
            auto piece = model.createCopy();
            piece.setProperty (ids::id, session.allocateId(), nullptr);
            piece.setProperty (ids::start, from + region.start, nullptr);
            piece.setProperty (ids::offset, clip::offset (model) + region.start, nullptr);
            piece.setProperty (ids::length, region.end - region.start, nullptr);
            // A region edge inside the span is a new edge: no fade there.
            if (region.start > 0)
                for (const auto& id : { ids::fadeIn, ids::fadeInShape, ids::crossfadeIn, ids::extendedIn })
                    piece.removeProperty (id, nullptr);
            if (from + region.end < to)
                for (const auto& id : { ids::fadeOut, ids::fadeOutShape, ids::extendedOut })
                    piece.removeProperty (id, nullptr);
            const auto length = region.end - region.start;
            piece.setProperty (ids::fadeIn, std::min (clip::fadeIn (piece), length), nullptr);
            piece.setProperty (ids::fadeOut, std::min (clip::fadeOut (piece), length - clip::fadeIn (piece)), nullptr);
            list.appendChild (piece, um);
            ++result.clipsMade;
            kept += region.end - region.start;
        }
        result.samplesRemoved += (to - from) - kept;
    }
    return result;
}

} // namespace meridian::stripsilence
