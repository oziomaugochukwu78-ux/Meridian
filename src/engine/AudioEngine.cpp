#include "AudioEngine.h"

#include <dsp/MixKernels.h>
#include <dsp/PanLaw.h>

#include <algorithm>
#include <cmath>

namespace meridian
{
namespace
{
constexpr double faderRampSeconds = 0.030;  // fader moves and SET_VOLUME commands
constexpr double muteRampSeconds = 0.005;   // mute/solo: fast, but never a click
constexpr double panRampSeconds = 0.020;
constexpr double silenceDb = -144.0;
constexpr double halfPi = 1.57079632679489661923;

double decibelsToGain (double db) noexcept
{
    return db <= silenceDb ? 0.0 : std::pow (10.0, db / 20.0);
}

int rampLength (double seconds, double sampleRate) noexcept
{
    return juce::jmax (1, (int) (seconds * sampleRate));
}

void storeMax (std::atomic<float>& slot, float value) noexcept
{
    auto current = slot.load (std::memory_order_relaxed);
    while (value > current && ! slot.compare_exchange_weak (current, value))
    {
    }
}
} // namespace

AudioEngine::AudioEngine()
{
    // Retired graphs are freed on the message thread in small batches.
    startTimerHz (20);
}

AudioEngine::~AudioEngine()
{
    stopTimer();
    deleteAllGraphs();
}

void AudioEngine::deleteAllGraphs()
{
    // Only valid once the device is detached (no audio callbacks).
    delete pendingGraph.exchange (nullptr);
    delete activeGraph;
    activeGraph = nullptr;
    RenderGraph* retired = nullptr;
    while (retiredGraphs.pop (retired))
        delete retired;
}

void AudioEngine::publishGraph (std::unique_ptr<RenderGraph> graph)
{
    JUCE_ASSERT_MESSAGE_THREAD
    graphLatency.store (graph != nullptr ? graph->compensatedLatency : 0);
    // If the audio thread never picked up the previous pending graph, it is
    // ours again and can be deleted right here.
    delete pendingGraph.exchange (graph.release(), std::memory_order_acq_rel);
}

void AudioEngine::timerCallback()
{
    RenderGraph* retired = nullptr;
    while (retiredGraphs.pop (retired))
        delete retired;
}

std::pair<float, float> AudioEngine::collectMasterPeaks() noexcept
{
    return { masterPeakLeft.exchange (0.0f), masterPeakRight.exchange (0.0f) };
}

//==============================================================================
void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double sr = device->getCurrentSampleRate();
    const int blockSize = juce::jmax (1, device->getCurrentBufferSizeSamples());

    prepare (sr, blockSize);
    deviceRunning.store (true);
    transport.setAudioRunning (true);

    if (listener != nullptr)
        listener->engineAboutToStart (sr, blockSize);
}

void AudioEngine::prepare (double sr, int blockSize)
{
    blockSize = juce::jmax (1, blockSize);

    for (int ch = 0; ch < 2; ++ch)
    {
        busSum[ch].assign ((size_t) blockSize, 0.0);
        busComp[ch].assign ((size_t) blockSize, 0.0);
        masterOut[ch].assign ((size_t) blockSize, 0.0);
    }
    for (auto* v : { &faderGains, &muteGains, &totalGains, &sendGains, &panLeft, &panRight, &coefLeft, &coefRight, &busScratch })
        v->assign ((size_t) blockSize, 0.0);
    silence.assign ((size_t) blockSize, 0.0f);
    clickScratch.assign ((size_t) blockSize, 0.0f);
    for (auto* v : { &delayedMain[0], &delayedMain[1], &delayedSend[0], &delayedSend[1] })
        v->assign ((size_t) blockSize, 0.0f);

    metronome.prepare (sr);
    appliedClick = metronome.getParams();

    masterBus.prepare (sr, blockSize);
    masterFader.reset (decibelsToGain (masterVolumeDb.load()));

    sampleRate.store (sr);
    maxBlockSize.store (blockSize);
}

void AudioEngine::audioDeviceStopped()
{
    deviceRunning.store (false);
    transport.setAudioRunning (false);
}

