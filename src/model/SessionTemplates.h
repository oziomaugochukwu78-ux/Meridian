#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace meridian
{
class Session;

/** A starting track layout offered by the Dashboard's "Create from template". */
struct SessionTemplate
{
    struct TrackSpec
    {
        juce::String name;
        int channels = 1;
    };

    juce::String group;   ///< Template group shown in the group dropdown, e.g. "Music"
    juce::String name;
    juce::String description;
    std::vector<TrackSpec> tracks;
};

const std::vector<SessionTemplate>& builtInTemplates();

juce::StringArray templateGroups();

/** Adds the template's tracks to a freshly created session (not undoable). */
void applyTemplate (Session&, const SessionTemplate&);

} // namespace meridian
