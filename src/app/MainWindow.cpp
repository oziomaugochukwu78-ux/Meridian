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

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

} // namespace meridian