//==============================================================================
void AudioEngine::adoptPendingGraph() noexcept
{
    if (pendingGraph.load (std::memory_order_relaxed) == nullptr)
        return;

    // The old graph must be handed back before the new one is taken; if the
    // return queue is full (the message thread is stalled) keep rendering the
    // current graph and try again next block.
    if (activeGraph != nullptr && ! retiredGraphs.push (activeGraph))
        return;

    activeGraph = pendingGraph.exchange (nullptr, std::memory_order_acq_rel);
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                                    float* const* outputs, int numOutputs,
                                                    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;
    const auto started = juce::Time::getHighResolutionTicks(); // QueryPerformanceCounter / clock_gettime: real-time safe
    renderBlock (inputs, numInputs, outputs, numOutputs, numSamples);

    // Callback time as a fraction of the buffer period, kept as a running maximum.
    const auto elapsed = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - started);
    const double load = numSamples > 0 ? elapsed * sampleRate.load (std::memory_order_relaxed) / numSamples : 0.0;
    auto worst = worstCallbackLoad.load (std::memory_order_relaxed);
    while (load > worst && ! worstCallbackLoad.compare_exchange_weak (worst, load, std::memory_order_relaxed)) {}
    if (load > 1.0)
        callbackOverruns.fetch_add (1, std::memory_order_relaxed);
}

void AudioEngine::renderOffline (float* const* outputs, int numOutputs, int numSamples) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    renderBlock (nullptr, 0, outputs, numOutputs, numSamples);
}

void AudioEngine::pushLiveMidi (int pitch, float velocity, bool noteOn, double timeSeconds) noexcept
{
    if (pitch < 0 || pitch > 127)
        return;
    const juce::SpinLock::ScopedLockType producers (liveMidiProducers); // only MIDI threads ever wait here
    liveMidiIn.push ({ pitch, juce::jlimit (0.0f, 1.0f, velocity), noteOn && velocity > 0.0f, timeSeconds });
}

void AudioEngine::drainLiveMidi (int numSamples) noexcept
{
    // A note that arrived during the previous block's time is placed at the same
    // point within this block: everything is one block late, evenly, never jittered.
    const double rate = sampleRate.load (std::memory_order_relaxed);
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const double windowStart = now - numSamples / rate;

    numBlockNotes = 0;
    LiveNote note;
    while (numBlockNotes < (int) blockNotes.size() && liveMidiIn.pop (note))
    {
        const auto offset = (int) juce::jlimit (0.0, (double) (numSamples - 1), std::floor ((note.time - windowStart) * rate));
        blockNotes[(size_t) numBlockNotes++] = { offset, note.pitch, note.velocity, note.on };
    }
    // Arrival order is time order; offsets are clamped, so they stay non-decreasing.
}

void AudioEngine::renderBlock (const float* const* inputs, int numInputs,
                               float* const* outputs, int numOutputs, int numSamples) noexcept
{
    adoptPendingGraph();
    drainLiveMidi (numSamples);

    int chunkLimit = (int) masterOut[0].size();
    if (activeGraph != nullptr && activeGraph->maxBlockSize > 0)
        chunkLimit = juce::jmin (chunkLimit, activeGraph->maxBlockSize);

    if (chunkLimit <= 0)
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputs[ch], numSamples);
        return;
    }

    // Devices may deliver blocks larger than they announced; render in chunks
    // that fit the preallocated scratch. Chunks also end exactly where a
    // count-in ends and where a loop wraps, so both are sample-accurate.
    for (int done = 0; done < numSamples;)
    {
        transport.applyPendingLocate();
        const auto state = transport.getState();
        const auto countIn = state != Transport::State::stopped ? transport.getCountInRemaining() : 0;
        const bool looping = state == Transport::State::playing && countIn == 0 && transport.isLoopEnabled();
        const auto loopEnd = transport.getLoopEnd();
        const auto position = transport.getPosition();

        int chunk = juce::jmin (chunkLimit, numSamples - done);
        if (countIn > 0)
            chunk = (int) std::min<juce::int64> (chunk, countIn);
        else if (looping && position < loopEnd)
            chunk = (int) std::min<juce::int64> (chunk, loopEnd - position);

        processChunk (inputs, numInputs, outputs, numOutputs, done, chunk, state, countIn > 0);

        if (countIn > 0)
            transport.consumeCountIn (chunk);
        else if (looping && position < loopEnd && position + chunk == loopEnd)
            transport.wrapTo (transport.getLoopStart());
        done += chunk;
    }
}

