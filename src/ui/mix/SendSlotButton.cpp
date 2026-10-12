#include "SendSlotButton.h"

#include "model/Automation.h"

#include "model/Routing.h"
#include "ui/Theme.h"

#include <dsp/FaderTaper.h>

namespace meridian
{
namespace
{
constexpr int masterItem = 1;
constexpr int newAuxItem = 2;
constexpr int firstBusItem = 1000;

juce::String trackNameFor (const Session& session, int id)
{
    const auto t = session.getTrackById (id);
    return t.isValid() ? track::name (t) : juce::String ("(missing bus)");
}

/** The panel a send slot opens: everything about one send. */
class SendPanel final : public juce::Component,
                        private juce::ValueTree::Listener
{
public:
    SendPanel (AppController& a, juce::ValueTree sendNode) : app (a), node (std::move (sendNode))
    {
        title.setFont (theme::font (13.0f, true));
        addAndMakeVisible (title);

        level.setSliderStyle (juce::Slider::LinearHorizontal);
        level.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        level.setRange (0.0, 1.0);
        level.setDoubleClickReturnValue (true, dsp::FaderTaper::unityPosition);
        level.onDragStart = [this]
        {
            app.getAutomationRecorder().setTouched (track::id (send::owningTrack (node)), automation::sendParameter (send::slot (node)), true);
            begin ("Send Level");
        };
        level.onDragEnd = [this]
        {
            app.getAutomationRecorder().setTouched (track::id (send::owningTrack (node)), automation::sendParameter (send::slot (node)), false);
        };
        level.onValueChange = [this] { set (ids::levelDb, dsp::FaderTaper::positionToDb (level.getValue())); };
        addAndMakeVisible (level);

        levelReadout.setFont (theme::monoFont (11.0f));
        levelReadout.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (levelReadout);

        pan.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        pan.setRange (-1.0, 1.0);
        pan.setDoubleClickReturnValue (true, 0.0);
        pan.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        pan.onDragStart = [this] { begin ("Send Pan"); };
        pan.onValueChange = [this] { set (ids::pan, pan.getValue()); };
        addAndMakeVisible (pan);

        preFader.setClickingTogglesState (false);
        preFader.setColour (juce::TextButton::buttonOnColourId, theme::automation);
        preFader.setTooltip ("Pre-fader: the send ignores the track's volume fader (for headphone mixes). "
                             "Post-fader (off): the send follows the fader (for effects like reverb).");
        preFader.onClick = [this] { begin ("Send Pre/Post"); set (ids::preFader, ! send::preFader (node)); };
        addAndMakeVisible (preFader);

        mute.setClickingTogglesState (false);
        mute.setColour (juce::TextButton::buttonOnColourId, theme::mute);
        mute.setColour (juce::TextButton::textColourOnId, theme::textOnLight);
        mute.onClick = [this] { begin ("Send Mute"); set (ids::mute, ! send::mute (node)); };
        addAndMakeVisible (mute);

        remove.onClick = [this]
        {
            auto t = send::owningTrack (node);
            if (auto* um = app.getUndoManager())
            {
                um->beginNewTransaction ("Remove Send");
                routing::removeSend (t, send::slot (node), um);
            }
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
                box->dismiss();
        };
        addAndMakeVisible (remove);

        node.addListener (this);
        refresh();
        setSize (240, 112);
    }

    ~SendPanel() override { node.removeListener (this); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10, 8);
        title.setBounds (area.removeFromTop (20));
        area.removeFromTop (4);
        auto row = area.removeFromTop (24);
        levelReadout.setBounds (row.removeFromRight (52));
        level.setBounds (row);
        area.removeFromTop (6);
        row = area.removeFromTop (40);
        pan.setBounds (row.removeFromLeft (40));
        row.removeFromLeft (8);
        preFader.setBounds (row.removeFromLeft (50).withSizeKeepingCentre (50, 24));
        row.removeFromLeft (6);
        mute.setBounds (row.removeFromLeft (30).withSizeKeepingCentre (30, 24));
        row.removeFromLeft (6);
        remove.setBounds (row.withSizeKeepingCentre (row.getWidth(), 24));
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }

private:
    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&) override
    {
        if (tree == node)
            refresh();
    }

    void begin (const juce::String& name)
    {
        if (auto* um = app.getUndoManager())
            um->beginNewTransaction (name);
    }

    void set (const juce::Identifier& property, const juce::var& value)
    {
        node.setProperty (property, value, app.getUndoManager());
    }

    void refresh()
    {
        const auto* session = app.getSession();
        title.setText ("Send " + juce::String::charToString ((juce::juce_wchar) ('A' + send::slot (node))) + "  to  "
                           + (session != nullptr ? trackNameFor (*session, send::target (node)) : juce::String()),
                       juce::dontSendNotification);

        const double db = send::levelDb (node);
        level.setValue (dsp::FaderTaper::dbToPosition (db), juce::dontSendNotification);
        levelReadout.setText (db <= dsp::FaderTaper::minDb ? juce::String ("-inf") : juce::String (db, 1), juce::dontSendNotification);
        pan.setValue (send::pan (node), juce::dontSendNotification);
        preFader.setToggleState (send::preFader (node), juce::dontSendNotification);
        mute.setToggleState (send::mute (node), juce::dontSendNotification);
    }

