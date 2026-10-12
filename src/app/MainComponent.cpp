#include "MainComponent.h"

#include "CommandIds.h"

#include "plugins/PluginWindow.h"
#include "ui/dialogs/DashboardComponent.h"
#include "ui/dialogs/NewTracksComponent.h"
#include "ui/dialogs/PlaybackEngineComponent.h"
#include "ui/Theme.h"

namespace meridian
{
namespace
{
constexpr int transportHeight = 58;

void showMessage (juce::MessageBoxIconType icon, const juce::String& title, const juce::String& message)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions::makeOptionsOk (icon, title, message), nullptr);
}
} // namespace

MainComponent::MainComponent (AppController& a)
    : app (a),
      transportBar (a, [this] (WindowKind w) { showWindow (w); }, [this] { return activeWindow; }),
      editView (a),
      mixView (a)
{
    addAndMakeVisible (transportBar);
    addAndMakeVisible (editView);
    addChildComponent (mixView);

    openDashboardButton.onClick = [this] { showDashboard(); };
    addChildComponent (openDashboardButton);

    app.showProblem = [] (const juce::String& title, const juce::String& message)
    {
        showMessage (juce::MessageBoxIconType::WarningIcon, title, message);
    };
    app.toggleWindowHandler = [this]
    {
        showWindow (activeWindow == WindowKind::edit ? WindowKind::mix : WindowKind::edit);
        return activeWindow;
    };
    app.activeWindowQuery = [this] { return activeWindow; };

    commandManager.registerAllCommandsForTarget (this);
    commandManager.setFirstCommandTarget (this);
    addKeyListener (commandManager.getKeyMappings());
    setWantsKeyboardFocus (true);

    app.addChangeListener (this);
    changeListenerCallback (&app);
    startTimerHz (4);

    setSize (1440, 880);
}

MainComponent::~MainComponent()
{
    stopTimer();
    app.removeChangeListener (this);
    app.showProblem = nullptr;
    app.toggleWindowHandler = nullptr;
    app.activeWindowQuery = nullptr;
    PluginWindow::closeAll();
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    const bool hasSession = app.getSession() != nullptr;
    openDashboardButton.setVisible (! hasSession);
    editView.setVisible (hasSession && activeWindow == WindowKind::edit);
    mixView.setVisible (hasSession && activeWindow == WindowKind::mix);
    updateWindowTitle();
    commandManager.commandStatusChanged();
}

void MainComponent::timerCallback()
{
    updateWindowTitle();
}

void MainComponent::updateWindowTitle()
{
    juce::String title = JUCE_APPLICATION_NAME_STRING;
    if (auto* session = app.getSession())
    {
        title << " - " << session->getName() << (session->hasUnsavedChanges() ? " *" : "")
              << (activeWindow == WindowKind::edit ? "  [Edit]" : "  [Mix]");
    }
    if (title != lastTitle)
    {
        lastTitle = title;
        if (auto* window = getTopLevelComponent())
            window->setName (title);
    }
}

void MainComponent::showWindow (WindowKind kind)
{
    activeWindow = kind;
    const bool hasSession = app.getSession() != nullptr;
    editView.setVisible (hasSession && kind == WindowKind::edit);
    mixView.setVisible (hasSession && kind == WindowKind::mix);
    updateWindowTitle();
    commandManager.commandStatusChanged();
}

void MainComponent::runAfterUnsavedCheck (std::function<void()> action)
{
    auto* session = app.getSession();
    if (session == nullptr || ! session->hasUnsavedChanges())
    {
        action();
        return;
    }

    auto options = juce::MessageBoxOptions::makeOptionsYesNoCancel (
        juce::MessageBoxIconType::QuestionIcon, "Save Changes?",
        "Save changes to \"" + session->getName() + "\" before closing it?", "Save", "Don't Save", "Cancel");

    juce::AlertWindow::showAsync (options, [this, action] (int result)
    {
        if (result == 0)
            return; // cancel
        if (result == 1)
        {
            if (const auto saved = app.saveSession(); saved.failed())
            {
                showMessage (juce::MessageBoxIconType::WarningIcon, "Save Failed", saved.getErrorMessage());
                return;
            }
        }
        action();
    });
}

