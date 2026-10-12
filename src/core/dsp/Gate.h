#pragma once

namespace meridian::dsp
{

/** A noise gate (stereo-linked).

    Opens when the louder channel's peak rises above `thresholdDb`, stays open
    while it is above `thresholdDb - hysteresisDb` (so a level hovering at the
    threshold cannot make it chatter), holds open for `holdMs` after that, then
    closes. Closed, the signal is turned down by `rangeDb`. The gain moves
    exponentially: open with `attackMs`, close with `releaseMs` (time
    constants, as in the compressor).

    Specification, checked by the unit tests:
      - above the threshold the gate passes audio at unity, bit for bit
      - closed, the level is exactly `rangeDb` lower
      - it stays open for exactly `holdMs` after the level drops
      - a level hovering within the hysteresis does not chatter

    Not allocating; process() is real-time safe. */
class Gate
{
public:
    struct Params
    {
        double thresholdDb = -40.0; ///< -80..0
        double rangeDb = -80.0;     ///< -80..0: attenuation when closed
        double attackMs = 0.5;      ///< 0.05..50
        double holdMs = 50.0;       ///< 0..2000
        double releaseMs = 100.0;   ///< 5..3000
        double hysteresisDb = 4.0;  ///< 0..20
    };

    void prepare (double sampleRate) noexcept;
    void setParams (const Params&) noexcept;
    const Params& getParams() const noexcept { return params; }
    void reset() noexcept;

    void process (float* left, float* right, int numSamples) noexcept;

    bool isOpen() const noexcept { return open; }
    double getGain() const noexcept { return gain; }

private:
    void update() noexcept;

    Params params;
    double sampleRate = 48000.0;
    double openLevel = 0.01, closeLevel = 0.006, floorGain = 1.0e-4;
    double attackCoef = 0.0, releaseCoef = 0.0;
    int holdSamples = 2400, holdLeft = 0;
    bool open = false;
    double gain = 1.0e-4;
};

} // namespace meridian::dsp
