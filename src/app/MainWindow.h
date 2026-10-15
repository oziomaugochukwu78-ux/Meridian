#pragma once

#include "AppController.h"
#include "MainComponent.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian
{

class MainWindow final : public juce::DocumentWindow
{
public:
    explicit MainWindow (AppController&);
    ~MainWindow() override;

    MainComponent& getMainComponent() noexcept { return *mainComponent; }

    // Shrinks the window to the screen's work area when the default layout is
    // too big for it (small or high-scaling displays). Normal start-up only:
    // the self test keeps the fixed default size its UI checks are written for.
    void fitToScreen();

    void closeButtonPressed() override;

private:
    MainComponent* mainComponent = nullptr; // owned by the window as its content

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

} // namespace meridian