void MainComponent::showDashboard()
{
    DashboardComponent::show (app, [this] (std::function<void()> action) { runAfterUnsavedCheck (std::move (action)); });
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);
    if (app.getSession() == nullptr)
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (15.0f));
        g.drawText ("No session is open.", getLocalBounds().withTrimmedTop (transportHeight).translated (0, -30),
                    juce::Justification::centred);
    }
}

void MainComponent::resized()
{
    auto area = getLocalBounds();
    transportBar.setBounds (area.removeFromTop (transportHeight));
    editView.setBounds (area);
    mixView.setBounds (area);
    openDashboardButton.setBounds (area.withSizeKeepingCentre (180, 32).translated (0, 10));
}

//==============================================================================
void MainComponent::showImportDialog()
{
    if (app.getSession() == nullptr)
        return;

    chooser = std::make_unique<juce::FileChooser> ("Import Audio", juce::File(), app.getFormatManager().getWildcardForAllFormats());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto files = fc.getResults();
                              if (files.isEmpty())
                                  return;

                              juce::ValueTree target;
                              auto& state = editView.getEditState();
                              if (state.selectedTrackIds.size() == 1 && app.getSession() != nullptr)
                                  target = app.getSession()->getTrackById (state.selectedTrackIds.getFirst());
                              app.importAudioFiles (files, app.getEngine().getTransport().getPosition(), target);
                          });
}

void MainComponent::showBounceDialog()
{
    if (app.getSession() == nullptr)
        return;

    const auto suggested = app.makeSessionBounceRequest();
    chooser = std::make_unique<juce::FileChooser> ("Bounce Mix", suggested.destination, suggested.aiff ? "*.aif" : "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, suggested] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();
                              if (file == juce::File())
                                  return;

                              auto request = suggested;
                              request.destination = file.withFileExtension (suggested.aiff ? ".aif" : ".wav");
                              const auto started = app.startBounce (request, [] (const BounceResult& result)
                              {
                                  if (! result.ok)
                                  {
                                      showMessage (juce::MessageBoxIconType::WarningIcon, "Bounce Mix", result.error);
                                      return;
                                  }
                                  showMessage (juce::MessageBoxIconType::InfoIcon, "Bounce Complete",
                                               result.file.getFullPathName() + "\n\nPeak: L "
                                                   + juce::String (dsp::gainToDb (result.left.peak), 1) + " dBFS, R "
                                                   + juce::String (dsp::gainToDb (result.right.peak), 1) + " dBFS"
                                                   + (juce::jmax (result.left.peak, result.right.peak) > 1.0
                                                          ? juce::String ("\n\nWarning: the mix exceeds 0 dBFS and will clip in the file. Lower the master fader.")
                                                          : juce::String()));
                              });
                              if (started.failed())
                                  showMessage (juce::MessageBoxIconType::WarningIcon, "Bounce Mix", started.getErrorMessage());
                          });
}

void MainComponent::openSessionFromDisk()
{
    chooser = std::make_unique<juce::FileChooser> ("Open Session", app.getDefaultSessionsFolder(),
                                                   juce::String ("*") + Session::fileExtension);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (file == juce::File())
                                  return;
                              runAfterUnsavedCheck ([this, file]
                              {
                                  if (const auto result = app.openSession (file); result.failed())
                                      showMessage (juce::MessageBoxIconType::WarningIcon, "Open Session", result.getErrorMessage());
                              });
                          });
}

void MainComponent::showRemoteControlInfo()
{
    const auto& server = app.getRemoteServer();
    juce::String text;
    if (server.isRunning())
    {
        text << "AI remote control is listening on:\n\n"
             << "    ws://127.0.0.1:" << server.getPort() << "/\n\n"
             << "Only programs on this computer can connect. Browser pages are refused unless their origin is\n"
             << "allowed. Set MERIDIAN_REMOTE_TOKEN to require ?token=... from clients.\n\n"
             << "Send one command per text message; every reply is JSON:\n\n"
             << "    PLAY   STOP   RECORD   TOGGLE_WINDOW   STATUS   HELP\n"
             << "    MUTE_TRACK_2\n"
             << "    SET_VOLUME_TRACK_1_-6.5dB\n\n"
             << "Connected clients: " << server.getNumClients();
    }
    else
    {
        text << "AI remote control is off. Turn it on in Setup > Enable AI Remote Control.";
    }
    showMessage (juce::MessageBoxIconType::InfoIcon, "AI Remote Control", text);
}

