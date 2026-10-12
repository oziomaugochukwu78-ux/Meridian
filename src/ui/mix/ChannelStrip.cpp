#include "ChannelStrip.h"

#include "ui/Theme.h"

#include <dsp/FaderTaper.h>

namespace meridian
{
namespace
{
constexpr int sectionHeader = 13;
constexpr int slotHeight = 17;

juce::String formatDb (double db)
{
    if (db <= dsp::FaderTaper::minDb)
        return "-inf";
    return (db > 0.0 ? "+" : "") + juce::String (db, 1);
}

juce::String formatPan (double pan)
{
    const int value = juce::roundToInt (pan * 100.0);
    if (value == 0)
        return "0";
    return value < 0 ? "<" + juce::String (-value) : juce::String (value) + ">";
}

void styleKey (juce::TextButton& b, juce::Colour onColour, const juce::String& tooltip)
{
    b.setColour (juce::TextButton::buttonOnColourId, onColour);
    b.setColour (juce::TextButton::textColourOnId, theme::textOnLight);
    b.setTooltip (tooltip);
}

void drawSectionHeader (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text)
{
    g.setColour (theme::textDim);
    g.setFont (theme::font (9.5f, true));
    g.drawText (text, area, juce::Justification::centredLeft);
}
} // namespace

ChannelStrip::ChannelStrip (AppController& a, juce::ValueTree t)
    : app (a), trackNode (std::move (t)), meter (track::channels (trackNode))
{
    for (int i = 0; i < Session::maxInserts; ++i)
        addAndMakeVisible (insertSlots.add (new InsertSlotButton (app, trackNode, i)));

    populateInputs();
    inputSelector.setTooltip ("Track input");
    inputSelector.onChange = [this] { setProperty (ids::inputChannel, inputSelector.getSelectedId() - 2, "Track Input"); };
    addAndMakeVisible (inputSelector);

    outputLabel.setText ("Main Out", juce::dontSendNotification);
    outputLabel.setFont (theme::font (11.0f));
    outputLabel.setColour (juce::Label::backgroundColourId, theme::well);
    outputLabel.setTooltip ("Output: the master bus");
    addAndMakeVisible (outputLabel);

    automationSelector.addItem ("auto off", 1);
    automationSelector.addItem ("auto read", 2);
    automationSelector.setTooltip ("Automation mode");
    automationSelector.onChange = [this]
    {
        setProperty (ids::automationMode, automationSelector.getSelectedId() == 2 ? "read" : "off", "Automation Mode");
    };
    addAndMakeVisible (automationSelector);

    panKnob.setRange (-1.0, 1.0);
    panKnob.setDoubleClickReturnValue (true, 0.0);
    panKnob.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    panKnob.setTooltip ("Pan (double-click to centre)");
    panKnob.onDragStart = [this] { if (auto* um = app.getUndoManager()) um->beginNewTransaction ("Pan"); };
    panKnob.onValueChange = [this]
    {
        if (auto* um = app.getUndoManager())
            trackNode.setProperty (ids::pan, panKnob.getValue(), um);
    };
    addAndMakeVisible (panKnob);

    panReadout.setJustificationType (juce::Justification::centred);
    panReadout.setFont (theme::monoFont (11.0f));
    addAndMakeVisible (panReadout);

    styleKey (recordButton, theme::record, "Record enable");
    styleKey (soloButton, theme::solo, "Solo");
    styleKey (muteButton, theme::mute, "Mute");
    recordButton.onClick = [this] { setProperty (ids::recordArm, ! track::recordArm (trackNode), "Record Enable"); };
    soloButton.onClick = [this] { setProperty (ids::solo, ! track::solo (trackNode), "Solo"); };
    muteButton.onClick = [this] { setProperty (ids::mute, ! track::mute (trackNode), "Mute"); };
    for (auto* b : { &recordButton, &soloButton, &muteButton })
        addAndMakeVisible (b);

    fader.setRange (0.0, 1.0);
    fader.setDoubleClickReturnValue (true, dsp::FaderTaper::unityPosition);
    fader.setTooltip ("Volume (double-click for 0 dB)");
    fader.setVelocityBasedMode (false);
    fader.onDragStart = [this]
    {
        draggingFader = true;
        if (auto* um = app.getUndoManager())
            um->beginNewTransaction ("Volume");
    };
    fader.onDragEnd = [this] { draggingFader = false; };
    fader.onValueChange = [this]
    {
        if (auto* um = app.getUndoManager())
            trackNode.setProperty (ids::volumeDb, dsp::FaderTaper::positionToDb (fader.getValue()), um);
    };
    addAndMakeVisible (fader);
    addAndMakeVisible (meter);

    volumeReadout.setJustificationType (juce::Justification::centred);
    volumeReadout.setFont (theme::monoFont (11.0f));
    volumeReadout.setColour (juce::Label::backgroundColourId, theme::well);
    volumeReadout.setEditable (false, true, false);
    volumeReadout.setTooltip ("Double-click to type a level in dB");
    volumeReadout.onTextChange = [this]
    {
        const auto text = volumeReadout.getText().trim();
        const double db = text.startsWithIgnoreCase ("-inf") ? dsp::FaderTaper::minDb
                                                             : juce::jlimit (dsp::FaderTaper::minDb, dsp::FaderTaper::maxDb, text.getDoubleValue());
        setProperty (ids::volumeDb, db, "Volume");
    };
    addAndMakeVisible (volumeReadout);

    namePlate.setJustificationType (juce::Justification::centred);
    namePlate.setFont (theme::font (12.0f, true));
    namePlate.setColour (juce::Label::textColourId, theme::textOnLight);
    namePlate.setEditable (false, true, false);
    namePlate.onTextChange = [this]
    {
        if (namePlate.getText().trim().isNotEmpty())
            setProperty (ids::name, namePlate.getText().trim(), "Rename Track");
    };
    addAndMakeVisible (namePlate);

    trackNode.addListener (this);
    refresh();
}

ChannelStrip::~ChannelStrip()
{
    trackNode.removeListener (this);
}

void ChannelStrip::populateInputs()
{
    inputSelector.clear (juce::dontSendNotification);
    inputSelector.addItem ("no input", 1);

    int numInputs = 8;
    if (auto* device = app.getDeviceManager().getCurrentAudioDevice())
        numInputs = juce::jmax (1, device->getActiveInputChannels().getHighestBit() + 1);

    const bool stereo = track::channels (trackNode) == 2;
    for (int ch = 0; ch < numInputs - (stereo ? 1 : 0); ch += (stereo ? 2 : 1))
        inputSelector.addItem (stereo ? "In " + juce::String (ch + 1) + "-" + juce::String (ch + 2)
                                      : "In " + juce::String (ch + 1),
                               ch + 2); // id = input channel + 2, so "no input" (-1) is id 1
}

void ChannelStrip::setProperty (const juce::Identifier& property, const juce::var& value, const juce::String& undoName)
{
    if (auto* um = app.getUndoManager())
    {
        um->beginNewTransaction (undoName);
        trackNode.setProperty (property, value, um);
    }
}

void ChannelStrip::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree == trackNode)
    {
        if (property == ids::channels)
        {
            meter.setNumChannels (track::channels (trackNode));
            populateInputs();
            resized();
        }
        refresh();
    }
    else if (tree.hasType (ids::INSERT))
    {
        triggerAsyncUpdate();
    }
}

