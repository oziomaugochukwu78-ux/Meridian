#pragma once

#include "app/AppController.h"
#include "model/Automation.h"

/** Edit-window helpers for automation lanes that need the running plug-ins:
    their names and current values. Shared by the track header and the lane. */
namespace meridian::automationview
{

/** The plug-in control a "plugin:<slot>:<index>" lane automates, or nullptr. */
inline juce::AudioProcessorParameter* pluginControl (AppController& app, const juce::ValueTree& track, const juce::String& parameter)
{
    const auto [slot, index] = automation::pluginTarget (parameter);
    if (slot < 0)
        return nullptr;
    const auto insert = app.getBridge().getInsert (track::id (track), slot);
    if (insert == nullptr || insert->plugin == nullptr)
        return nullptr;
    const auto& all = insert->plugin->getParameters();
    return juce::isPositiveAndBelow (index, all.size()) ? all[index] : nullptr;
}

/** Lane name: "volume", "pan", "mute", "send A", or "A: <control>" for a plug-in. */
inline juce::String laneName (AppController& app, const juce::ValueTree& track, const juce::String& parameter)
{
    if (const auto [slot, index] = automation::pluginTarget (parameter); slot >= 0)
    {
        const auto letter = juce::String::charToString ((juce::juce_wchar) ('A' + slot));
        if (auto* control = pluginControl (app, track, parameter))
            return letter + ": " + control->getName (32);
        return letter + ": #" + juce::String (index + 1);
    }
    return automation::shortName (parameter);
}

/** What plays while the lane is empty: the control's own setting (a plug-in
    control's current value, normalised 0-1). */
inline double staticValue (AppController& app, const juce::ValueTree& track, const juce::String& parameter)
{
    if (automation::pluginTarget (parameter).first >= 0)
    {
        auto* control = pluginControl (app, track, parameter);
        return control != nullptr ? (double) control->getValue() : 0.0;
    }
    return automation::staticValue (track, parameter);
}

} // namespace meridian::automationview