//==============================================================================
void MainComponent::getAllCommands (juce::Array<juce::CommandID>& c)
{
    using namespace commands;
    c.addArray ({ dashboard, openSession, saveSession, importAudio, bounceMix,
                  undo, redo, separateClip, deleteSelection, elasticAudio, zoomIn, zoomOut,
                  slipMode, gridMode, trimTool, selectorTool, grabberTool, smartTool, pencilTool,
                  newTracks, deleteTrack,
                  playStop, record, returnToZero,
                  playbackEngine, pluginManager, toggleRemoteControl, remoteControlInfo,
                  toggleWindow, showEditWindow, showMixWindow });
}

void MainComponent::getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    using namespace commands;
    using K = juce::KeyPress;
    const auto cmd = juce::ModifierKeys::commandModifier;
    const auto shift = juce::ModifierKeys::shiftModifier;
    const bool hasSession = app.getSession() != nullptr;
    auto& edit = editView.getEditState();

    switch (id)
    {
        case dashboard:        info.setInfo ("Dashboard...", "Create or open a session", "File", 0);
                               info.addDefaultKeypress ('n', cmd); break;
        case openSession:      info.setInfo ("Open Session...", "Open a session from disk", "File", 0);
                               info.addDefaultKeypress ('o', cmd); break;
        case saveSession:      info.setInfo ("Save", "Save the session", "File", 0);
                               info.addDefaultKeypress ('s', cmd); info.setActive (hasSession); break;
        case importAudio:      info.setInfo ("Import Audio...", "Copy audio files into the session", "File", 0);
                               info.addDefaultKeypress ('i', cmd | shift); info.setActive (hasSession); break;
        case bounceMix:        info.setInfo ("Bounce Mix...", "Mix the session down to an audio file", "File", 0);
                               info.addDefaultKeypress ('b', cmd | juce::ModifierKeys::altModifier);
                               info.setActive (hasSession && ! app.isBouncing()); break;

        case undo:
        {
            auto* um = app.getUndoManager();
            info.setInfo (um != nullptr && um->canUndo() ? "Undo " + um->getUndoDescription() : juce::String ("Undo"),
                          "Undo the last edit", "Edit", 0);
            info.addDefaultKeypress ('z', cmd);
            info.setActive (um != nullptr && um->canUndo());
            break;
        }
        case redo:
        {
            auto* um = app.getUndoManager();
            info.setInfo (um != nullptr && um->canRedo() ? "Redo " + um->getRedoDescription() : juce::String ("Redo"),
                          "Redo", "Edit", 0);
            info.addDefaultKeypress ('z', cmd | shift);
            info.addDefaultKeypress ('y', cmd);
            info.setActive (um != nullptr && um->canRedo());
            break;
        }
        case separateClip:     info.setInfo ("Separate Clip", "Split clips at the selection or cursor", "Edit", 0);
                               info.addDefaultKeypress ('e', cmd); info.setActive (hasSession); break;
        case deleteSelection:  info.setInfo ("Delete", "Delete the selected clip or time range", "Edit", 0);
                               info.addDefaultKeypress (K::deleteKey, 0); info.addDefaultKeypress (K::backspaceKey, 0);
                               info.setActive (hasSession); break;
        case elasticAudio:     info.setInfo ("Elastic Audio (Time / Pitch)...", "Time-stretch or pitch-shift the selected clip", "Edit", 0);
                               info.addDefaultKeypress ('t', cmd | shift); info.setActive (edit.selectedClip.isValid()); break;
        case zoomIn:           info.setInfo ("Zoom In", "Zoom in horizontally", "Edit", 0);
                               info.addDefaultKeypress (']', cmd); break;
        case zoomOut:          info.setInfo ("Zoom Out", "Zoom out horizontally", "Edit", 0);
                               info.addDefaultKeypress ('[', cmd); break;

        case slipMode:         info.setInfo ("Slip Mode", "Clips move freely", "Tools", 0);
                               info.addDefaultKeypress (K::F2Key, 0); info.setTicked (edit.mode == EditMode::slip); break;
        case gridMode:         info.setInfo ("Grid Mode", "Edits snap to the grid", "Tools", 0);
                               info.addDefaultKeypress (K::F4Key, 0); info.setTicked (edit.mode == EditMode::grid); break;
        case trimTool:         info.setInfo ("Trim Tool", "", "Tools", 0);
                               info.addDefaultKeypress (K::F6Key, 0); info.setTicked (edit.tool == EditTool::trim); break;
        case selectorTool:     info.setInfo ("Selector Tool", "", "Tools", 0);
                               info.addDefaultKeypress (K::F7Key, 0); info.setTicked (edit.tool == EditTool::selector); break;
        case grabberTool:      info.setInfo ("Grabber Tool", "", "Tools", 0);
                               info.addDefaultKeypress (K::F8Key, 0); info.setTicked (edit.tool == EditTool::grabber); break;
        case smartTool:        info.setInfo ("Smart Tool", "", "Tools", 0);
                               info.addDefaultKeypress (K::F9Key, 0); info.setTicked (edit.tool == EditTool::smart); break;
        case pencilTool:       info.setInfo ("Pencil (draw automation)", "", "Tools", 0);
                               info.addDefaultKeypress (K::F10Key, 0); info.setTicked (edit.tool == EditTool::pencil); break;

        case newTracks:        info.setInfo ("New Track...", "Create new tracks", "Track", 0);
                               info.addDefaultKeypress ('n', cmd | shift); info.setActive (hasSession); break;
        case deleteTrack:      info.setInfo ("Delete Selected Track", "", "Track", 0);
                               info.setActive (hasSession && edit.selectedTrackIds.size() == 1); break;

        case playStop:         info.setInfo (app.isRolling() ? "Stop" : "Play", "Start or stop the transport", "Transport", 0);
                               info.addDefaultKeypress (K::spaceKey, 0); info.setActive (hasSession); break;
        case record:           info.setInfo ("Record", "Record on armed tracks", "Transport", 0);
                               info.addDefaultKeypress (K::F12Key, 0); info.addDefaultKeypress (K::spaceKey, cmd);
                               info.setActive (hasSession); break;
        case returnToZero:     info.setInfo ("Return to Zero", "Move the cursor to the session start", "Transport", 0);
                               info.addDefaultKeypress (K::homeKey, 0); info.addDefaultKeypress (K::returnKey, 0); break;

        case playbackEngine:   info.setInfo ("Playback Engine...", "Audio device, buffer size and engine options", "Setup", 0); break;
        case pluginManager:    info.setInfo ("Plug-in Manager...", "Scan for and manage plug-ins", "Setup", 0); break;
        case toggleRemoteControl:
                               info.setInfo ("Enable AI Remote Control", "Listen for commands on 127.0.0.1", "Setup", 0);
                               info.setTicked (app.getRemoteServer().isRunning()); break;
        case remoteControlInfo:info.setInfo ("AI Remote Control Info...", "Connection details and commands", "Setup", 0); break;

        case toggleWindow:     info.setInfo ("Toggle Edit / Mix", "Swap the Edit and Mix windows", "Window", 0);
                               info.addDefaultKeypress ('=', cmd); break;
        case showEditWindow:   info.setInfo ("Edit", "", "Window", 0); info.setTicked (activeWindow == WindowKind::edit); break;
        case showMixWindow:    info.setInfo ("Mix", "", "Window", 0); info.setTicked (activeWindow == WindowKind::mix); break;

        default: break;
    }
}

