#include "MasterStrip.h"

#include "ui/Theme.h"

#include <dsp/FaderTaper.h>

namespace meridian
{
namespace
{
juce::String formatDb (double db)
{
    if (db <= dsp::FaderTaper::minDb)
        return "-inf";
    return (db > 0.0 ? "+" : "") + juce::String (db, 1);
}

void setupKnob (juce::Slider& knob, double min, double max, double defaultValue, const juce::String& tooltip)
{
    knob.setRange (min, max);
    knob.setDoubleClickReturnValue (true, defaultValue);
    knob.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    knob.setColour (juce::Slider::rotarySliderFillColourId, theme::meterOrange);
    knob.setTooltip (tooltip);
}
} // namespace

MasterStrip::MasterStrip (AppController& a) : app (a)
{
    auto& console = app.getEngine().getMasterBus();

    consoleButton.setClickingTogglesState (false);
    consoleButton.setColour (juce::TextButton::buttonOnColourId, theme::meterOrange);
    consoleButton.setTooltip ("Analog console emulation on the master bus: 64-bit, 4x oversampled saturation");
    consoleButton.onClick = [this, &console]
    {
        const bool enabled = ! console.consoleEnabled.load();
        console.consoleEnabled.store (enabled);
        store (ids::consoleEnabled, enabled, enabled ? "Console On" : "Console Off");
        refresh();
    };
    addAndMakeVisible (consoleButton);

    setupKnob (driveKnob, 0.0, 24.0, 6.0, "Drive: how early saturation starts (0 dB = almost transparent)");
    driveKnob.onDragStart = [this] { if (auto* um = app.getUndoManager()) um->beginNewTransaction ("Console Drive"); };
    driveKnob.onValueChange = [this, &console]
    {
        console.driveDb.store (driveKnob.getValue());
        if (auto* s = app.getSession())
            s->getState().setProperty (ids::consoleDrive, driveKnob.getValue(), app.getUndoManager());
        driveLabel.setText ("DRIVE " + juce::String (driveKnob.getValue(), 1), juce::dontSendNotification);
    };
    addAndMakeVisible (driveKnob);

    setupKnob (characterKnob, 0.0, 1.0, 0.35, "Character: asymmetry, which adds even-order (2nd) harmonics");
    characterKnob.onDragStart = [this] { if (auto* um = app.getUndoManager()) um->beginNewTransaction ("Console Character"); };
    characterKnob.onValueChange = [this, &console]
    {
        console.character.store (characterKnob.getValue());
        if (auto* s = app.getSession())
            s->getState().setProperty (ids::consoleCharacter, characterKnob.getValue(), app.getUndoManager());
        characterLabel.setText ("CHAR " + juce::String (juce::roundToInt (characterKnob.getValue() * 100.0)) + "%",
                                juce::dontSendNotification);
    };
    addAndMakeVisible (characterKnob);

    for (auto* l : { &driveLabel, &characterLabel, &latencyLabel })
    {
        l->setJustificationType (juce::Justification::centred);
        l->setFont (theme::font (10.0f, true));
        l->setColour (juce::Label::textColourId, theme::textDim);
        addAndMakeVisible (l);
    }

    curveSelector.addItem ("Tanh (tape)", 1);
    curveSelector.addItem ("Cubic (clip)", 2);
    curveSelector.setTooltip ("Saturation curve");
    curveSelector.onChange = [this, &console]
    {
        const int type = curveSelector.getSelectedId() == 2 ? 1 : 0;
        console.curveType.store (type);
        store (ids::consoleCurve, type, "Console Curve");
    };
    addAndMakeVisible (curveSelector);

    fader.setRange (0.0, 1.0);
    fader.setDoubleClickReturnValue (true, dsp::FaderTaper::unityPosition);
    fader.onDragStart = [this]
    {
        draggingFader = true;
        if (auto* um = app.getUndoManager())
            um->beginNewTransaction ("Master Volume");
    };
    fader.onDragEnd = [this] { draggingFader = false; };
    fader.onValueChange = [this]
    {
        const double db = dsp::FaderTaper::positionToDb (fader.getValue());
        app.getEngine().masterVolumeDb.store (db);
        if (auto* s = app.getSession())
            s->getState().setProperty (ids::masterVolumeDb, db, app.getUndoManager());
        volumeReadout.setText (formatDb (db), juce::dontSendNotification);
    };
    addAndMakeVisible (fader);
    addAndMakeVisible (meter);

    volumeReadout.setJustificationType (juce::Justification::centred);
    volumeReadout.setFont (theme::monoFont (11.0f));
    volumeReadout.setColour (juce::Label::backgroundColourId, theme::well);
    addAndMakeVisible (volumeReadout);

    namePlate.setText ("Master", juce::dontSendNotification);
    namePlate.setJustificationType (juce::Justification::centred);
    namePlate.setFont (theme::font (12.0f, true));
    namePlate.setColour (juce::Label::backgroundColourId, theme::text);
    namePlate.setColour (juce::Label::textColourId, theme::textOnLight);
    addAndMakeVisible (namePlate);

    loudnessReadout.setFont (theme::monoFont (10.5f));
    loudnessReadout.setJustificationType (juce::Justification::topLeft);
    loudnessReadout.setColour (juce::Label::backgroundColourId, theme::well);
    loudnessReadout.setText ("M --  S --\nI -- LUFS\nTP -- dBTP", juce::dontSendNotification);
    loudnessReadout.setTooltip ("Loudness of the master output (EBU R128 / ITU-R BS.1770-4): momentary (400 ms), "
                                "short-term (3 s), integrated since reset, and the highest true peak (red above -1 dBTP).");
    addAndMakeVisible (loudnessReadout);
    loudnessReset.setTooltip ("Restart the integrated loudness and true-peak hold");
    loudnessReset.onClick = [this]
    {
        loudness.reset();
        truePeakHeld = 0.0;
    };
    addAndMakeVisible (loudnessReset);

    refresh();
}

void MasterStrip::store (const juce::Identifier& property, const juce::var& value, const juce::String& undoName)
{
    if (auto* s = app.getSession())
    {
        s->getUndoManager().beginNewTransaction (undoName);
        s->getState().setProperty (property, value, &s->getUndoManager());
    }
}

void MasterStrip::refresh()
{
    auto& console = app.getEngine().getMasterBus();
    consoleButton.setToggleState (console.consoleEnabled.load(), juce::dontSendNotification);
    driveKnob.setValue (console.driveDb.load(), juce::dontSendNotification);
    characterKnob.setValue (console.character.load(), juce::dontSendNotification);
    curveSelector.setSelectedId (console.curveType.load() == 1 ? 2 : 1, juce::dontSendNotification);

    driveLabel.setText ("DRIVE " + juce::String (driveKnob.getValue(), 1), juce::dontSendNotification);
    characterLabel.setText ("CHAR " + juce::String (juce::roundToInt (characterKnob.getValue() * 100.0)) + "%",
                            juce::dontSendNotification);
    latencyLabel.setText ("+" + juce::String (console.getLatencySamples()) + " smp latency", juce::dontSendNotification);

    const double db = app.getEngine().masterVolumeDb.load();
    if (! draggingFader)
        fader.setValue (dsp::FaderTaper::dbToPosition (db), juce::dontSendNotification);
    volumeReadout.setText (formatDb (db), juce::dontSendNotification);
}

void MasterStrip::updateMeter()
{
    const auto [left, right] = app.getEngine().collectMasterPeaks();
    meter.pushPeaks (left, right);

    // Loudness: the engine sends 100 ms K-weighted blocks; the statistics are kept here.
    if (const double rate = app.getEngine().getSampleRate(); ! juce::exactlyEqual (rate, loudnessRate))
    {
        loudness.prepare (rate, 2);
        loudnessRate = rate;
        truePeakHeld = 0.0;
    }
    AudioEngine::LoudnessBlock block;
    bool any = false;
    while (app.getEngine().popLoudnessBlock (block))
    {
        loudness.addSubBlock (block.meanSquare);
        truePeakHeld = juce::jmax (truePeakHeld, (double) block.truePeak);
        any = true;
    }
    if (! any)
        return;

    auto show = [] (double v) { return v <= dsp::LoudnessMeter::absoluteGate ? juce::String ("--") : juce::String (v, 1); };
    loudnessReadout.setText ("M " + show (loudness.getMomentary()) + "  S " + show (loudness.getShortTerm()) + "\n"
                             + "I " + show (loudness.getIntegrated()) + " LUFS\n"
                             + "TP " + (truePeakHeld > 0.0 ? juce::String (20.0 * std::log10 (truePeakHeld), 1) : juce::String ("--")) + " dBTP",
                             juce::dontSendNotification);
    loudnessReadout.setColour (juce::Label::textColourId, truePeakHeld > std::pow (10.0, -1.0 / 20.0) ? theme::record : theme::text);
}

void MasterStrip::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::divider);
    g.drawVerticalLine (0, 0.0f, (float) getHeight());

    g.setColour (theme::well);
    g.fillRoundedRectangle (consoleHeader.toFloat(), 3.0f);

    g.setFont (theme::font (9.0f));
    g.setColour (theme::textDim);
    const auto faderBounds = fader.getBounds();
    const float travelTop = (float) faderBounds.getY() + 13.0f;
    const float travelHeight = (float) faderBounds.getHeight() - 26.0f;
    for (const double db : { 12.0, 6.0, 0.0, -6.0, -12.0, -24.0, -40.0, -60.0 })
    {
        const float y = travelTop + travelHeight * (float) (1.0 - dsp::FaderTaper::dbToPosition (db));
        g.drawText (db > 0 ? "+" + juce::String ((int) db) : juce::String ((int) db),
                    juce::Rectangle<float> ((float) scaleArea.getX(), y - 6.0f, (float) scaleArea.getWidth(), 12.0f),
                    juce::Justification::centredRight);
    }
}