    AppController& app;
    juce::ValueTree node;
    juce::Label title, levelReadout;
    juce::Slider level, pan;
    juce::TextButton preFader { "PRE" }, mute { "M" }, remove { "Remove" };
};
} // namespace

//==============================================================================
void showBusMenu (AppController& app, juce::Component& target, const juce::ValueTree& t, bool includeMaster,
                  std::function<void (int)> onChosen)
{
    auto* session = app.getSession();
    if (session == nullptr)
        return;

    juce::PopupMenu menu;
    if (includeMaster)
        menu.addItem (masterItem, "Main Out (master)", true, track::outputBus (t) == 0);
    menu.addSectionHeader ("Buses");

    const auto targets = routing::validTargets (*session, t);
    for (const auto& aux : targets)
        menu.addItem (firstBusItem + track::id (aux), track::name (aux), true, includeMaster && track::outputBus (t) == track::id (aux));
    if (targets.isEmpty())
        menu.addItem (-1, "(no aux tracks yet)", false);

    menu.addSeparator();
    menu.addItem (newAuxItem, "New Aux Track...");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target),
                        [&app, onChosen] (int result)
                        {
                            auto* s = app.getSession();
                            if (s == nullptr || result == 0)
                                return;
                            if (result == masterItem)
                            {
                                onChosen (0);
                                return;
                            }
                            if (result == newAuxItem)
                            {
                                int n = 1;
                                while (s->getTracksNode().getChildWithProperty (ids::name, "Aux " + juce::String (n)).isValid())
                                    ++n;
                                s->getUndoManager().beginNewTransaction ("New Aux Track");
                                auto aux = s->addTrack ("Aux " + juce::String (n), 2, &s->getUndoManager(), Session::TrackKind::aux);
                                onChosen (track::id (aux));
                                return;
                            }
                            onChosen (result - firstBusItem);
                        });
}

//==============================================================================
SendSlotButton::SendSlotButton (AppController& a, juce::ValueTree t, int slotIndex)
    : app (a), trackNode (std::move (t)), slot (slotIndex)
{
    setTooltip ("Send " + juce::String::charToString ((juce::juce_wchar) ('A' + slot))
                + ": click to send this track to a bus (e.g. a shared reverb)");
}

void SendSlotButton::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (1.0f, 0.5f);
    const auto node = routing::findSend (trackNode, slot);
    auto* session = app.getSession();

    g.setColour (node.isValid() ? (send::mute (node) ? theme::panelRaised.darker (0.3f) : theme::panelRaised) : theme::well);
    g.fillRect (bounds);

    if (node.isValid() && session != nullptr)
    {
        // Level bar along the bottom: fader travel of the send.
        const auto position = (float) dsp::FaderTaper::dbToPosition (send::levelDb (node));
        g.setColour (send::preFader (node) ? theme::automation : theme::meterGreen.withAlpha (0.8f));
        g.fillRect (bounds.withTop (bounds.getBottom() - 3.0f).withWidth (bounds.getWidth() * position));

        g.setColour (send::mute (node) ? theme::textDim : theme::text);
        g.setFont (theme::font (11.0f));
        g.drawText (trackNameFor (*session, send::target (node)), bounds.reduced (3.0f, 0.0f).withTrimmedBottom (2.0f),
                    juce::Justification::centredLeft, true);
    }

    g.setColour (theme::divider);
    g.drawRect (bounds, 1.0f);
}

void SendSlotButton::mouseDown (const juce::MouseEvent& e)
{
    const auto node = routing::findSend (trackNode, slot);
    if (! node.isValid() || e.mods.isPopupMenu())
        chooseTarget();
    else
        openPanel();
}

void SendSlotButton::chooseTarget()
{
    showBusMenu (app, *this, trackNode, false,
                 [safe = juce::Component::SafePointer<SendSlotButton> (this)] (int auxId)
                 {
                     if (safe == nullptr || auxId == 0)
                         return;
                     auto* session = safe->app.getSession();
                     auto& um = session->getUndoManager();
                     um.beginNewTransaction ("Assign Send");
                     if (routing::setSend (*session, safe->trackNode, safe->slot, auxId, &um))
                         safe->openPanel();
                     safe->repaint();
                 });
}

void SendSlotButton::openPanel()
{
    const auto node = routing::findSend (trackNode, slot);
    if (! node.isValid())
        return;
    juce::CallOutBox::launchAsynchronously (std::make_unique<SendPanel> (app, node), getScreenBounds(), nullptr);
}

} // namespace meridian