bool MainComponent::perform (const InvocationInfo& info)
{
    using namespace commands;
    auto& edit = editView.getEditState();

    switch (info.commandID)
    {
        case dashboard:       showDashboard(); break;
        case openSession:     openSessionFromDisk(); break;
        case saveSession:
            if (const auto result = app.saveSession(); result.failed())
                showMessage (juce::MessageBoxIconType::WarningIcon, "Save Failed", result.getErrorMessage());
            break;
        case importAudio:     showImportDialog(); break;
        case bounceMix:       showBounceDialog(); break;

        case undo:            if (auto* um = app.getUndoManager()) um->undo(); break;
        case redo:            if (auto* um = app.getUndoManager()) um->redo(); break;
        case separateClip:    editView.separateAtSelection(); break;
        case deleteSelection: editView.deleteSelection(); break;
        case elasticAudio:    editView.openElasticDialog ({}); break;
        case zoomIn:          editView.zoomBy (0.5); break;
        case zoomOut:         editView.zoomBy (2.0); break;

        case slipMode:        edit.setMode (EditMode::slip); break;
        case gridMode:        edit.setMode (EditMode::grid); break;
        case trimTool:        edit.setTool (EditTool::trim); break;
        case selectorTool:    edit.setTool (EditTool::selector); break;
        case grabberTool:     edit.setTool (EditTool::grabber); break;
        case smartTool:       edit.setTool (EditTool::smart); break;
        case pencilTool:      edit.setTool (EditTool::pencil); break;

        case newTracks:       NewTracksComponent::show (app); break;
        case deleteTrack:
            if (auto* session = app.getSession(); session != nullptr && edit.selectedTrackIds.size() == 1)
                app.deleteTrack (session->getTrackById (edit.selectedTrackIds.getFirst()));
            break;

        case playStop:        app.togglePlay(); break;
        case record:
            if (app.isRecording())
                app.transportStop();
            else if (const auto result = app.transportRecord(); result.failed())
                showMessage (juce::MessageBoxIconType::WarningIcon, "Record", result.getErrorMessage());
            break;
        case returnToZero:    app.returnToZero(); break;

        case playbackEngine:  PlaybackEngineComponent::show (app); break;
        case pluginManager:   app.getPluginHost().showPluginManager(); break;
        case toggleRemoteControl:
            if (app.getRemoteServer().isRunning())
                app.stopRemoteControl();
            else if (const auto result = app.startRemoteControl(); result.failed())
                showMessage (juce::MessageBoxIconType::WarningIcon, "AI Remote Control", result.getErrorMessage());
            break;
        case remoteControlInfo: showRemoteControlInfo(); break;

        case toggleWindow:    showWindow (activeWindow == WindowKind::edit ? WindowKind::mix : WindowKind::edit); break;
        case showEditWindow:  showWindow (WindowKind::edit); break;
        case showMixWindow:   showWindow (WindowKind::mix); break;

        default: return false;
    }

    commandManager.commandStatusChanged();
    return true;
}

