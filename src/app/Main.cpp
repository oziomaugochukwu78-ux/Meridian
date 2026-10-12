#include "AppController.h"
#include "MainWindow.h"

#include "ui/MeridianLookAndFeel.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

class MeridianApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        controller = std::make_unique<AppController>();
        window = std::make_unique<MainWindow> (*controller);

        // A session file passed on the command line (e.g. double-clicked) opens
        // directly; otherwise show the Dashboard, as a console DAW does.
        const auto argument = commandLine.trim().unquoted(); // trim first: Windows passes a leading space
        if (argument.isNotEmpty() && juce::File::isAbsolutePath (argument) && juce::File (argument).existsAsFile())
        {
            if (const auto result = controller->openSession (juce::File (argument)); result.failed() && controller->showProblem)
                controller->showProblem ("Open Session", result.getErrorMessage());
        }
        else if (controller->getSettings().getBoolValue ("showDashboardOnStartup", true))
        {
            window->getMainComponent().showDashboard();
        }
        else if (const auto recent = controller->getRecentSessions().getFile (0); recent.existsAsFile())
        {
            controller->openSession (recent);
        }
    }

    void shutdown() override
    {
        window.reset();
        controller.reset();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override
    {
        if (window == nullptr)
        {
            quit();
            return;
        }
        window->getMainComponent().runAfterUnsavedCheck ([] { juce::JUCEApplication::getInstance()->quit(); });
    }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        const auto argument = commandLine.trim().unquoted();
        if (window == nullptr || ! juce::File::isAbsolutePath (argument))
            return;
        const juce::File file (argument);
        if (file.existsAsFile())
            window->getMainComponent().runAfterUnsavedCheck ([this, file] { controller->openSession (file); });
    }

private:
    MeridianLookAndFeel lookAndFeel;
    std::unique_ptr<AppController> controller;
    std::unique_ptr<MainWindow> window;
};

} // namespace meridian

START_JUCE_APPLICATION (meridian::MeridianApplication)