void AudioEngine::renderClick (int numSamples, juce::int64 gridPosition) noexcept
{
    dsp::Metronome::Params p;
    p.bpm = clickBpm.load (std::memory_order_relaxed);
    p.beatsPerBar = clickBeatsPerBar.load (std::memory_order_relaxed);
    p.levelDb = clickLevelDb.load (std::memory_order_relaxed);
    if (! juce::exactlyEqual (p.bpm, appliedClick.bpm) || p.beatsPerBar != appliedClick.beatsPerBar
        || ! juce::exactlyEqual (p.levelDb, appliedClick.levelDb))
    {
        metronome.setParams (p); // no allocation
        appliedClick = p;
    }

    juce::FloatVectorOperations::clear (clickScratch.data(), numSamples);
    metronome.render (clickScratch.data(), numSamples, gridPosition);
    for (auto& channel : masterOut)
        for (int i = 0; i < numSamples; ++i)
            channel[(size_t) i] += clickScratch[(size_t) i];
}

void AudioEngine::processChunk (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                                int offset, int numSamples, Transport::State transportState, bool countingIn) noexcept
{
    // During a count-in the tracks behave as if stopped (no recording, no
    // playback, input monitoring still on) and only the click sounds.
    BlockContext ctx { inputs, numInputs, offset, numSamples, transport.getPosition(),
                       countingIn ? Transport::State::stopped : transportState, false,
                       transportState == Transport::State::playing && ! countingIn && transport.isLoopEnabled(),
                       transport.getLoopStart(), transport.getLoopEnd() };
    const bool rolling = ctx.state != Transport::State::stopped;

    dsp::CompensatedChannel bus[2] = { { busSum[0].data(), busComp[0].data() },
                                       { busSum[1].data(), busComp[1].data() } };
    dsp::clear (bus[0], numSamples);
    dsp::clear (bus[1], numSamples);

    if (activeGraph != nullptr)
    {
        for (auto& b : activeGraph->buses)
            for (int ch = 0; ch < 2; ++ch)
                dsp::clear ({ b.sum[ch].data(), b.comp[ch].data() }, numSamples);

        for (const auto& t : activeGraph->tracks)
            ctx.anySolo = ctx.anySolo || t.state->soloed.load (std::memory_order_relaxed);

        // Tracks are ordered so that every bus's inputs are rendered before the bus.
        for (auto& t : activeGraph->tracks)
            renderTrack (t, ctx);
    }

    // Master: collapse the double-double bus, console, fader, device.
    dsp::resolve (bus[0], masterOut[0].data(), numSamples);
    dsp::resolve (bus[1], masterOut[1].data(), numSamples);

    masterBus.process (masterOut[0].data(), masterOut[1].data(), numSamples);

    masterFader.setTarget (decibelsToGain (masterVolumeDb.load (std::memory_order_relaxed)),
                           rampLength (faderRampSeconds, sampleRate.load (std::memory_order_relaxed)));
    if (masterFader.isRamping())
    {
        masterFader.render (faderGains.data(), numSamples);
        dsp::multiplyInPlace (masterOut[0].data(), faderGains.data(), numSamples);
        dsp::multiplyInPlace (masterOut[1].data(), faderGains.data(), numSamples);
    }
    else if (! juce::exactlyEqual (masterFader.getCurrent(), 1.0))
    {
        dsp::multiplyInPlace (masterOut[0].data(), masterFader.getCurrent(), numSamples);
        dsp::multiplyInPlace (masterOut[1].data(), masterFader.getCurrent(), numSamples);
    }

    // The click: after the master fader, so it is never in a bounce's mix level
    // and the fader does not change it. Count-in clicks run on their own grid.
    if (countingIn || (rolling && clickEnabled.load (std::memory_order_relaxed)))
        renderClick (numSamples, countingIn ? transport.getCountInLength() - transport.getCountInRemaining() : ctx.position);

    storeMax (masterPeakLeft, (float) dsp::peakMagnitude (masterOut[0].data(), numSamples));
    storeMax (masterPeakRight, (float) dsp::peakMagnitude (masterOut[1].data(), numSamples));

    for (int ch = 0; ch < numOutputs; ++ch)
    {
        if (outputs[ch] == nullptr)
            continue;
        if (ch < 2)
            dsp::convertToFloat (masterOut[ch].data(), outputs[ch] + offset, numSamples);
        else
            juce::FloatVectorOperations::clear (outputs[ch] + offset, numSamples);
    }

    if (rolling)
        transport.advance (numSamples);
}

