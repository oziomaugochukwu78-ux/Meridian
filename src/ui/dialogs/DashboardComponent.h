#pragma once

#include "app/AppController.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

/** File > Dashboard: the session launcher shown at startup.

    CREATE  name, "Create from template" (group + template list), or a blank
            session with File Type / Sample Rate / Bit Depth / I/O Settings /
            Interleaved, and the location to create it in.
    RECENT  recently opened sessions.

    Footer: "Show on startup", Open from Disk..., Cancel, Create / Open. */
class DashboardComponent final : public juce::Component,
                                 private juce::ListBoxModel
{
public:
    /** `guard` runs an action after the user has dealt with unsaved changes. */
    using UnsavedGuard = std::function<void (std::function<void()>)>;

    static void show (AppController&, UnsavedGuard guard);

    DashboardComponent (AppController&, UnsavedGuard guard);
    ~DashboardComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    enum class Page { create, recent };

    // ListBoxModel (used for both the template list and the recent list)
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void selectedRowsChanged (int) override;

    void showPage (Page);
    void refreshTemplateList();
    void updateEnablement();
    void createSession();
    void openSelectedRecent();
    void openFromDisk();
    void chooseLocation();
    std::vector<const SessionTemplate*> templatesInGroup() const;

    AppController& app;
    UnsavedGuard guard;
    Page page = Page::create;

    juce::TextButton createTab { "Create" }, recentTab { "Recent" };

    juce::Label nameLabel, settingsHeader, fileTypeLabel, sampleRateLabel, bitDepthLabel, ioLabel, locationHeader, locationPath, status;
    juce::TextEditor nameEditor;
    juce::ToggleButton fromTemplate { "Create From Template" }, interleaved { "Interleaved" };
    juce::ComboBox templateGroup, fileType, sampleRate, bitDepth, ioSettings;
    juce::ListBox templateList { "Templates", this };
    juce::ListBox recentList { "Recent", this };
    juce::TextButton chooseLocationButton { "Choose..." };

    juce::ToggleButton showOnStartup { "Show on startup" };
    juce::TextButton openFromDiskButton { "Open from Disk..." }, cancelButton { "Cancel" }, primaryButton { "Create" };

    juce::File location;
    std::unique_ptr<juce::FileChooser> chooser;
};

} // namespace meridian
