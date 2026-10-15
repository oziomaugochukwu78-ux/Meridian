#pragma once

namespace meridian::dsp
{

/** The analog-console colour applied on the master bus.

    A memoryless wave-shaper y = f(x) normalised to UNITY SMALL-SIGNAL GAIN:
    quiet material passes through bit-for-bit level-matched, and only peaks
    approaching the "knee" are rounded off. Raising drive lowers the knee, so
    drive controls *how early* saturation starts rather than simply making the
    mix louder; that keeps A/B comparisons level-matched.

    Two curves:
      - tanh:  smooth, progressively compressing, odd harmonics (tape/transformer-like)
      - cubic: x - x^3/3 with a hard ceiling; gentler low-order harmonics (classic soft clip)

    Asymmetry adds a DC bias inside the curve, which generates even (2nd-order)
    harmonics, the "warmth" associated with transformer and valve stages. The
    bias offset is subtracted so silence stays silence; the slow DC wander
    that asymmetric shaping causes on real material is removed by DcBlocker.

    Wave-shaping creates harmonics above Nyquist. This class is the pure curve:
    the engine runs it inside 4x oversampling (MasterBusProcessor) so those
    harmonics are filtered out instead of aliasing back as inharmonic tones.
*/
class SaturationCurve
{
public:
    enum class Type { tanh, cubic };

    /** Drive range, dB. 0 dB puts the knee at full scale; below 0 it sits above
        full scale, so even loud mixes are only coloured (the gentle presets). */
    static constexpr double minDriveDb = -12.0, maxDriveDb = 24.0;

    /** Drive in dB, minDriveDb..maxDriveDb. */
    void setDriveDecibels (double driveDb) noexcept;

    /** 0 = perfectly symmetric (odd harmonics only) .. 1 = strongly asymmetric. */
    void setAsymmetry (double amount) noexcept;

    void setType (Type newType) noexcept;

    double process (double x) const noexcept;
    void processBlock (double* data, int numSamples) const noexcept;

    double getDriveGain() const noexcept { return drive; }

private:
    void recalculate() noexcept;
    double shape (double u) const noexcept;

    Type type = Type::tanh;
    double drive = 1.0;
    double bias = 0.0;
    double biasOffset = 0.0;  // shape(bias): keeps f(0) == 0
    double normaliser = 1.0;  // 1 / (drive * shape'(bias)): unity small-signal gain
};

} // namespace meridian::dsp
