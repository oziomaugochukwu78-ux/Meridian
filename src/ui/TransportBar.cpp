#include "TransportBar.h"
#include "model/MidiModel.h"

#include "Theme.h"
#include "TimeFormat.h"

namespace meridian
{

class TransportBar::TransportButton final : public juce::Button
{
public:
    enum class Icon { returnToZero, stop, play, record };

    TransportButton (Icon i, const juce::String& tooltip) : juce::Button (tooltip), icon (i)
    {
        setTooltip (tooltip);
    }

    void setLit (bool shouldBeLit)
    {
        if (lit != shouldBeLit)
        {
            lit = shouldBeLit;
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool isHighlighted, bool isDown) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (1.0f);
        const auto litColour = icon == Icon::record ? theme::record : icon == Icon::play ? theme::play : theme::panelHover;

        auto fill = lit ? litColour : theme::panelRaised;
        if (isDown)
            fill = fill.darker (0.2f);
        else if (isHighlighted)
            fill = fill.brighter (0.1f);

        g.setColour (fill);
        g.fillRoundedRectangle (bounds, 3.0f);
        g.setColour (theme::divider);
        g.drawRoundedRectangle (bounds, 3.0f, 1.0f);

        const auto glyph = bounds.withSizeKeepingCentre (12.0f, 12.0f);
        g.setColour (lit ? theme::textOnLight : (icon == Icon::record ? theme::record : theme::text));

        juce::Path p;
        switch (icon)
        {
            case Icon::returnToZero:
                p.addRectangle (glyph.getX(), glyph.getY(), 2.0f, glyph.getHeight());
                p.addTriangle (glyph.getRight(), glyph.getY(), glyph.getRight(), glyph.getBottom(), glyph.getX() + 3.0f, glyph.getCentreY());
                break;
            case Icon::stop:
                p.addRectangle (glyph.reduced (1.0f));
                break;
            case Icon::play:
                p.addTriangle (glyph.getX() + 1.0f, glyph.getY(), glyph.getX() + 1.0f, glyph.getBottom(), glyph.getRight(), glyph.getCentreY());
                break;
            case Icon::record:
                p.addEllipse (glyph);
                break;
        }
        g.fillPath (p);
    }

private:
    Icon icon;
    bool lit = false;
};

//==============================================================================
TransportBar::TransportBar (AppController& a, std::function<void (WindowKind)> show, std::function<WindowKind()> current)
    : app (a), showWindow (std::move (show)), currentWindow (std::move (current))
{
    using Icon = TransportButton::Icon;
    rtzButton = std::make_unique<TransportButton> (Icon::returnToZero, "Return to Zero (Home)");
    stopButton = std::make_unique<TransportButton> (Icon::stop, "Stop (Space)");
    playButton = std::make_unique<TransportButton> (Icon::play, "Play (Space)");
    recordButton = std::make_unique<TransportButton> (Icon::record, "Record (F12 or Ctrl+Space)");

    rtzButton->onClick = [this] { app.returnToZero(); };
    stopButton->onClick = [this] { app.transportStop(); };
    playButton->onClick = [this] { app.togglePlay(); };
    recordButton->onClick = [this]
    {
        if (app.isRecording())
            app.transportStop();
        else if (const auto result = app.transportRecord(); result.failed() && app.showProblem)
            app.showProblem ("Record", result.getErrorMessage());
    };

    for (auto* b : { rtzButton.get(), stopButton.get(), playButton.get(), recordButton.get() })
        addAndMakeVisible (b);

    for (auto* b : { &editButton, &mixButton })
    {
        b->setClickingTogglesState (false);
        b->setColour (juce::TextButton::buttonOnColourId, theme::text);
        addAndMakeVisible (b);
    }
    editButton.setTooltip ("Edit window (Ctrl+=)");
    mixButton.setTooltip ("Mix window (Ctrl+=)");
    editButton.onClick = [this] { showWindow (WindowKind::edit); };
    mixButton.onClick = [this] { showWindow (WindowKind::mix); };

    startTimerHz (30);
}

TransportBar::~TransportBar() = default;

void TransportBar::timerCallback()
{
    const auto& transport = app.getEngine().getTransport();
    const double sr = app.getEngine().getSampleRate();
    const auto position = transport.getPosition();

    playButton->setLit (transport.getState() != Transport::State::stopped);
    recordButton->setLit (transport.getState() == Transport::State::recording);

    const auto window = currentWindow();
    editButton.setToggleState (window == WindowKind::edit, juce::dontSendNotification);
    mixButton.setToggleState (window == WindowKind::mix, juce::dontSendNotification);

    mainCounter = timeformat::minSecs (position, sr);
    if (auto* session = app.getSession())
        subCounter = timeformat::barsBeats (position, sr, midimodel::tempoMap (*session), midimodel::meterMap (*session));
    else
        subCounter = timeformat::barsBeats (position, sr);
    subCounter << "    " << juce::String (position) << " smp";

    cpuLoad = (float) app.getDeviceManager().getCpuUsage();
    if (app.getEngine().collectDiskUnderruns() > 0)
        diskWarningFrames = 45; // keep the warning up for ~1.5 s

    juce::StringArray parts;
    parts.add ("CPU " + juce::String (juce::roundToInt (cpuLoad * 100.0f)) + "%");
    if (diskWarningFrames > 0)
    {
        --diskWarningFrames;
        parts.add ("DISK TOO SLOW");
    }

    const auto& server = app.getRemoteServer();
    if (server.isRunning())
    {
        const int clients = server.getNumClients();
        parts.add ("AI Remote 127.0.0.1:" + juce::String (server.getPort()) + " - "
                   + (clients == 0 ? juce::String ("waiting") : juce::String (clients) + (clients == 1 ? " client" : " clients")));
    }
    else
    {
        parts.add ("AI Remote off");
    }

    if (const int jobs = app.getNumBackgroundJobs(); jobs > 0)
        parts.add ("Processing " + juce::String (jobs) + (jobs == 1 ? " file..." : " files..."));

    statusText = parts.joinIntoString ("   |   ");
    repaint (counterArea);
    repaint (statusArea);
}

void TransportBar::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::divider);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

    // Counter well
    g.setColour (theme::well);
    g.fillRoundedRectangle (counterArea.toFloat(), 3.0f);
    auto c = counterArea.reduced (10, 4);
    g.setColour (theme::text);
    g.setFont (theme::monoFont (24.0f));
    g.drawText (mainCounter, c.removeFromTop (28), juce::Justification::centredLeft);
    g.setColour (theme::textDim);
    g.setFont (theme::monoFont (12.0f));
    g.drawText (subCounter, c, juce::Justification::centredLeft);

    g.setFont (theme::font (12.0f));
    g.setColour (diskWarningFrames > 0 ? theme::meterOrange : theme::textDim);
    g.drawText (statusText, statusArea, juce::Justification::centredRight);
}

void TransportBar::resized()
{
    auto area = getLocalBounds().reduced (8, 6);

    auto windowButtons = area.removeFromLeft (120);
    editButton.setBounds (windowButtons.removeFromLeft (58).withSizeKeepingCentre (56, 26));
    mixButton.setBounds (windowButtons.withSizeKeepingCentre (56, 26));

    area.removeFromLeft (16);
    for (auto* b : { rtzButton.get(), stopButton.get(), playButton.get(), recordButton.get() })
    {
        b->setBounds (area.removeFromLeft (40).withSizeKeepingCentre (36, 30));
        area.removeFromLeft (4);
    }

    area.removeFromLeft (16);
    counterArea = area.removeFromLeft (300);
    area.removeFromLeft (16);
    statusArea = area;
}

} // namespace meridian
