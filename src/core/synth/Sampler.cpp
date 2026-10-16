#include "Sampler.h"

#include <algorithm>
#include <cmath>

namespace meridian::synth
{
namespace
{
double dbToGain (double db) noexcept { return std::pow (10.0, db / 20.0); }

/** 4-point, 3rd-order Hermite interpolation of x at position `at` (0 outside). */
float readAt (const std::vector<float>& x, double at) noexcept
{
    const auto i = (int64_t) std::floor (at);
    const float t = (float) (at - (double) i);
    const auto n = (int64_t) x.size();
    auto get = [&] (int64_t k) { return k >= 0 && k < n ? x[(size_t) k] : 0.0f; };
    const float xm1 = get (i - 1), x0 = get (i), x1 = get (i + 1), x2 = get (i + 2);
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

bool isOpenHat (int note) noexcept { return note == 46; }
bool chokesOpenHat (int note) noexcept { return note == 42 || note == 44; }
} // namespace

const Zone* ZoneMap::find (int note) const noexcept
{
    for (const auto& z : zones)
        if (z.sample != nullptr && note >= z.lowNote && note <= z.highNote)
            return &z;
    return nullptr;
}

void Sampler::prepare (double rate) noexcept
{
    sampleRate = rate > 0.0 ? rate : 48000.0;
    reset();
}

void Sampler::setParams (const Params& p) noexcept
{
    params = p;
    params.tuneSemitones = std::clamp (p.tuneSemitones, -24.0, 24.0);
    params.attack = std::clamp (p.attack, 0.0, 10.0);
    params.release = std::clamp (p.release, 0.001, 20.0);
}

void Sampler::setZones (const ZoneMap* map) noexcept
{
    if (map != zones)
        reset(); // the old map's samples may go away
    zones = map;
}

void Sampler::reset() noexcept
{
    for (auto& v : voices)
        v = Voice {};
    pedalDown = false;
    bendFactor = 1.0;
}

double Sampler::releaseFactor (bool fast) const noexcept
{
    // Per-sample multiplier reaching -60 dB in the release time (choke: 5 ms).
    const double seconds = fast ? 0.005 : params.release;
    return std::pow (10.0, -3.0 / (seconds * sampleRate));
}

void Sampler::startRelease (Voice& v, bool fast) noexcept
{
    v.releasing = true;
    v.choking = v.choking || fast;
    v.held = false;
}

void Sampler::noteOn (int pitch, float velocity) noexcept
{
    if (zones == nullptr || velocity <= 0.0f)
        return;
    const auto* zone = zones->find (pitch);
    if (zone == nullptr)
        return;

    if (params.oneShot && chokesOpenHat (pitch))
        for (auto& v : voices)
            if (v.sample != nullptr && isOpenHat (v.note))
                startRelease (v, true);

    // A free voice, or else the oldest releasing one, or else the oldest.
    Voice* slot = nullptr;
    for (auto& v : voices)
        if (v.sample == nullptr)
        {
            slot = &v;
            break;
        }
    if (slot == nullptr)
        for (auto& v : voices)
            if (v.releasing && (slot == nullptr || v.age < slot->age))
                slot = &v;
    if (slot == nullptr)
        for (auto& v : voices)
            if (slot == nullptr || v.age < slot->age)
                slot = &v;

    const auto& sample = *zone->sample;
    Voice v;
    v.sample = &sample;
    v.note = pitch;
    v.position = 0.0;
    v.speed = sample.sampleRate / sampleRate * std::pow (2.0, ((double) (pitch - zone->rootNote) + params.tuneSemitones) / 12.0);
    v.gain = (float) (std::pow ((double) std::clamp (velocity, 0.0f, 1.0f), 1.5) * dbToGain (zone->gainDb));
    v.attackStep = params.attack > 0.0 ? 1.0 / (params.attack * sampleRate) : 1.0;
    v.envelope = params.attack > 0.0 ? 0.0 : 1.0;
    v.held = v.keyDown = true;
    v.age = ++counter;
    *slot = v;
}

void Sampler::noteOff (int pitch) noexcept
{
    if (params.oneShot)
        return;
    for (auto& v : voices)
        if (v.sample != nullptr && v.note == pitch && v.keyDown)
        {
            v.keyDown = false;
            if (! pedalDown)
                startRelease (v, false);
        }
}

void Sampler::allNotesOff() noexcept
{
    pedalDown = false;
    bendFactor = 1.0;
    if (params.oneShot)
        return;
    for (auto& v : voices)
        if (v.sample != nullptr && ! v.releasing)
        {
            v.keyDown = false;
            startRelease (v, false);
        }
}

void Sampler::setSustain (bool down) noexcept
{
    pedalDown = down;
    if (! down && ! params.oneShot)
        for (auto& v : voices)
            if (v.sample != nullptr && ! v.keyDown && ! v.releasing)
                startRelease (v, false);
}

void Sampler::setPitchBend (float amount) noexcept
{
    bendFactor = params.oneShot ? 1.0 : std::pow (2.0, std::clamp ((double) amount, -1.0, 1.0) * bendRangeSemitones / 12.0);
}

void Sampler::render (float* left, float* right, int numSamples) noexcept
{
    const float output = (float) dbToGain (params.gainDb);
    const double slow = releaseFactor (false), fast = releaseFactor (true);
    for (auto& v : voices)
    {
        if (v.sample == nullptr)
            continue;
        const auto& s = *v.sample;
        const bool stereo = ! s.right.empty();
        const double step = v.speed * bendFactor;
        const double end = (double) s.length();
        for (int i = 0; i < numSamples; ++i)
        {
            if (v.position >= end + 2.0 || (v.releasing && v.envelope < 1.0e-3))
            {
                v.sample = nullptr;
                break;
            }
            if (v.releasing)
                v.envelope *= v.choking ? fast : slow;
            else if (v.envelope < 1.0)
                v.envelope = std::min (1.0, v.envelope + v.attackStep);

            const float g = v.gain * (float) v.envelope * output;
            const float l = readAt (s.left, v.position);
            left[i] += g * l;
            right[i] += g * (stereo ? readAt (s.right, v.position) : l);
            v.position += step;
        }
    }
}

int Sampler::getNumActiveVoices() const noexcept
{
    int n = 0;
    for (const auto& v : voices)
        n += v.sample != nullptr ? 1 : 0;
    return n;
}

} // namespace meridian::synth
