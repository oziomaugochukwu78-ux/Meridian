#pragma once

#include "AppController.h"

#include "ui/MeridianLookAndFeel.h"
#include "ui/TransportBar.h"
#include "ui/edit/EditView.h"
#include "ui/mix/MixView.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** Root component of the main window: transport bar plus the Edit or Mix
    window (one shown at a time, swapped with Ctrl+= or TOGGLE_WINDOW).
    Owns the menu bar and every keyboard command. */
class MainComponent final : public juce::Component,
                            public juce::ApplicationCommandTarget,
                            public juce::MenuBarModel,
                            private juce::ChangeListener,
                            private juce::Timer
{
public:
    explicit MainComponent (AppController&);
    ~MainComponent() override;

    juce::ApplicationCommandManager& getCommandManager() noexcept { return commandManager; }
    EditView& getEditView() noexcept { return editView; }
    MixView& getMixView() noexcept   { return mixView; }

    void showWindow (WindowKind);
    WindowKind getActiveWindow() const noexcept { return activeWindow; }

    /** Runs `action` once the user has saved or discarded unsaved changes
        (or immediately if there are none). Does nothing if they cancel. */
    void runAfterUnsavedCheck (std::function<void()> action);

    void showDashboard();

    void paint (juce::Graphics&) override;
    void resized() override;

    // ApplicationCommandTarget
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands (juce::Array<juce::CommandID>&) override;
    void getCommandInfo (juce::CommandID, juce::ApplicationCommandInfo&) override;
    bool perform (const InvocationInfo&) override;

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int topLevelMenuIndex, const juce::String& menuName) override;
    void menuItemSelected (int, int) override {}

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void updateWindowTitle();
    void showImportDialog();
    void showBounceDialog();
    void openSessionFromDisk();
    void showSaveAsDialog();
    void showGroupDialog();
    void showStripSilenceDialog();
    void offerRecovery (const juce::File& backup);
    void showRemoteControlInfo();

    AppController& app;
    juce::ApplicationCommandManager commandManager;
    juce::TooltipWindow tooltipWindow { this, 600 };

    TransportBar transportBar;
    EditView editView;
    MixView mixView;
    juce::TextButton openDashboardButton { "Open Dashboard..." };
    WindowKind activeWindow = WindowKind::edit;

    std::unique_ptr<juce::FileChooser> chooser;
    juce::String lastTitle;
};

} // namespace meridian
