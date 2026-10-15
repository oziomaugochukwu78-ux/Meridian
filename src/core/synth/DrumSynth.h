#pragma once

#include <array>
#include <cstdint>

namespace meridian::synth
{

/** Meridian's built-in drum machine: every sound synthesised (no samples), in
    the manner of classic analog drum machines, on the General MIDI drum map.

        35, 36  kick           37      side stick (rim)   38, 40  snare
        39      clap           42      closed hi-hat      44      pedal hi-hat
        46      open hi-hat    41, 43  low tom            45, 47  mid tom
        48, 50  high tom       49, 52, 55, 57  crash      51, 53, 59  ride
        56      cowbell        (other notes are silent)

    Sounds are one-shots: note-off is ignored. A closed or pedal hi-hat chokes
    (quickly silences) a ringing open hi-hat, as on a real kit. Noise comes
    from per-voice deterministic generators, so a render is identical every
    time (the golden-file tests rely on it).

    Same interface as PolySynth, so the engine drives either the same way;
    real-time safe (fixed voice pool, no allocation); no latency. */
class DrumSynth
{
public:
    struct Params
    {
        double tuneSemitones = 0.0; ///< -12..+12: pitch of every drum
        double decayScale = 1.0;    ///< 0.25..4: shorter / longer tails
        double tone = 0.5;          ///< 0 (dark) .. 1 (bright): noise and click brightness
        double gainDb = -6.0;       ///< kit output level, -60..+12
    };

    enum class Sound { none, kick, rim, snare, clap, closedHat, pedalHat, openHat,
                       lowTom, midTom, highTom, crash, ride, cowbell };

    static constexpr int maxVoices = 24;
    static constexpr int getLatencySamples() noexcept { return 0; }

    static Sound soundForNote (int pitch) noexcept;
    static const char* nameForNote (int pitch) noexcept; ///< "Kick", ... or nullptr if unmapped

    void prepare (double sampleRate) noexcept;
    void setParams (const Params&) noexcept;
    const Params& getParams() const noexcept { return params; }

    void noteOn (int pitch, float velocity) noexcept;
    void noteOff (int) noexcept {}       ///< one-shots ring out
    void allNotesOff() noexcept {}       ///< (tails ring out on stop as on a hardware machine)
    void setSustain (bool) noexcept {}   ///< one-shots: the pedal changes nothing
    void setPitchBend (float) noexcept {} ///< drums are not bent (tune them with the tune control)
    void setModWheel (float) noexcept {}
    void reset() noexcept;

    /** Adds the kit's output into left/right. */
    void render (float* left, float* right, int numSamples) noexcept;

    int getNumActiveVoices() const noexcept;

private:
    struct Filter // topology-preserving state-variable filter
    {
        double g = 0.0, k = 1.0, a1 = 0.0, ic1 = 0.0, ic2 = 0.0;
        void set (double cutoffHz, double q, double sampleRate) noexcept;
        void process (double x, double& low, double& band, double& high) noexcept;
    };

    struct Voice
    {
        Sound sound = Sound::none;
        bool active = false;
        uint64_t age = 0;
        double t = 0.0;              // seconds since the hit
        double amp = 0.0;            // velocity x kit gain
        double panL = 0.7071, panR = 0.7071;
        double phase[6] {};
        double freqs[6] {};
        double pitchStart = 0.0, pitchEnd = 0.0;
        double ampDecay = 0.1;       // seconds per time constant
        double choke = 1.0;          // multiplies the output; decays fast once choked
        bool choking = false;
        uint32_t noise = 1;
        Filter f1, f2;
    };

    double renderVoice (Voice&) noexcept; // one mono sample
    double nextNoise (Voice&) noexcept;
    double tuneFactor() const noexcept;

    Params params;
    double sampleRate = 48000.0, dt = 1.0 / 48000.0;
    double kitGain = 0.5;
    std::array<Voice, maxVoices> voices {};
    uint64_t hitCounter = 0;
};

} // namespace meridian::synth
