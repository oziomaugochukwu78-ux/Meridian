#include "AppController.h"
#include "MainWindow.h"
#include "SelfTest.h"
#include "SoakTest.h"

#include "model/SessionBackups.h"
#include "ui/MeridianLookAndFeel.h"

#include <cstdlib>

#include <juce_gui_basics/juce_gui_basics.h>

#if JUCE_WINDOWS && defined (_DEBUG)
 #include <crtdbg.h>
#endif

namespace meridian
{

class MeridianApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override
    {
        // The crash-recovery test runs a second copy next to the self test.
        return getCommandLineParameters().contains ("--crash-test-child");
    }

    void initialise (const juce::String& commandLine) override
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        const auto args = juce::StringArray::fromTokens (commandLine, true);

        // Meridian --crash-test-child <session> <ready file>: used only by the self
        // test. Opens the session, makes an edit it never saves, writes a backup,
        // signals, then waits to be killed (a hard kill: no clean shutdown).
        if (const int index = args.indexOf ("--crash-test-child"); index >= 0)
        {
            runCrashTestChild (juce::File (args[index + 1].unquoted()), juce::File (args[index + 2].unquoted()));
            return;
        }

        // Meridian --self-test <folder>: run the end-to-end self test and exit
        // with 0 (all passed) or 1.
        const int selfTestIndex = args.indexOf ("--self-test");
        if (selfTestIndex >= 0)
        {
            // Before anything else, so assertions during start-up are caught too.
            assertionLog = std::make_unique<AssertionLog>();
            juce::Logger::setCurrentLogger (assertionLog.get());
           #if JUCE_WINDOWS && defined (_DEBUG)
            // Unattended runs: C runtime assertions go to stderr and fail the
            // process instead of opening a dialog that waits forever.
            _CrtSetReportMode (_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
            _CrtSetReportFile (_CRT_ASSERT, _CRTDBG_FILE_STDERR);
            _CrtSetReportMode (_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
            _CrtSetReportFile (_CRT_ERROR, _CRTDBG_FILE_STDERR);
           #endif
        }

        controller = std::make_unique<AppController>();
        window = std::make_unique<MainWindow> (*controller);

        if (const int index = selfTestIndex; index >= 0)
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
            selfTest->setAllowNoAudioDevice (args.contains ("--allow-no-audio-device")); // CI servers
            selfTest->setAssertionLog (assertionLog.get());
            // Start once the window is on screen (screenshots need it).
            juce::MessageManager::callAsync ([this] { if (selfTest != nullptr) selfTest->start(); });
            return;
        }

        // Meridian --soak <minutes> <folder>: long-running stability test (silent).
        if (const int index = args.indexOf ("--soak"); index >= 0)
        {
            const auto folder = args[index + 2].unquoted();
            const auto outputFolder = juce::File::isAbsolutePath (folder)
                                          ? juce::File (folder)
                                          : juce::File::getCurrentWorkingDirectory().getChildFile (folder.isNotEmpty() ? folder : juce::String ("soak"));
            soakTest = std::make_unique<SoakTest> (*controller, outputFolder, args[index + 1].getDoubleValue(), [this] (int failures)
            {
                setApplicationReturnValue (failures == 0 ? 0 : 1);
                quit();
            });
            juce::MessageManager::callAsync ([this] { if (soakTest != nullptr) soakTest->start(); });
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
        soakTest.reset();
        window.reset();
        controller.reset();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        juce::Logger::setCurrentLogger (nullptr);
        assertionLog.reset();
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
    static void runCrashTestChild (const juce::File& sessionFile, const juce::File& readyFile)
    {
        juce::String error;
        auto session = Session::load (sessionFile, error);
        if (session == nullptr)
            std::_Exit (2);
        backups::markOpen (sessionFile);
        session->addTrack ("Unsaved Edit", 1, nullptr);
        if (backups::write (*session).failed())
            std::_Exit (3);
        readyFile.replaceWithText ("ready");

        // Wait for the kill. If the parent never sends it, leave anyway after a
        // minute, still without a clean shutdown, so no stray process remains.
        for (int i = 0; i < 600; ++i)
            juce::Thread::sleep (100);
        std::_Exit (4);
    }

    MeridianLookAndFeel lookAndFeel;
    std::unique_ptr<AssertionLog> assertionLog; // self test only
    std::unique_ptr<AppController> controller;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<SelfTest> selfTest;
    std::unique_ptr<SoakTest> soakTest;
};

} // namespace meridian

START_JUCE_APPLICATION (meridian::MeridianApplication)
