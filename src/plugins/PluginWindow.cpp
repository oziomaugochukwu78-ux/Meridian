#include "PluginWindow.h"

#include "ui/Theme.h"

namespace meridian
{

juce::OwnedArray<PluginWindow>& PluginWindow::openWindows()
{
    static juce::OwnedArray<PluginWindow> windows;
    return windows;
}

void PluginWindow::open (std::shared_ptr<InsertSlot> slot, const juce::String& title)
{
    if (slot == nullptr || slot->plugin == nullptr)
        return;

    for (auto* w : openWindows())
    {
        if (w->slot == slot)
        {
            w->toFront (true);
            return;
        }
    }

    openWindows().add (new PluginWindow (std::move (slot), title));
}

void PluginWindow::closeAll()
{
    openWindows().clear();
}

PluginWindow::PluginWindow (std::shared_ptr<InsertSlot> s, const juce::String& title)
    : juce::DocumentWindow (title, theme::panel, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
      slot (std::move (s))
{
    setUsingNativeTitleBar (true);

    juce::AudioProcessorEditor* editor = slot->plugin->hasEditor() ? slot->plugin->createEditorIfNeeded() : nullptr;
    if (editor == nullptr)
        editor = new juce::GenericAudioProcessorEditor (*slot->plugin);

    setContentOwned (editor, true);
    setResizable (editor->isResizable(), false);
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
    startTimer (500);
}

PluginWindow::~PluginWindow()
{
    // The editor must be destroyed before the plug-in it belongs to.
    clearContentComponent();
}

void PluginWindow::timerCallback()
{
    // Only this window still references the insert: it was removed from the track.
    if (slot.use_count() <= 1)
        closeButtonPressed();
}

void PluginWindow::closeButtonPressed()
{
    stopTimer();
    // Deleting from inside our own callback is not safe; defer it.
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<PluginWindow> (this)]
    {
        if (safe != nullptr)
            openWindows().removeObject (safe.getComponent());
    });
}

} // namespace meridian
