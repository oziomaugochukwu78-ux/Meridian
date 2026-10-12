#pragma once

#include "ChannelStrip.h"
#include "MasterStrip.h"

#include "app/AppController.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** The Mix window: a horizontally scrolling row of channel strips with the
    master strip fixed at the right edge. */
class MixView final : public juce::Component,
                      private juce::Timer,
                      private juce::ChangeListener,
                      private juce::ValueTree::Listener
{
public:
    explicit MixView (AppController&);
    ~MixView() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;

    void bindSession();
    void rebuildStrips();
    void layoutStrips();

    AppController& app;
    juce::Viewport viewport;
    juce::Component stripContainer;
    juce::OwnedArray<ChannelStrip> strips;
    MasterStrip master;
    juce::Label emptyHint;
    juce::ValueTree tracksNode;
    int refreshCounter = 0;
};

} // namespace meridian
