#include "ConsolePresets.h"

#include <cmath>

namespace meridian::consolepresets
{

const std::vector<Preset>& all()
{
    // THD and H2/H3 of the curve alone at -6 dBFS (a model of SaturationCurve):
    //   Warm Tape        0.67 %, H2 14 dB over H3
    //   Vintage Console  0.25 %, H3 3.6 dB over H2
    //   Gritty           4.6 %,  H3 18 dB over H2
    static const std::vector<Preset> presets {
        { "Clean",           "Bypassed: the mix untouched.",                                false,   0.0, 0.0,  0, 0.0, 0.001, 0 },
        { "Warm Tape",       "Soft, even-order saturation that rounds peaks gently.",      true,  -12.0, 0.30, 0, 0.3, 1.0,   2 },
        { "Vintage Console", "Gentle odd-order density, like a classic console's bus.",     true,  -10.0, 0.05, 1, 0.1, 0.5,   3 },
        { "Gritty",          "Audible drive: odd harmonics that add edge.",                true,    4.0, 0.05, 0, 3.0, 8.0,   3 },
    };
    return presets;
}

const Preset* find (const juce::String& name)
{
    for (const auto& p : all())
        if (name.trim().equalsIgnoreCase (p.name))
            return &p;
    return nullptr;
}

void apply (juce::ValueTree s, const Preset& p, juce::UndoManager* um)
{
    s.setProperty (ids::consoleEnabled, p.enabled, um);
    if (! p.enabled)
        return; // Clean leaves the stage's settings for when it is switched back on
    s.setProperty (ids::consoleDrive, p.driveDb, um);
    s.setProperty (ids::consoleCharacter, p.character, um);
    s.setProperty (ids::consoleCurve, p.curve, um);
}

juce::String current (const juce::ValueTree& s)
{
    const bool enabled = s.getProperty (ids::consoleEnabled, true);
    if (! enabled)
        return "Clean";

    const double drive = s.getProperty (ids::consoleDrive, 6.0);
    const double character = s.getProperty (ids::consoleCharacter, 0.35);
    const int curve = s.getProperty (ids::consoleCurve, 0);
    for (const auto& p : all())
        if (p.enabled && std::abs (drive - p.driveDb) < 0.05 && std::abs (character - p.character) < 0.005 && curve == p.curve)
            return p.name;
    return "Custom";
}

} // namespace meridian::consolepresets
