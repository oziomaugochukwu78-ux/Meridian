#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace meridian::synth
{

/** One recorded sound, in memory: mono or stereo, at its own sample rate. */
struct Sample
{
    std::vector<float> left, right;   ///< right empty = mono
    double sampleRate = 48000.0;

    int64_t length() const noexcept { return (int64_t) left.size(); }
};

/** Which sample a key plays: notes lowNote..highNote play `sample`, at its
    recorded pitch on rootNote and transposed from there. */
struct Zone
{
    std::shared_ptr<const Sample> sample;
    int rootNote = 60, lowNote = 0, highNote = 127;
    double gainDb = 0.0;
};

/** The zones of one instrument. Built off the audio thread and never changed
    after; the sampler only reads it. */
struct ZoneMap
{
    std::vector<Zone> zones;

    /** The zone for a note: the first whose range holds it, or null. */
    const Zone* find (int note) const noexcept;
};

/** Meridian's built-in sampler: plays recorded sounds from MIDI.

    Two modes:
      - one-shot (drums): every hit plays to the end of its sample; note-offs
        are ignored; the closed and pedal hi-hats (42, 44) cut off a
        ringing open hat (46), as on a real kit
      - keys: notes sound while held and fade over `release` after note-off
        (the sustain pedal holds them)

    Transposition is by playback speed (as on classic samplers: a sound an
    octave up is twice as fast), read with 4-point Hermite interpolation; a
    sample at another rate than the session's is played at its own speed.
    Velocity scales the level along a gentle curve (velocity^1.5).

    Real-time safe: a fixed voice pool, no allocation, no locks. The zone map
    is owned elsewhere and must outlive its use; setZones() with a different
    map silences the voices playing the old one. The caller splits blocks at
    events, as for PolySynth, so every hit is sample-accurate. Latency: none. */
class Sampler
{
public:
    struct Params
    {
        bool oneShot = true;          ///< drums; false = keys
        double attack = 0.0;          ///< seconds, linear rise
        double release = 0.25;        ///< seconds to fall 60 dB after note-off (keys)
        double tuneSemitones = 0.0;   ///< -24..+24
        double gainDb = -6.0;         ///< output level
    };

    static constexpr int maxVoices = 48;
    static constexpr int getLatencySamples() noexcept { return 0; }
    static constexpr double bendRangeSemitones = 2.0;

    void prepare (double sampleRate) noexcept;
    void setParams (const Params&) noexcept;
    const Params& getParams() const noexcept { return params; }

    /** The map the keys play. Null = silent. */
    void setZones (const ZoneMap*) noexcept;

    void noteOn (int pitch, float velocity) noexcept;
    void noteOff (int pitch) noexcept;
    void allNotesOff() noexcept;      ///< keys: release every voice; one-shots ring on
    void reset() noexcept;            ///< silence immediately
    void setSustain (bool down) noexcept;
    void setPitchBend (float amount) noexcept;   ///< -1..+1, up to bendRangeSemitones (keys)
    void setModWheel (float) noexcept {}

    /** Adds the output into left/right. */
    void render (float* left, float* right, int numSamples) noexcept;

    int getNumActiveVoices() const noexcept;

private:
    struct Voice
    {
        const Sample* sample = nullptr;
        int note = -1;
        double position = 0.0;     ///< in the sample's own samples
        double speed = 1.0;        ///< before the pitch bend
        float gain = 0.0f;
        double envelope = 0.0;
        double attackStep = 1.0;
        bool held = false;         ///< key down (or pedal holding it)
        bool keyDown = false;
        bool releasing = false;
        bool choking = false;      ///< a fast fade (hi-hat choke, voice stolen)
        uint64_t age = 0;
    };

    void startRelease (Voice&, bool fast) noexcept;
    double releaseFactor (bool fast) const noexcept;

    Params params;
    const ZoneMap* zones = nullptr;
    double sampleRate = 48000.0;
    std::array<Voice, maxVoices> voices {};
    uint64_t counter = 0;
    bool pedalDown = false;
    double bendFactor = 1.0;
};

} // namespace meridian::synth
