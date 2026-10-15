#include "MixView.h"

#include "ui/Theme.h"

namespace meridian
{

MixView::MixView (AppController& a) : app (a), master (a)
{
    viewport.setViewedComponent (&stripContainer, false);
    viewport.setScrollBarsShown (true, true);
    addAndMakeVisible (viewport);
    addAndMakeVisible (master);

    emptyHint.setText ("No tracks. Track > New Track (Ctrl+Shift+N).", juce::dontSendNotification);
    emptyHint.setJustificationType (juce::Justification::centred);
    emptyHint.setColour (juce::Label::textColourId, theme::textDim);
    addChildComponent (emptyHint);

    auto& settings = app.getSettings();
    sections.insertsFJ = settings.getBoolValue ("mixShowInsertsFJ", true);
    sections.sends = settings.getBoolValue ("mixShowSends", true);
    sections.simple = settings.getBoolValue ("mixSimpleMode", false);

    app.addChangeListener (this);
    bindSession();
}

void MixView::setSections (ChannelStrip::Sections s)
{
    sections = s;
    auto& settings = app.getSettings();
    settings.setValue ("mixShowInsertsFJ", s.insertsFJ);
    settings.setValue ("mixShowSends", s.sends);
    settings.setValue ("mixSimpleMode", s.simple);
    for (auto* strip : strips)
        strip->setSections (s);
    layoutStrips();
}

MixView::~MixView()
{
    if (tracksNode.isValid())
        tracksNode.removeListener (this);
    app.removeChangeListener (this);
}

void MixView::bindSession()
{
    if (tracksNode.isValid())
        tracksNode.removeListener (this);

    auto* session = app.getSession();
    tracksNode = session != nullptr ? session->getTracksNode() : juce::ValueTree();
    if (tracksNode.isValid())
        tracksNode.addListener (this);

    master.refresh();
    rebuildStrips();
}

void MixView::rebuildStrips()
{
    strips.clear();
    for (const auto& t : tracksNode)
    {
        auto* strip = strips.add (new ChannelStrip (app, t));
        strip->setSections (sections);
        strip->onPreferredHeightChanged = [this] { layoutStrips(); };
        stripContainer.addAndMakeVisible (strip);
    }

    emptyHint.setVisible (strips.isEmpty() && app.getSession() != nullptr);
    layoutStrips();
}

void MixView::layoutStrips()
{
    // The tallest strip decides (in Simple mode strips differ: only used sections show).
    int needed = strips.isEmpty() ? ChannelStrip::minimumHeight (sections) : 0;
    for (const auto* strip : strips)
        needed = juce::jmax (needed, strip->preferredHeight());
    const int height = juce::jmax (needed, viewport.getMaximumVisibleHeight());
    stripContainer.setSize (juce::jmax (1, strips.size() * ChannelStrip::stripWidth), height);
    for (int i = 0; i < strips.size(); ++i)
        strips[i]->setBounds (i * ChannelStrip::stripWidth, 0, ChannelStrip::stripWidth, height);
}

void MixView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    bindSession();
}

void MixView::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&)
{
    if (parent == tracksNode)
        rebuildStrips();
}

void MixView::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int)
{
    if (parent == tracksNode)
        rebuildStrips();
}

void MixView::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    if (parent == tracksNode)
        rebuildStrips();
}

void MixView::visibilityChanged()
{
    if (isVisible())
    {
        master.refresh();
        startTimerHz (30);
    }
    else
    {
        stopTimer();
    }
}

void MixView::timerCallback()
{
    for (auto* s : strips)
        s->updateMeter();
    master.updateMeter();

    // Master settings and bus names can change without this view (undo, other
    // strips, session load): resync at 5 Hz.
    if (++refreshCounter % 6 == 0)
    {
        master.refresh();
        for (auto* s : strips)
            s->refreshRouting();
    }
}

void MixView::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);
}

void MixView::resized()
{
    auto area = getLocalBounds();
    master.setBounds (area.removeFromRight (MasterStrip::stripWidth));
    viewport.setBounds (area);
    emptyHint.setBounds (area);
    layoutStrips();
}

} // namespace meridian
