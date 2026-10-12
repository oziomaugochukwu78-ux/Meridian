#pragma once

#include <array>
#include <vector>

namespace meridian::dsp
{

/** An algorithmic stereo reverb built on an 8-line feedback delay network.

        input -> pre-delay -> two diffusing all-passes per channel -> FDN
        FDN: 8 delay lines of mutually prime lengths, mixed every sample by an
             orthonormal Hadamard matrix (energy-preserving), each line with a
             decay gain chosen so the tail falls 60 dB in exactly `decay`
             seconds, and a one-pole low-pass for high-frequency damping
        output: two decorrelated sums of the lines (stereo), width, dry/wet mix

    Because the mixing matrix preserves energy, the decay time is set exactly
    by the per-line gains: g = 10^(-3 * lineLength / (decay * sampleRate)).

    prepare() allocates; process() is real-time safe. */
class FdnReverb
{
public:
    struct Params
    {
        double size = 1.0;        ///< 0.3 (small room) .. 2.0 (hall): scales the delay lengths
        double decay = 2.0;       ///< RT60 in seconds, 0.2..20
        double damping = 0.3;     ///< 0 (bright) .. 1 (dark): faster decay of high frequencies
        double predelayMs = 10.0; ///< 0..250
        double mix = 0.25;        ///< 0 (dry) .. 1 (wet only)
        double width = 1.0;       ///< 0 (mono tail) .. 1 (full stereo)
    };

    static constexpr int numLines = 8;

    void prepare (double sampleRate);
    void setParams (const Params&) noexcept;
    const Params& getParams() const noexcept { return params; }
    void reset() noexcept;

    /** In place. Mono input feeds both sides; a mono buffer receives the mid signal. */
    void process (float* left, float* right, int numSamples) noexcept;

private:
    struct Delay
    {
        std::vector<double> buffer;
        int length = 1, pos = 0;
        double read() const noexcept { return buffer[(size_t) pos]; }
        void write (double x) noexcept { buffer[(size_t) pos] = x; if (++pos >= length) pos = 0; }
    };

    struct AllPass
    {
        std::vector<double> buffer;
        int pos = 0;
        double process (double x, double g) noexcept;
    };

    void update() noexcept;

    Params params;
    double sampleRate = 48000.0;
    std::array<Delay, numLines> lines;
    std::array<double, numLines> gains {}, lowState {};
    double dampCoef = 0.0;
    std::array<AllPass, 4> diffusers;
    std::vector<double> predelayL, predelayR;
    int predelayPos = 0, predelaySamples = 0;
    int maxLineLength = 0;
};

} // namespace meridian::dsp