//==============================================================================
void AudioEngine::renderTrack (TrackRender& t, const BlockContext& ctx) noexcept
{
    const int numChannels = t.numChannels;

    if (t.isAux)
    {
        // An aux track's source is its bus: the sum of everything routed or sent to it.
        auto& input = activeGraph->buses[(size_t) t.ownBus];
        for (int ch = 0; ch < numChannels; ++ch)
        {
            dsp::resolve ({ input.sum[ch].data(), input.comp[ch].data() }, busScratch.data(), ctx.numSamples);
            dsp::convertToFloat (busScratch.data(), t.buffer.getWritePointer (ch), ctx.numSamples);
        }
    }
    else if (t.instrument != nullptr)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            juce::FloatVectorOperations::clear (t.buffer.getWritePointer (ch), ctx.numSamples);
        renderInstrument (t, ctx);
    }
    else
    {
        for (int ch = 0; ch < numChannels; ++ch)
            juce::FloatVectorOperations::clear (t.buffer.getWritePointer (ch), ctx.numSamples);

        // Auto-input monitoring: an armed track listens to its input while
        // stopped or recording, and plays back its clips while just playing.
        const bool armed = t.state->recordArmed.load (std::memory_order_relaxed);
        const bool monitorInput = armed && ctx.state != Transport::State::playing && t.inputChannel >= 0;

        if (monitorInput)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                int source = t.inputChannel + ch;
                if (source >= ctx.numInputs)
                    source = t.inputChannel; // mono input feeding a stereo track
                if (source < ctx.numInputs && ctx.inputs[source] != nullptr)
                    juce::FloatVectorOperations::copy (t.buffer.getWritePointer (ch), ctx.inputs[source] + ctx.inputOffset, ctx.numSamples);
            }
        }

        renderSegments (t, ctx);

        if (ctx.state == Transport::State::recording && t.take != nullptr)
            writeTake (*t.take, ctx);
    }

    if (! t.inserts.empty())
    {
        juce::AudioBuffer<float> view (t.buffer.getArrayOfWritePointers(), numChannels, 0, ctx.numSamples);
        for (auto& slot : t.inserts)
        {
            if (slot != nullptr && slot->plugin != nullptr && ! slot->bypassed.load (std::memory_order_relaxed))
            {
                t.midi.clear();
                slot->plugin->processBlock (view, t.midi);
            }
        }
    }

    mixTrackIntoBus (t, ctx);
}