void MasterStrip::resized()
{
    auto area = getLocalBounds().reduced (6, 4).withTrimmedLeft (1);

    consoleHeader = area.removeFromTop (196);
    auto console = consoleHeader.reduced (6);
    consoleButton.setBounds (console.removeFromTop (24));
    console.removeFromTop (6);

    auto knobs = console.removeFromTop (54);
    driveKnob.setBounds (knobs.removeFromLeft (knobs.getWidth() / 2).withSizeKeepingCentre (48, 48));
    characterKnob.setBounds (knobs.withSizeKeepingCentre (48, 48));

    auto labels = console.removeFromTop (14);
    driveLabel.setBounds (labels.removeFromLeft (labels.getWidth() / 2));
    characterLabel.setBounds (labels);
    console.removeFromTop (8);
    curveSelector.setBounds (console.removeFromTop (22));
    console.removeFromTop (6);
    latencyLabel.setBounds (console.removeFromTop (16));

    area.removeFromTop (6);
    auto loudnessArea = area.removeFromTop (46);
    loudnessReset.setBounds (loudnessArea.removeFromTop (14).removeFromRight (40));
    loudnessReadout.setBounds (loudnessArea.withTrimmedTop (-14).withTrimmedRight (0));
    area.removeFromTop (6);
    namePlate.setBounds (area.removeFromBottom (22));
    area.removeFromBottom (4);
    volumeReadout.setBounds (area.removeFromBottom (18));
    area.removeFromBottom (4);

    meter.setBounds (area.removeFromRight (20));
    area.removeFromRight (2);
    scaleArea = area.removeFromRight (24);
    fader.setBounds (area);
}

} // namespace meridian