// Insert changes are shown asynchronously: this track-level listener runs
// before the session-level EngineBridge has (de)instantiated the plug-in.
void ChannelStrip::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&)
{
    if (parent.hasType (ids::INSERTS))
        triggerAsyncUpdate();
}

void ChannelStrip::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int)
{
    if (parent.hasType (ids::INSERTS))
        triggerAsyncUpdate();
}

void ChannelStrip::refreshInsertSlots()
{
    for (auto* s : insertSlots)
        s->refresh();
}

void ChannelStrip::refresh()
{
    const double db = track::volumeDb (trackNode);
    if (! draggingFader)
        fader.setValue (dsp::FaderTaper::dbToPosition (db), juce::dontSendNotification);
    volumeReadout.setText (formatDb (db), juce::dontSendNotification);

    panKnob.setValue (track::pan (trackNode), juce::dontSendNotification);
    panReadout.setText (formatPan (track::pan (trackNode)), juce::dontSendNotification);

    recordButton.setToggleState (track::recordArm (trackNode), juce::dontSendNotification);
    soloButton.setToggleState (track::solo (trackNode), juce::dontSendNotification);
    muteButton.setToggleState (track::mute (trackNode), juce::dontSendNotification);

    inputSelector.setSelectedId (track::inputChannel (trackNode) + 2, juce::dontSendNotification);
    automationSelector.setSelectedId (track::automationRead (trackNode) ? 2 : 1, juce::dontSendNotification);

    namePlate.setText (track::name (trackNode), juce::dontSendNotification);
    namePlate.setColour (juce::Label::backgroundColourId, track::colour (trackNode));
    refreshInsertSlots();
    repaint();
}