void AudioEngine::renderSegments (TrackRender& t, const BlockContext& ctx) noexcept
{
    const auto pos = ctx.position;
    const auto blockEnd = pos + ctx.numSamples;
    const bool rolling = ctx.state != Transport::State::stopped;
    const bool monitoringInput = t.state->recordArmed.load (std::memory_order_relaxed)
                                 && ctx.state != Transport::State::playing && t.inputChannel >= 0;

    for (auto& seg : t.segments)
    {
        const auto segEnd = seg.start + seg.length;

        // Loop playback: keep the audio just after the loop start loaded, so the
        // wrap back to it never waits for the disk.
        if (ctx.looping && ! offline)
        {
            const auto loopFrom = juce::jmax (ctx.loopStart, seg.start);
            if (loopFrom < segEnd && loopFrom < ctx.loopEnd)
                seg.stream->pin (seg.sourceOffset + (loopFrom - seg.start));
        }

        if (segEnd <= pos)
            continue;

        if (! rolling || monitoringInput || seg.start >= blockEnd)
        {
            // Not audible this block: keep its disk buffers positioned where
            // playback will need them, so starting the transport is instant.
            seg.stream->cue (seg.sourceOffset + juce::jlimit<juce::int64> (0, seg.length, pos - seg.start));
            continue;
        }

        const auto s = juce::jmax (pos, seg.start);
        const auto e = juce::jmin (blockEnd, segEnd);
        const int count = (int) (e - s);
        const int destOffset = (int) (s - pos);

        float* scratch[2] = { t.clipScratch.getWritePointer (0), t.clipScratch.getWritePointer (1) };
        const auto sourcePosition = seg.sourceOffset + (s - seg.start);
        if (offline)
            seg.stream->loadBlocking (sourcePosition, count); // a bounce may take longer, but never drops audio
        if (! seg.stream->read (sourcePosition, scratch, t.numChannels, count))
            diskUnderruns.fetch_add (1, std::memory_order_relaxed);

        // Equal-power fades, evaluated per sample only inside fade regions.
        const bool inFadeIn = seg.fadeIn > 0 && s < seg.clipStart + seg.fadeIn;
        const bool inFadeOut = seg.fadeOut > 0 && e > seg.clipEnd - seg.fadeOut;
        if (inFadeIn || inFadeOut)
        {
            for (int i = 0; i < count; ++i)
            {
                const auto time = s + i;
                double g = 1.0;
                const auto sinceStart = time - seg.clipStart;
                if (seg.fadeIn > 0 && sinceStart < seg.fadeIn)
                    g *= std::sin (halfPi * ((double) sinceStart + 0.5) / (double) seg.fadeIn);
                const auto untilEnd = seg.clipEnd - time;
                if (seg.fadeOut > 0 && untilEnd <= seg.fadeOut)
                    g *= std::sin (halfPi * ((double) untilEnd - 0.5) / (double) seg.fadeOut);

                for (int ch = 0; ch < t.numChannels; ++ch)
                    scratch[ch][i] *= (float) g;
            }
        }

        for (int ch = 0; ch < t.numChannels; ++ch)
            juce::FloatVectorOperations::addWithMultiply (t.buffer.getWritePointer (ch, destOffset), scratch[ch], seg.gain, count);
    }
}

