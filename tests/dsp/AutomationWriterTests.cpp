// Recording automation passes: write, touch, latch, and thinning.

#include <dsp/AutomationWriter.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace meridian::dsp;
using Catch::Approx;

namespace
{
/** Feeds a pass sampled every `step` samples from `from` to `to`; value(t), touched(t). */
template <typename Value, typename Touched>
void pass (AutomationWriter& w, int64_t from, int64_t to, int64_t step, Value value, Touched touched)
{
    for (auto t = from; t <= to; t += step)
        w.update (t, value (t), touched (t));
}

double at (const std::vector<AutomationPoint>& lane, int64_t t) { return AutomationCurve (lane).valueAt (t); }
} // namespace

TEST_CASE ("Automation writer: write mode records the whole pass and leaves the rest alone", "[autowrite]")
{
    // Existing: flat -6 dB everywhere (points at 0 and 1,000,000).
    AutomationWriter w (AutomationMode::write, { { 0, -6.0 }, { 1000000, -6.0 } }, 0.0, 0.01);
    pass (w, 100000, 200000, 480, [] (int64_t t) { return -20.0 + (double) (t - 100000) / 10000.0; }, [] (int64_t) { return false; });
    const auto lane = w.finish (200000);

    CHECK (at (lane, 50000) == Approx (-6.0));            // before: unchanged
    CHECK (at (lane, 99999) == Approx (-6.0));
    CHECK (at (lane, 100000) == Approx (-20.0));           // the pass, a ramp -20 -> -10 dB
    CHECK (at (lane, 150000) == Approx (-15.0).margin (0.01));
    CHECK (at (lane, 199840) == Approx (-10.016).margin (0.001)); // the last value sampled...
    CHECK (at (lane, 200000) == Approx (-10.016).margin (0.001)); // ...held to the stop
    CHECK (at (lane, 200001) == Approx (-6.0));            // after: unchanged
    CHECK (at (lane, 900000) == Approx (-6.0));
    CHECK (lane.size() <= 8);                               // a straight ramp thins to its two ends (+ joins)
}

TEST_CASE ("Automation writer: touch mode records only while held, then glides back", "[autowrite]")
{
    AutomationWriter w (AutomationMode::touch, { { 0, 0.0 } }, 0.0, 0.01, 4800);
    // Held from 100000 to 150000 at -12 dB; untouched otherwise (value ignored then).
    pass (w, 0, 300000, 480, [] (int64_t t) { return t >= 100000 && t <= 150000 ? -12.0 : 99.0; },
          [] (int64_t t) { return t >= 100000 && t <= 150000; });
    const auto lane = w.finish (300000);

    CHECK (at (lane, 90000) == Approx (0.0));
    CHECK (at (lane, 120000) == Approx (-12.0));
    CHECK (at (lane, 149760) == Approx (-12.0));               // the last value while held
    CHECK (at (lane, 149760 + 2400) == Approx (-6.0));         // halfway through the 4800-sample glide back
    CHECK (at (lane, 149760 + 4800) == Approx (0.0));          // back on the existing curve
    CHECK (at (lane, 250000) == Approx (0.0));
}

TEST_CASE ("Automation writer: latch mode records from the first touch to the stop", "[autowrite]")
{
    AutomationWriter w (AutomationMode::latch, {}, 3.0, 0.01); // no lane yet: the static value is +3 dB
    pass (w, 0, 200000, 480, [] (int64_t t) { return t < 100000 ? 3.0 : -9.0; }, [] (int64_t t) { return t >= 100000 && t < 110000; });
    const auto lane = w.finish (200000);
    CHECK (at (lane, 50000) == Approx (3.0));    // before the touch: the static value
    CHECK (at (lane, 150000) == Approx (-9.0));  // released at 110000, but latch keeps writing
    CHECK (at (lane, 200000) == Approx (-9.0));
}

TEST_CASE ("Automation writer: read and off record nothing; a jump back ends the stretch", "[autowrite]")
{
    AutomationWriter r (AutomationMode::read, { { 0, -3.0 } }, 0.0, 0.01);
    pass (r, 0, 48000, 480, [] (int64_t) { return 6.0; }, [] (int64_t) { return true; });
    CHECK (! r.hasRecorded());
    CHECK (r.finish (48000).size() == 1);

    // Write, playing 0..48000, then a loop jump back to 24000 and on to 48000 again.
    AutomationWriter w (AutomationMode::write, {}, 0.0, 0.01);
    pass (w, 0, 48000, 480, [] (int64_t) { return -6.0; }, [] (int64_t) { return false; });
    pass (w, 24000, 48000, 480, [] (int64_t) { return -3.0; }, [] (int64_t) { return false; });
    const auto lane = w.finish (48000);
    CHECK (at (lane, 10000) == Approx (-6.0)); // the first pass, before the loop start
    CHECK (at (lane, 30000) == Approx (-3.0)); // the second pass replaced the loop range
}

TEST_CASE ("Automation writer: thinning keeps every value within the tolerance", "[autowrite]")
{
    std::vector<AutomationPoint> dense;
    for (int64_t t = 0; t <= 48000; t += 48)
        dense.push_back ({ t, 6.0 * std::sin (6.283185307179586 * (double) t / 48000.0) });
    const auto thinned = AutomationWriter::thin (dense, 0.05);
    CHECK (thinned.size() < dense.size() / 4);
    const AutomationCurve curve (thinned);
    double worst = 0.0;
    for (const auto& p : dense)
        worst = std::max (worst, std::abs (curve.valueAt (p.time) - p.value));
    CHECK (worst <= 0.05);
}
