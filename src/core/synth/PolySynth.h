#pragma once

#include <array>
#include <cstdint>

namespace meridian::synth
{

/** Meridian's built-in instrument: a polyphonic subtractive synthesiser.

    Per voice:
      - two oscillators (the second detuned for width), band-limited with
        polyBLEP so high notes do not alias
      - a resonant low-pass filter (topology-preserving state-variable
        filter) with its own envelope
      - an ADSR amplitude envelope

    Voices run at twice the output rate and are filtered back down with a
    65-tap linear-phase low-pass (Kaiser window): harmonics between 24 and 48
    kHz are removed instead of folding back as inharmonic tones. The filter
    delays the output by getLatencySamples() (16 samples), which the engine
    compensates like plug-in latency.

    Real-time safe: a fixed pool of voices, no allocation, no locks. When all
    voices are busy, the quietest releasing voice (or else the oldest) is reused.

    The caller splits blocks at event times: render up to an event, apply
    noteOn/noteOff, then render the rest. That keeps every note sample-accurate. */
class PolySynth
{
public:
    enum class Waveform { sine, saw, square };

    struct Params
    {
        Waveform waveform = Waveform::saw;
        double detuneCents = 7.0;     ///< second oscillator, 0..50
        double attack = 0.005;        ///< seconds, linear rise to full level
        double decay = 0.25;          ///< seconds to fall 60 dB toward the sustain level
        double sustain = 0.7;         ///< level, 0..1
        double release = 0.3;         ///< seconds to fall 60 dB after note-off
        double cutoffHz = 2500.0;     ///< filter cutoff at rest, 20..20000
        double resonance = 0.2;       ///< 0..1 (1 = close to self-oscillation)
        double filterEnvOctaves = 2.0;///< how far the envelope opens the filter
        double gainDb = -12.0;        ///< per-voice output level
    };

    static constexpr int maxVoices = 32;
    static constexpr int oversampling = 2;
    static constexpr int decimatorTaps = 65;

    /** Fixed output delay of the decimation filter, in output samples. */
    static constexpr int getLatencySamples() noexcept { return (decimatorTaps - 1) / 2 / oversampling; }

    void prepare (double sampleRate) noexcept;
    void setParams (const Params&) noexcept;
    const Params& getParams() const noexcept { return params; }

    void noteOn (int pitch, float velocity) noexcept;
    void noteOff (int pitch) noexcept;
    void allNotesOff() noexcept;      ///< release every voice, and lift the sustain pedal
    void reset() noexcept;            ///< silence immediately

    /** The sustain pedal (MIDI CC 64): while down, released keys keep sounding at
        their sustain level; lifting it releases every note whose key is up. */
    void setSustain (bool down) noexcept;
    bool isSustainDown() const noexcept { return pedalDown; }

    /** Adds the synth's output into left/right (they are not cleared first). */
    void render (float* left, float* right, int numSamples) noexcept;

    int getNumActiveVoices() const noexcept;

private:
    struct Voice
    {
        bool active = false, releasing = false;
        bool sustained = false; // key up, held by the pedal
        int pitch = -1;
        float velocity = 0.0f;
        uint64_t age = 0;
        double phase1 = 0.0, phase2 = 0.0, inc1 = 0.0, inc2 = 0.0;
        double env = 0.0, filterEnv = 0.0;
        int stage = 0; // 0 attack, 1 decay/sustain, 2 release
        double ic1 = 0.0, ic2 = 0.0; // filter state
    };

    double oscillator (double phase, double increment) const noexcept;
    void startVoice (Voice&, int pitch, float velocity) noexcept;
    void renderVoices (double* out, int numInternalSamples) noexcept;

    static constexpr int chunk = 256; // output samples per internal pass

    Params params;
    double sampleRate = 48000.0, internalRate = 96000.0;
    double voiceGain = 0.25;
    double attackStep = 0.0, decayCoef = 0.0, releaseCoef = 0.0;
    std::array<Voice, maxVoices> voices {};
    uint64_t noteCounter = 0;
    bool pedalDown = false;

    std::array<double, decimatorTaps> fir {};
    std::array<double, 2 * decimatorTaps> history {}; // doubled ring: contiguous windows
    int historyPos = 0;
    std::array<double, chunk * oversampling> internal {};
};

} // namespace meridian::synth