void AudioEngine::renderInstrument (TrackRender& t, const BlockContext& ctx) noexcept
{
    auto& inst = *t.instrument;

    // prepare() and setParams() allocate nothing, so the audio thread applies them.
    // Both engines are always prepared, so switching a track's engine is immediate.
    const double rate = activeGraph->sampleRate > 0.0 ? activeGraph->sampleRate : sampleRate.load (std::memory_order_relaxed);
    if (! juce::exactlyEqual (inst.preparedRate, rate))
    {
        inst.synth.prepare (rate);
        inst.drums.prepare (rate);
        inst.preparedRate = rate;
        inst.appliedParamsVersion = 0;
    }

    float* left = t.buffer.getWritePointer (0);
    // (Instrument tracks are stereo; should one ever be mono, the right half goes to scratch.)
    float* right = t.numChannels > 1 ? t.buffer.getWritePointer (1) : t.clipScratch.getWritePointer (1);
    const int n = ctx.numSamples;

    // The same playback logic drives either engine (they share an interface).
    auto play = [&] (auto& engineObject, const auto& params)
    {
        if (inst.appliedParamsVersion != t.synthParamsVersion)
        {
            engineObject.setParams (params);
            inst.appliedParamsVersion = t.synthParamsVersion;
        }

        const bool rolling = ctx.state != Transport::State::stopped;
        if (! rolling)
        {
            // Stopped: let the clip's sounding notes release naturally, start no new ones.
            if (inst.nextPosition >= 0)
            {
                engineObject.allNotesOff();
                inst.nextPosition = -1;
            }
        }
        else if (inst.nextPosition != ctx.position)
        {
            // Started, or jumped (locate, loop): notes from before the jump must not hang.
            engineObject.allNotesOff();
        }

        // Clip events (while rolling) and live notes (record-armed tracks), merged
        // in time; the synth renders between them, so each acts on its exact sample.
        const auto pos = ctx.position, end = pos + n;
        auto it = rolling ? std::lower_bound (t.events.begin(), t.events.end(), pos,
                                              [] (const MidiEvent& e, juce::int64 time) { return e.time < time; })
                          : t.events.end();
        const bool live = t.state->recordArmed.load (std::memory_order_relaxed);
        const bool capture = live && ctx.state == Transport::State::recording;
        int liveIndex = 0;
        while (live && liveIndex < numBlockNotes && blockNotes[(size_t) liveIndex].offset < ctx.inputOffset)
            ++liveIndex;

        int done = 0;
        for (;;)
        {
            const int clipAt = it != t.events.end() && it->time < end ? (int) (it->time - pos) : n;
            const int liveAt = live && liveIndex < numBlockNotes && blockNotes[(size_t) liveIndex].offset < ctx.inputOffset + n
                                   ? blockNotes[(size_t) liveIndex].offset - ctx.inputOffset
                                   : n;
            const int at = std::min (clipAt, liveAt);
            if (at >= n)
                break;
            if (at > done)
            {
                engineObject.render (left + done, right + done, at - done);
                done = at;
            }

            if (clipAt <= liveAt)
            {
                if (it->on)
                    engineObject.noteOn (it->pitch, it->velocity);
                else
                    engineObject.noteOff (it->pitch);
                ++it;
            }
            else
            {
                const auto& note = blockNotes[(size_t) liveIndex++];
                if (note.on)
                    engineObject.noteOn (note.pitch, note.velocity);
                else
                    engineObject.noteOff (note.pitch);
                if (capture && ! recordedMidi.push ({ t.state->trackId, pos + at, note.pitch, note.velocity, note.on }))
                    recordedMidiOverflows.fetch_add (1, std::memory_order_relaxed);
            }
        }
        if (done < n)
            engineObject.render (left + done, right + done, n - done);

        inst.nextPosition = rolling ? end : -1;
    };

    if (t.drumKit)
        play (inst.drums, t.drumParams);
    else
        play (inst.synth, t.synthParams);
}

void AudioEngine::writeTake (RecordTake& take, const BlockContext& ctx) noexcept
{
    const float* channels[2] = { silence.data(), silence.data() };
    for (int ch = 0; ch < take.numChannels; ++ch)
    {
        const int source = take.inputChannel + ch;
        if (source < ctx.numInputs && ctx.inputs[source] != nullptr)
            channels[ch] = ctx.inputs[source] + ctx.inputOffset;
    }

    juce::int64 expected = -1;
    take.startSample.compare_exchange_strong (expected, ctx.position);

    if (! take.writer->write (channels, ctx.numSamples))
        take.overflowed.store (true, std::memory_order_relaxed);
    take.samplesWritten.fetch_add (ctx.numSamples, std::memory_order_relaxed);
}

