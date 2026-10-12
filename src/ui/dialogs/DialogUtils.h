#pragma once

#include "ui/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian::dialogs
{

/** Shows `content` in a modal dialog window that owns and deletes it. */
inline juce::DialogWindow* launch (juce::Component* content, const juce::String& title, bool resizable = false)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (content);
    options.dialogTitle = title;
    options.dialogBackgroundColour = theme::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = resizable;
    return options.launchAsync();
}

/** Closes the dialog window that contains `content` (deleting it). */
inline void close (juce::Component& content)
{
    if (auto* window = content.findParentComponentOfClass<juce::DialogWindow>())
        window->exitModalState (0);
}

inline void styleSectionLabel (juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setFont (theme::font (11.0f, true));
    label.setColour (juce::Label::textColourId, theme::textDim);
}

inline void styleFieldLabel (juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setFont (theme::font (12.0f));
    label.setJustificationType (juce::Justification::centredRight);
}

} // namespace meridian::dialogs
