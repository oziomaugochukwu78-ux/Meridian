#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace meridian
{
class Session;

/** A starting session offered by the Dashboard's "Create from template": its
    tracks (kind, channels, level), aux buses with their sends and outputs,
    the built-in effects on each track, a tempo and a console preset. */
struct SessionTemplate
{
    struct Send
    {
        juce::String bus;      ///< the name of an aux track in the template
        double levelDb = -12.0;
    };

    struct TrackSpec
    {
        juce::String name;
        int channels = 1;
        juce::String kind = "audio";  ///< "audio", "instrument", "drums" or "aux"
        double volumeDb = 0.0;
        juce::StringArray inserts;    ///< built-in effects by name, from insert A on
        std::vector<Send> sends;      ///< from send A on
        juce::String output;          ///< an aux track's name; empty = master
    };

    juce::String group;   ///< Template group shown in the group dropdown, e.g. "Music"
    juce::String name;
    juce::String description;
    std::vector<TrackSpec> tracks;
    double tempo = 0.0;          ///< BPM; 0 keeps the default
    juce::String consolePreset;  ///< a consolepresets name; empty keeps the default
};

const std::vector<SessionTemplate>& builtInTemplates();

juce::StringArray templateGroups();

/** Adds the template's tracks, buses, sends, outputs, tempo and console preset to
    a freshly created session (not undoable). Inserts need running plug-ins, so
    the caller adds them once the session is installed (AppController). */
void applyTemplate (Session&, const SessionTemplate&);

} // namespace meridian
