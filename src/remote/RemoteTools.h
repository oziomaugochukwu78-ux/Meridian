#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <string>

namespace juce { class AudioPluginInstance; }

namespace meridian
{
class AppController;

/** The complete AI control surface, as named tools with JSON arguments.

    One implementation serves three front ends:
      - MCP (Model Context Protocol) over HTTP at http://127.0.0.1:<port>/mcp,
        so AI applications discover and call the tools natively;
      - JSON messages on the WebSocket port: {"tool": "...", "arguments": {...}};
      - the original text commands (PLAY, MUTE_TRACK_2, ...), which map onto
        the same application calls.

    Tracks are identified by number (1 = first track, as shown in the UI) or
    by name. Every edit goes through the session's undo history, so the user
    can undo anything the AI did. Message thread only. */
class RemoteTools
{
public:
    explicit RemoteTools (AppController&);

    struct Result
    {
        bool ok = false;
        juce::var data;     ///< returned to the caller (object, string, ...)
        juce::String error; ///< set when ok is false; written for the AI to act on
    };

    /** MCP tool descriptors: [{name, title, description, inputSchema}, ...]. */
    juce::var describeTools() const;

    Result call (const juce::String& toolName, const juce::var& arguments);

    /** WebSocket JSON form: {"tool": name, "arguments": {...}} -> JSON reply. */
    std::string handleJsonMessage (const std::string& message);

    /** One MCP JSON-RPC request body -> response body. Empty response with
        status 202 for notifications. */
    std::string handleMcpRequest (const std::string& body, int& httpStatus);

private:
    Result getStatus();
    Result transport (const juce::var&);
    Result setTrack (const juce::var&);
    Result duplicateTrack (const juce::var&);
    Result moveTrack (const juce::var&);
    Result playlist (const juce::var&);
    Result setLoop (const juce::var&);
    Result setClick (const juce::var&);
    Result setPunch (const juce::var&);
    Result addMarker (const juce::var&);
    Result listMarkers();
    Result deleteMarker (const juce::var&);
    juce::var describeTransportSettings() const;
    juce::var describeMarker (const juce::ValueTree&) const;
    Result createTrack (const juce::var&);
    Result deleteTrack (const juce::var&);
    Result setOutput (const juce::var&);
    Result setSend (const juce::var&);
    Result removeSend (const juce::var&);
    Result listPlugins (const juce::var&);
    Result insertPlugin (const juce::var&);
    Result removePlugin (const juce::var&);
    Result setPluginBypass (const juce::var&);
    Result listPluginParameters (const juce::var&);
    Result setPluginParameter (const juce::var&);
    juce::AudioPluginInstance* findPlugin (const juce::var& args, juce::String& error) const;
    Result listClips (const juce::var&);
    Result splitClip (const juce::var&);
    Result deleteClip (const juce::var&);
    Result moveClip (const juce::var&);
    Result setClip (const juce::var&);
    Result importAudio (const juce::var&);
    Result setVolumeAutomation (const juce::var&);
    Result setAutomation (const juce::var&);
    Result setTempo (const juce::var&);
    Result setMeter (const juce::var&);
    juce::var describeTempoMap() const;
    Result addMidiClip (const juce::var&);
    Result addNotes (const juce::var&);
    Result listNotes (const juce::var&);
    Result setSynth (const juce::var&);
    Result setMaster (const juce::var&);
    Result bounce (const juce::var&);
    Result undoRedo (const juce::var&, bool undo);
    Result saveSession();
    Result showWindow (const juce::var&);

    juce::ValueTree findTrack (const juce::var& reference, juce::String& error) const;
    juce::ValueTree findClip (const juce::ValueTree& track, const juce::var& reference, juce::String& error) const;
    juce::ValueTree findInstrumentTrack (const juce::var& reference, juce::String& error) const;
    juce::var describeTrack (const juce::ValueTree& track, int number) const;
    juce::UndoManager* beginEdit (const juce::String& name);
    juce::int64 secondsToSamples (const juce::var& seconds) const;
    double samplesToSeconds (juce::int64 samples) const;

    AppController& app;
};

} // namespace meridian