void AudioEngine::mixTrackIntoBus (TrackRender& t, const BlockContext& ctx) noexcept
{
    auto& s = *t.state;
    const int n = ctx.numSamples;
    const double sr = sampleRate.load (std::memory_order_relaxed);
    const bool rolling = ctx.state != Transport::State::stopped;

    // Solo: an aux stays audible while anything feeding it is soloed ("implicit solo"),
    // otherwise soloing a vocal would also silence its reverb return.
    bool soloedHere = s.soloed.load (std::memory_order_relaxed);
    for (size_t i = 0; ! soloedHere && i < t.upstream.size(); ++i)
        soloedHere = t.upstream[i]->soloed.load (std::memory_order_relaxed);

    const bool silencedBySolo = ctx.anySolo && ! soloedHere;
    const double muteTarget = (s.muted.load (std::memory_order_relaxed) || silencedBySolo) ? 0.0 : 1.0;
    const double faderTarget = decibelsToGain (s.volumeDb.load (std::memory_order_relaxed));
    const double pan = s.pan.load (std::memory_order_relaxed);
    const auto panGains = t.numChannels == 1 ? dsp::monoPan (pan) : dsp::stereoBalance (pan);
    const bool useAutomation = rolling && s.automationRead.load (std::memory_order_relaxed) && ! t.volumeAutomation.isEmpty();

    if (! s.rampsInitialised)
    {
        s.faderRamp.reset (faderTarget);
        s.muteRamp.reset (muteTarget);
        s.panLeftRamp.reset (panGains.left);
        s.panRightRamp.reset (panGains.right);
        s.rampsInitialised = true;
    }

    s.muteRamp.setTarget (muteTarget, rampLength (muteRampSeconds, sr));
    s.panLeftRamp.setTarget (panGains.left, rampLength (panRampSeconds, sr));
    s.panRightRamp.setTarget (panGains.right, rampLength (panRampSeconds, sr));

    if (useAutomation)
    {
        t.volumeAutomation.render (ctx.position, faderGains.data(), n);              // dB, sample-accurate
        dsp::decibelsToGain (faderGains.data(), faderGains.data(), n, silenceDb);   // linear-in-dB, as drawn
        s.faderRamp.reset (faderGains[(size_t) (n - 1)]); // leaving Read mode ramps from here
    }
    else
    {
        s.faderRamp.setTarget (faderTarget, rampLength (faderRampSeconds, sr));
    }

    // The signal after the inserts; sends tap it before the output's compensation delay.
    const float* sourceLeft = t.buffer.getReadPointer (0);
    const float* sourceRight = t.numChannels > 1 ? t.buffer.getReadPointer (1) : sourceLeft;

    // Plug-in delay compensation: hold this path back so it lines up with the
    // slowest path into the same bus. Runs every block, even when muted, so the
    // delay line always holds current audio.
    auto compensate = [&] (const PathDelay& delay, std::vector<float>* scratch, const float*& l, const float*& r)
    {
        l = sourceLeft;
        r = sourceRight;
        if (delay.line == nullptr || delay.samples <= 0)
            return;
        const float* in[2] = { sourceLeft, sourceRight };
        float* out[2] = { scratch[0].data(), scratch[1].data() };
        delay.line->process (in, out, t.numChannels, n, delay.samples);
        l = out[0];
        r = t.numChannels > 1 ? out[1] : out[0];
    };

    const float* left = nullptr;
    const float* right = nullptr;
    compensate (t.outputCompensation, delayedMain, left, right);

    // Destination: the master bus, or the input of the aux this track is routed to.
    dsp::CompensatedChannel bus[2] = { { busSum[0].data(), busComp[0].data() },
                                       { busSum[1].data(), busComp[1].data() } };
    if (t.outputBus >= 0)
    {
        auto& target = activeGraph->buses[(size_t) t.outputBus];
        bus[0] = { target.sum[0].data(), target.comp[0].data() };
        bus[1] = { target.sum[1].data(), target.comp[1].data() };
    }

    const bool steady = ! useAutomation && ! s.faderRamp.isRamping() && ! s.muteRamp.isRamping()
                        && ! s.panLeftRamp.isRamping() && ! s.panRightRamp.isRamping();

    if (steady && t.sends.empty())
    {
        const double gain = s.faderRamp.getCurrent() * s.muteRamp.getCurrent();
        if (juce::exactlyEqual (gain, 0.0))
            return; // muted or fader off: nothing to add, meters read silence

        const double gl = gain * s.panLeftRamp.getCurrent();
        const double gr = gain * s.panRightRamp.getCurrent();
        dsp::accumulateScaled (left, gl, bus[0], n);
        dsp::accumulateScaled (right, gr, bus[1], n);

        if (t.numChannels == 1)
        {
            const auto peak = (float) (dsp::peakMagnitude (left, n) * gain);
            s.publishPeaks (peak, peak);
        }
        else
        {
            s.publishPeaks ((float) (dsp::peakMagnitude (left, n) * gl), (float) (dsp::peakMagnitude (right, n) * gr));
        }
        return;
    }

    // General path (something is moving, or there are sends): per-sample coefficients.
    if (! useAutomation)
        s.faderRamp.render (faderGains.data(), n);
    s.muteRamp.render (muteGains.data(), n);
    std::copy (faderGains.begin(), faderGains.begin() + n, totalGains.begin());
    dsp::multiplyInPlace (totalGains.data(), muteGains.data(), n); // fader x mute/solo: post-fader gain

    s.panLeftRamp.render (panLeft.data(), n);
    s.panRightRamp.render (panRight.data(), n);
    std::copy (totalGains.begin(), totalGains.begin() + n, coefLeft.begin());
    std::copy (totalGains.begin(), totalGains.begin() + n, coefRight.begin());
    dsp::multiplyInPlace (coefLeft.data(), panLeft.data(), n);
    dsp::multiplyInPlace (coefRight.data(), panRight.data(), n);

    dsp::accumulateScaled (left, coefLeft.data(), bus[0], n);
    dsp::accumulateScaled (right, coefRight.data(), bus[1], n);

    if (t.numChannels == 1)
    {
        const auto peak = (float) dsp::peakMagnitudeScaled (left, totalGains.data(), n);
        s.publishPeaks (peak, peak);
    }
    else
    {
        s.publishPeaks ((float) dsp::peakMagnitudeScaled (left, coefLeft.data(), n),
                        (float) dsp::peakMagnitudeScaled (right, coefRight.data(), n));
    }

    // Sends: post-fader sends follow fader and mute; pre-fader sends follow only
    // mute/solo. Each has its own ramped level and constant-power pan.
    for (auto& send : t.sends)
    {
        auto& ss = *send.state;
        const double level = ss.muted.load (std::memory_order_relaxed)
                                 ? 0.0
                                 : decibelsToGain (ss.levelDb.load (std::memory_order_relaxed));
        const double sendPan = ss.pan.load (std::memory_order_relaxed);
        const auto sendPanGains = t.numChannels == 1 ? dsp::monoPan (sendPan) : dsp::stereoBalance (sendPan);

        if (! ss.rampsInitialised)
        {
            ss.levelRamp.reset (level);
            ss.panLeftRamp.reset (sendPanGains.left);
            ss.panRightRamp.reset (sendPanGains.right);
            ss.rampsInitialised = true;
        }
        ss.levelRamp.setTarget (level, rampLength (faderRampSeconds, sr));
        ss.panLeftRamp.setTarget (sendPanGains.left, rampLength (panRampSeconds, sr));
        ss.panRightRamp.setTarget (sendPanGains.right, rampLength (panRampSeconds, sr));

        ss.levelRamp.render (sendGains.data(), n);
        dsp::multiplyInPlace (sendGains.data(), ss.preFader.load (std::memory_order_relaxed) ? muteGains.data()
                                                                                             : totalGains.data(), n);
        ss.panLeftRamp.render (panLeft.data(), n);
        ss.panRightRamp.render (panRight.data(), n);
        std::copy (sendGains.begin(), sendGains.begin() + n, coefLeft.begin());
        std::copy (sendGains.begin(), sendGains.begin() + n, coefRight.begin());
        dsp::multiplyInPlace (coefLeft.data(), panLeft.data(), n);
        dsp::multiplyInPlace (coefRight.data(), panRight.data(), n);

        const float* sendLeft = nullptr;
        const float* sendRight = nullptr;
        compensate (send.compensation, delayedSend, sendLeft, sendRight);

        auto& target = activeGraph->buses[(size_t) send.targetBus];
        dsp::accumulateScaled (sendLeft, coefLeft.data(), { target.sum[0].data(), target.comp[0].data() }, n);
        dsp::accumulateScaled (sendRight, coefRight.data(), { target.sum[1].data(), target.comp[1].data() }, n);
    }
}

} // namespace meridian
