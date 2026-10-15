#pragma once

#include "Session.h"

#include <vector>

/** Factory presets for the master bus console stage (ROADMAP.md, "Presets").

    Each preset is defined by measurable targets, not adjectives: the total
    harmonic distortion and harmonic balance of a 1 kHz sine at -6 dBFS
    through the master. The settings were tuned to fall inside each band, and
    the self test measures every preset through a real bounce to keep them
    there. Level matching is built into the curve (unity small-signal gain),
    so comparing presets is never fooled by "louder sounds better". */
namespace meridian::consolepresets
{

struct Preset
{
    const char* name;
    const char* description;
    bool enabled;          ///< Clean bypasses the stage
    double driveDb;
    double character;
    int curve;             ///< 0 tanh, 1 cubic (as SESSION.consoleCurve)
    double minThdPercent;  ///< the target band at -6 dBFS, 1 kHz
    double maxThdPercent;
    int dominantHarmonic;  ///< 2 or 3: which of H2 and H3 must be the larger (0: none)
};

/** Clean, Warm Tape, Vintage Console, Gritty. */
const std::vector<Preset>& all();

/** The preset with this name (case-insensitive), or nullptr. */
const Preset* find (const juce::String& name);

/** Sets the session's console to the preset (undoable). */
void apply (juce::ValueTree sessionState, const Preset&, juce::UndoManager*);

/** The name of the preset the console matches, or "Custom". */
juce::String current (const juce::ValueTree& sessionState);

} // namespace meridian::consolepresets
