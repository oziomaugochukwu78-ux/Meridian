// Loudness (ITU-R BS.1770-4 / EBU R128) against the EBU Tech 3341 and 3342
// test signals, which are defined as sine sequences and generated here.

#include <dsp/Loudness.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace meridian::dsp;
using Catch::Approx;

namespace
{
constexpr double pi = 3.14159265358979323846;

struct Segment
{
    double dbfs;    ///< sine peak level, both channels
    double seconds;
};

/** Feeds a stereo 1 kHz sine sequence to a meter in 100-sample blocks. */
LoudnessMeter measure (const std::vector<Segment>& segments, double rate = 48000.0)
{
    LoudnessMeter meter;
    meter.prepare (rate, 2);
    std::vector<float> block (100);
    const float* channels[2] = { block.data(), block.data() };
    int64_t n = 0;
    for (const auto& s : segments)
    {
        const double amplitude = std::pow (10.0, s.dbfs / 20.0);
        const auto total = (int64_t) std::llround (s.seconds * rate);
        for (int64_t done = 0; done < total; done += (int64_t) block.size())
        {
            const int count = (int) std::min<int64_t> ((int64_t) block.size(), total - done);
            for (int i = 0; i < count; ++i, ++n)
                block[(size_t) i] = (float) (amplitude * std::sin (2.0 * pi * 1000.0 * (double) n / rate));
            meter.process (channels, 2, count);
        }
    }
    return meter;
}
} // namespace

TEST_CASE ("K-weighting reproduces the BS.1770-4 coefficient table at 48 kHz", "[loudness]")
{
    KWeighting k;
    k.prepare (48000.0);
    const auto& s = k.getShelf();
    CHECK (s.b0 == Approx (1.53512485958697).margin (1.0e-9));
    CHECK (s.b1 == Approx (-2.69169618940638).margin (1.0e-9));
    CHECK (s.b2 == Approx (1.19839281085285).margin (1.0e-9));
    CHECK (s.a1 == Approx (-1.69065929318241).margin (1.0e-9));
    CHECK (s.a2 == Approx (0.73248077421585).margin (1.0e-9));
    const auto& h = k.getHighPass();
    CHECK (h.b0 == 1.0);
    CHECK (h.b1 == -2.0);
    CHECK (h.b2 == 1.0);
    CHECK (h.a1 == Approx (-1.99004745483398).margin (1.0e-9));
    CHECK (h.a2 == Approx (0.99007225036621).margin (1.0e-9));
}

TEST_CASE ("EBU 3341 cases 1 and 2: stereo sine at -23 and -33 dBFS reads -23 and -33 LUFS", "[loudness]")
{
    for (const double level : { -23.0, -33.0 })
    {
        const auto m = measure ({ { level, 20.0 } });
        CHECK (m.getMomentary() == Approx (level).margin (0.1));
        CHECK (m.getShortTerm() == Approx (level).margin (0.1));
        CHECK (m.getIntegrated() == Approx (level).margin (0.1));
    }
    // The same at 44.1 kHz (the filter is designed for any rate).
    CHECK (measure ({ { -23.0, 20.0 } }, 44100.0).getIntegrated() == Approx (-23.0).margin (0.1));
}

TEST_CASE ("EBU 3341 case 3: the relative gate ignores the quiet parts", "[loudness]")
{
    const auto m = measure ({ { -36.0, 10.0 }, { -23.0, 60.0 }, { -36.0, 10.0 } });
    CHECK (m.getIntegrated() == Approx (-23.0).margin (0.1));
}

TEST_CASE ("EBU 3341 case 4: the absolute gate ignores near-silence", "[loudness]")
{
    const auto m = measure ({ { -72.0, 10.0 }, { -36.0, 10.0 }, { -23.0, 60.0 }, { -36.0, 10.0 }, { -72.0, 10.0 } });
    CHECK (m.getIntegrated() == Approx (-23.0).margin (0.1));
}

TEST_CASE ("EBU 3341 case 5: -26 / -20 / -26 dBFS integrates to -23 LUFS", "[loudness]")
{
    const auto m = measure ({ { -26.0, 20.0 }, { -20.0, 20.1 }, { -26.0, 20.0 } });
    CHECK (m.getIntegrated() == Approx (-23.0).margin (0.1));
}

TEST_CASE ("EBU 3342 loudness range cases 1-4", "[loudness]")
{
    CHECK (measure ({ { -20.0, 20.0 }, { -30.0, 20.0 } }).getLoudnessRange() == Approx (10.0).margin (1.0));
    CHECK (measure ({ { -20.0, 20.0 }, { -15.0, 20.0 } }).getLoudnessRange() == Approx (5.0).margin (1.0));
    CHECK (measure ({ { -40.0, 20.0 }, { -20.0, 20.0 } }).getLoudnessRange() == Approx (20.0).margin (1.0));
    CHECK (measure ({ { -50.0, 20.0 }, { -35.0, 20.0 }, { -20.0, 20.0 }, { -35.0, 20.0 }, { -50.0, 20.0 } }).getLoudnessRange()
           == Approx (15.0).margin (1.0));
}

TEST_CASE ("Silence and a fresh meter report silence, not a number", "[loudness]")
{
    const auto m = measure ({ { -200.0, 5.0 } });
    CHECK (m.getIntegrated() == LoudnessMeter::silence);
    CHECK (m.getLoudnessRange() == 0.0);
}

TEST_CASE ("True peak: a quarter-rate sine with its samples at -3 dB of its peak", "[loudness]")
{
    // Every sample at 0.5 * 0.7071: sample peak -9.03 dBFS, true peak -6.02 dBTP.
    std::vector<float> x (48000);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = (float) (0.5 * std::sin (2.0 * pi * 0.25 * (double) i + pi / 4.0));
    TruePeakMeter tp;
    tp.prepare (1);
    const float* ch[1] = { x.data() };
    tp.process (ch, 1, (int) x.size());
    CHECK (20.0 * std::log10 (tp.getSamplePeak()) == Approx (-9.03).margin (0.01));
    // The whole signal, including its abrupt start from silence (where the
    // reconstruction rings, as a real converter's would): EBU 3341's +0.2 / -0.4 dB.
    CHECK (tp.getTruePeakDb() <= -6.02 + 0.2);
    CHECK (tp.getTruePeakDb() >= -6.02 - 0.4);

    // Steady state: the meter's own accuracy, well inside the tolerance.
    tp.resetPeak();
    const float* later[1] = { x.data() + 4800 };
    tp.process (later, 1, (int) x.size() - 4800);
    CHECK (tp.getTruePeakDb() == Approx (-6.02).margin (0.02));
}
