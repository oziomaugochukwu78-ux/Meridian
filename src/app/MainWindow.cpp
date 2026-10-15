#include "MainWindow.h"

#include "ui/Theme.h"

namespace meridian
{

MainWindow::MainWindow (AppController& app)
    : juce::DocumentWindow (JUCE_APPLICATION_NAME_STRING, theme::window, juce::DocumentWindow::allButtons)
{
    setUsingNativeTitleBar (true);

    mainComponent = new MainComponent (app);
    setContentOwned (mainComponent, true);

    auto& commands = mainComponent->getCommandManager();
    commands.registerAllCommandsForTarget (juce::JUCEApplication::getInstance()); // Quit
    addKeyListener (commands.getKeyMappings());

#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu (mainComponent);
#else
    setMenuBar (mainComponent);
#endif

    setResizable (true, false);
    setResizeLimits (960, 600, 10000, 10000);
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
}

MainWindow::~MainWindow()
{
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu (nullptr);
#else
    setMenuBar (nullptr);
#endif
    clearContentComponent();
}

void MainWindow::fitToScreen()
{
    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    if (display == nullptr)
        return;

    // Our bounds exclude the native title bar and frame, so take those off the
    // work area (the screen minus the taskbar) before comparing.
    auto area = display->userBounds.getSmallestIntegerContainer();
    if (auto* peer = getPeer())
        if (const auto frame = peer->getFrameSizeIfPresent())
            area = (*frame).subtractedFrom (area);

    auto bounds = getBounds();
    if (bounds.getWidth() > area.getWidth() || bounds.getHeight() > area.getHeight())
    {
        setResizeLimits (juce::jmin (960, area.getWidth()), juce::jmin (600, area.getHeight()), 10000, 10000);
        bounds = area.withSizeKeepingCentre (juce::jmin (bounds.getWidth(), area.getWidth()),
                                             juce::jmin (bounds.getHeight(), area.getHeight()));
    }
    // A window that only just fits can still be centred with its title bar above
    // the screen or its bottom under the taskbar: move it wholly inside.
    bounds = bounds.constrainedWithin (area);
    if (bounds != getBounds())
        setBounds (bounds);
}

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

} // namespace meridian
