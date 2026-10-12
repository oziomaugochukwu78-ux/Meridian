#include "AppController.h"
#include "MainWindow.h"
#include "SelfTest.h"

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

        // Meridian --self-test <folder>: run the end-to-end self test and exit
        // with 0 (all passed) or 1.
        const auto args = juce::StringArray::fromTokens (commandLine, true);
        if (const int index = args.indexOf ("--self-test"); index >= 0)
        {
            const auto folder = args[index + 1].unquoted();
            const auto outputFolder = juce::File::isAbsolutePath (folder)
                                          ? juce::File (folder)
                                          : juce::File::getCurrentWorkingDirectory().getChildFile (folder.isNotEmpty() ? folder : juce::String ("self-test"));
            const bool includeRecording = args.contains ("--test-recording"); // opt-in: uses the microphone
            selfTest = std::make_unique<SelfTest> (*controller, window->getMainComponent(), outputFolder, includeRecording,
                                                   [this] (int failures)
            {
                setApplicationReturnValue (failures == 0 ? 0 : 1);
                quit();
            });
            // --update-golden: rewrite the reference renders in tests/golden
            // (only after a deliberate, reviewed change to the sound).
            selfTest->setUpdateGoldenFiles (args.contains ("--update-golden"));
            // Start once the window is on screen (screenshots need it).
            juce::MessageManager::callAsync ([this] { if (selfTest != nullptr) selfTest->start(); });
            return;
        }

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
        selfTest.reset();
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
    std::unique_ptr<SelfTest> selfTest;
};

} // namespace meridian

START_JUCE_APPLICATION (meridian::MeridianApplication)
