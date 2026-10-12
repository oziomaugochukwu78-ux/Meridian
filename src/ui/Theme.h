#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/** The visual language: a dark-grey console. Every colour used by the UI is
    defined here, so the whole look can be re-tuned in one place. */
namespace meridian::theme
{
// Surfaces, darkest to lightest
inline const juce::Colour window      { 0xff1a1a1a }; ///< main background
inline const juce::Colour well        { 0xff141414 }; ///< recessed areas: timeline, fader slots
inline const juce::Colour panel       { 0xff232323 }; ///< headers, strips, toolbars
inline const juce::Colour panelRaised { 0xff2e2e2e }; ///< buttons, slots
inline const juce::Colour panelHover  { 0xff3a3a3a };
inline const juce::Colour divider     { 0xff0b0b0b };
inline const juce::Colour gridLine    { 0xff262626 };
inline const juce::Colour gridBar     { 0xff333333 };

// Text
inline const juce::Colour text        { 0xffd6d6d6 }; ///< light-grey primary text
inline const juce::Colour textDim     { 0xff8c8c8c };
inline const juce::Colour textOnLight { 0xff101010 };

// The traditional console button colours
inline const juce::Colour mute        { 0xffe6c229 }; ///< yellow
inline const juce::Colour solo        { 0xff3fbf4a }; ///< green
inline const juce::Colour record      { 0xffd8352b }; ///< red
inline const juce::Colour play        { 0xff47b54f };
inline const juce::Colour automation  { 0xff4b8fe2 };
inline const juce::Colour bypass      { 0xff3d6aa6 };

// Timeline
inline const juce::Colour playhead    { 0xffe8e8e8 };
inline const juce::Colour selection   { 0x553a7bd5 };
inline const juce::Colour selectionEdge { 0xff5b9cf0 };
inline const juce::Colour clipBody    { 0xff2b3a4f };
inline const juce::Colour waveform    { 0xffc9d3e0 };

// Meters
inline const juce::Colour meterGreen  { 0xff36c24a };
inline const juce::Colour meterYellow { 0xffd9cf32 };
inline const juce::Colour meterOrange { 0xffe08a2a };
inline const juce::Colour meterRed    { 0xffe3302a };
inline const juce::Colour meterOff    { 0xff1f2420 };

inline juce::Font font (float height, bool bold = false)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}

inline juce::Font monoFont (float height)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), height, juce::Font::plain));
}
} // namespace meridian::theme