//==============================================================================
juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Edit", "Track", "Transport", "Setup", "Window" };
}

juce::PopupMenu MainComponent::getMenuForIndex (int index, const juce::String&)
{
    using namespace commands;
    juce::PopupMenu menu;
    auto add = [&] (juce::CommandID id) { menu.addCommandItem (&commandManager, id); };

    switch (index)
    {
        case 0:
            add (dashboard); add (openSession);
            menu.addSeparator();
            add (saveSession);
            menu.addSeparator();
            add (importAudio);
            add (bounceMix);
            menu.addSeparator();
            menu.addCommandItem (&commandManager, juce::StandardApplicationCommandIDs::quit);
            break;
        case 1:
        {
            add (undo); add (redo);
            menu.addSeparator();
            add (separateClip); add (deleteSelection);
            menu.addSeparator();
            add (elasticAudio);
            menu.addSeparator();
            add (zoomIn); add (zoomOut);
            menu.addSeparator();
            juce::PopupMenu tools;
            for (const auto id : { slipMode, gridMode })
                tools.addCommandItem (&commandManager, id);
            tools.addSeparator();
            for (const auto id : { trimTool, selectorTool, grabberTool, smartTool, pencilTool })
                tools.addCommandItem (&commandManager, id);
            menu.addSubMenu ("Modes and Tools", tools);
            break;
        }
        case 2: add (newTracks); add (deleteTrack); break;
        case 3: add (playStop); add (record); add (returnToZero); break;
        case 4:
            add (playbackEngine); add (pluginManager);
            menu.addSeparator();
            add (toggleRemoteControl); add (remoteControlInfo);
            break;
        case 5: add (toggleWindow); menu.addSeparator(); add (showEditWindow); add (showMixWindow); break;
        default: break;
    }
    return menu;
}

} // namespace meridian