void ChannelStrip::updateMeter()
{
    if (auto state = app.getBridge().getTrackState (track::id (trackNode)))
    {
        const auto [left, right] = state->collectPeaks();
        meter.pushPeaks (left, right);
    }
}

//==============================================================================
void ChannelStrip::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::divider);
    g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());

    drawSectionHeader (g, insertsHeaderA, "INSERTS A-E");
    drawSectionHeader (g, insertsHeaderB, "INSERTS F-J");
    drawSectionHeader (g, sendsHeader, "SENDS A-E");
    drawSectionHeader (g, ioHeader, "I/O");

    // Sends: not implemented yet, drawn as disabled slots so the layout matches.
    auto sends = sendsArea;
    for (int i = 0; i < 5; ++i)
    {
        const auto r = sends.removeFromTop (slotHeight).toFloat().reduced (1.0f, 0.5f);
        g.setColour (theme::well.withAlpha (0.5f));
        g.fillRect (r);
        g.setColour (theme::divider);
        g.drawRect (r, 1.0f);
    }

    // dB scale between fader and meter.
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

void ChannelStrip::resized()
{
    auto area = getLocalBounds().reduced (4, 4).withTrimmedRight (1);

    insertsHeaderA = area.removeFromTop (sectionHeader);
    for (int i = 0; i < 5; ++i)
        insertSlots[i]->setBounds (area.removeFromTop (slotHeight));
    insertsHeaderB = area.removeFromTop (sectionHeader);
    for (int i = 5; i < 10; ++i)
        insertSlots[i]->setBounds (area.removeFromTop (slotHeight));

    sendsHeader = area.removeFromTop (sectionHeader);
    sendsArea = area.removeFromTop (slotHeight * 5);

    ioHeader = area.removeFromTop (sectionHeader);
    inputSelector.setBounds (area.removeFromTop (20));
    area.removeFromTop (2);
    outputLabel.setBounds (area.removeFromTop (18));
    area.removeFromTop (4);
    automationSelector.setBounds (area.removeFromTop (20));
    area.removeFromTop (4);

    panKnob.setBounds (area.removeFromTop (40).withSizeKeepingCentre (40, 40));
    panReadout.setBounds (area.removeFromTop (14));
    area.removeFromTop (4);

    auto keys = area.removeFromTop (22);
    const int keyWidth = (keys.getWidth() - 8) / 3;
    recordButton.setBounds (keys.removeFromLeft (keyWidth));
    keys.removeFromLeft (4);
    soloButton.setBounds (keys.removeFromLeft (keyWidth));
    keys.removeFromLeft (4);
    muteButton.setBounds (keys);
    area.removeFromTop (6);

    namePlate.setBounds (area.removeFromBottom (22));
    area.removeFromBottom (4);
    volumeReadout.setBounds (area.removeFromBottom (18));
    area.removeFromBottom (4);

    meter.setBounds (area.removeFromRight (track::channels (trackNode) == 2 ? 16 : 9));
    area.removeFromRight (2);
    scaleArea = area.removeFromRight (22);
    fader.setBounds (area);
}

} // namespace meridian
