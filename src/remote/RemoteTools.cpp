#include "RemoteTools.h"

#include "app/AppController.h"
#include "model/Automation.h"
#include "model/Routing.h"

#include <dsp/SignalAnalysis.h>

namespace meridian
{
namespace
{
constexpr double faderFloorDb = -144.0;
const juce::StringArray supportedMcpVersions { "2025-06-18", "2025-03-26", "2024-11-05" };

//==============================================================================
// Small helpers for building JSON values

juce::var makeObject (std::initializer_list<std::pair<const char*, juce::var>> properties)
{
    auto* object = new juce::DynamicObject();
    for (const auto& [name, value] : properties)
        object->setProperty (name, value);
    return juce::var (object);
}

void set (juce::var& object, const char* name, const juce::var& value)
{
    if (auto* o = object.getDynamicObject())
        o->setProperty (name, value);
}

juce::var dbOrNull (double db)
{
    return db <= faderFloorDb ? juce::var() : juce::var (std::round (db * 100.0) / 100.0);
}

/** Accepts a number, or "-inf"/"off" for a fully lowered fader. */
double readDb (const juce::var& v)
{
    if (v.isString() && (v.toString().trim().startsWithIgnoreCase ("-inf") || v.toString().trim().equalsIgnoreCase ("off")))
        return faderFloorDb;
    if (v.isVoid())
        return faderFloorDb;
    return juce::jlimit (faderFloorDb, 12.0, (double) v);
}

/** Send/insert slots: A-E / A-J letters or 1-based numbers. Returns -1 if invalid. */
int readSlot (const juce::var& v, int count)
{
    int slot = -1;
    if (v.isString())
    {
        const auto s = v.toString().trim().toUpperCase();
        const int letter = (int) s[0];
        if (s.length() == 1 && letter >= 'A' && letter < 'A' + count)
            slot = letter - 'A';
        else if (s.containsOnly ("0123456789"))
            slot = s.getIntValue() - 1;
    }
    else if (v.isInt() || v.isInt64() || v.isDouble())
    {
        slot = (int) v - 1;
    }
    return juce::isPositiveAndBelow (slot, count) ? slot : -1;
}

juce::String slotLetter (int slot) { return juce::String::charToString ((juce::juce_wchar) ('A' + slot)); }

//==============================================================================
// Tool schemas

struct Param
{
    const char* name;
    const char* type;          ///< "string", "number", "integer", "boolean", or "track" (number or name)
    const char* description;
    bool required = false;
    std::vector<const char*> choices {};
};

juce::var schema (std::initializer_list<Param> params)
{
    auto properties = makeObject ({});
    juce::Array<juce::var> required;

    for (const auto& p : params)
    {
        auto property = makeObject ({ { "description", p.description } });
        if (juce::String (p.type) == "track")
        {
            set (property, "type", juce::Array<juce::var> { "integer", "string" });
        }
        else if (juce::String (p.type) == "array")
        {
            // The only array parameter: automation breakpoints.
            set (property, "type", "array");
            set (property, "items", makeObject ({
                { "type", "object" },
                { "properties", makeObject ({ { "seconds", makeObject ({ { "type", "number" } }) },
                                              { "db", makeObject ({ { "type", "number" } }) } }) },
                { "required", juce::Array<juce::var> { "seconds", "db" } } }));
        }
        else
        {
            set (property, "type", p.type);
        }

        if (! p.choices.empty())
        {
            juce::Array<juce::var> choices;
            for (const auto* c : p.choices)
                choices.add (c);
            set (property, "enum", choices);
        }
        set (properties, p.name, property);
        if (p.required)
            required.add (p.name);
    }

    auto result = makeObject ({ { "type", "object" }, { "properties", properties } });
    if (! required.isEmpty())
        set (result, "required", required);
    return result;
}

struct ToolInfo
{
    const char* name;
    const char* title;
    const char* description;
    juce::var inputSchema;
};

const std::vector<ToolInfo>& toolTable()
{
    static const char* trackHelp = "Track number (1 = first track, as shown in the app) or exact track name";

    static const std::vector<ToolInfo> tools {
        { "get_status", "Get session status",
          "The open session: transport, master bus, and every track with its level, pan, mute/solo, routing, sends, "
          "plug-ins and clip count. Call this first to learn track numbers and names.",
          schema ({}) },
        { "transport", "Transport",
          "Play, stop, record (on armed tracks), return to zero, or move the playback cursor.",
          schema ({ { "action", "string", "What to do", true, { "play", "stop", "record", "locate", "return_to_zero" } },
                    { "seconds", "number", "Cursor position in seconds (for locate)" } }) },
        { "set_track", "Change a track",
          "Set any of a track's mixer settings. Level changes glide smoothly (no clicks). Only the given fields change.",
          schema ({ { "track", "track", trackHelp, true },
                    { "name", "string", "New name" },
                    { "volume_db", "number", "Fader level in dB, -144 (off) to +12; 0 is unity" },
                    { "pan", "number", "-1 (left) to +1 (right); 0 is centre" },
                    { "mute", "boolean", "Mute" },
                    { "solo", "boolean", "Solo" },
                    { "record_arm", "boolean", "Record-enable (audio tracks)" },
                    { "input", "integer", "Audio input channel, 1-based (0 = no input)" } }) },
        { "create_track", "Create tracks",
          "Create audio tracks (play and record audio) or aux tracks (buses that receive sends, e.g. for a shared reverb).",
          schema ({ { "kind", "string", "Track type", false, { "audio", "aux" } },
                    { "channels", "integer", "1 = mono, 2 = stereo (aux tracks are always stereo)" },
                    { "name", "string", "Name (numbered automatically if creating several)" },
                    { "count", "integer", "How many to create (default 1)" } }) },
        { "delete_track", "Delete a track", "Delete a track (undoable).", schema ({ { "track", "track", trackHelp, true } }) },
        { "set_output", "Route a track's output",
          "Send a track's whole output to the master bus or into a bus (aux track). Routes that would loop are refused.",
          schema ({ { "track", "track", trackHelp, true },
                    { "destination", "track", "\"master\", or the aux track's number or name", true } }) },
        { "set_send", "Create or change a send",
          "Send part of a track to a bus (aux track), e.g. for reverb. Slots A-E. Post-fader by default.",
          schema ({ { "track", "track", trackHelp, true },
                    { "slot", "string", "Send slot", true, { "A", "B", "C", "D", "E" } },
                    { "bus", "track", "Destination aux track number or name (required when creating)" },
                    { "level_db", "number", "Send level in dB (-144 to +12)" },
                    { "pan", "number", "Send pan, -1 to +1" },
                    { "pre_fader", "boolean", "true = independent of the track fader" },
                    { "mute", "boolean", "Mute the send" } }) },
        { "remove_send", "Remove a send", "Remove a send.",
          schema ({ { "track", "track", trackHelp, true }, { "slot", "string", "Send slot", true, { "A", "B", "C", "D", "E" } } }) },
        { "list_plugins", "List plug-ins", "Effect plug-ins installed and scanned on this computer.",
          schema ({ { "filter", "string", "Only names containing this text" } }) },
        { "insert_plugin", "Insert a plug-in",
          "Put an effect plug-in on a track's insert slot (A-J, processed in order). Replaces what is there.",
          schema ({ { "track", "track", trackHelp, true },
                    { "slot", "string", "Insert slot A-J (or 1-10)", true },
                    { "plugin", "string", "Plug-in name, as returned by list_plugins", true } }) },
        { "remove_plugin", "Remove a plug-in", "Remove the plug-in from an insert slot.",
          schema ({ { "track", "track", trackHelp, true }, { "slot", "string", "Insert slot A-J (or 1-10)", true } }) },
        { "set_plugin_bypass", "Bypass a plug-in", "Bypass (true) or enable (false) an insert.",
          schema ({ { "track", "track", trackHelp, true },
                    { "slot", "string", "Insert slot A-J (or 1-10)", true },
                    { "bypass", "boolean", "true = bypassed", true } }) },
        { "list_clips", "List clips", "Audio clips on one track (or all tracks), with positions in seconds.",
          schema ({ { "track", "track", "Optional: only this track" } }) },
        { "split_clip", "Split clips", "Cut every clip on a track at a time position (like Separate Clip).",
          schema ({ { "track", "track", trackHelp, true }, { "seconds", "number", "Where to cut", true } }) },
        { "delete_clip", "Delete a clip", "Delete a clip (undoable).",
          schema ({ { "track", "track", trackHelp, true }, { "clip", "track", "Clip number (from list_clips) or name", true } }) },
        { "move_clip", "Move a clip", "Move a clip in time and/or to another track.",
          schema ({ { "track", "track", trackHelp, true },
                    { "clip", "track", "Clip number (from list_clips) or name", true },
                    { "start_seconds", "number", "New start time" },
                    { "to_track", "track", "Destination track number or name" } }) },
        { "set_clip", "Change a clip", "Clip gain, fades and name.",
          schema ({ { "track", "track", trackHelp, true },
                    { "clip", "track", "Clip number (from list_clips) or name", true },
                    { "gain_db", "number", "Clip gain in dB" },
                    { "fade_in_seconds", "number", "Fade-in length" },
                    { "fade_out_seconds", "number", "Fade-out length" },
                    { "name", "string", "New name" } }) },
        { "import_audio", "Import audio",
          "Copy an audio file into the session (converted to the session's format) and place it on a track. Runs in the "
          "background; call list_clips to see the result.",
          schema ({ { "file", "string", "Absolute path of a WAV/AIFF/FLAC/MP3/... file", true },
                    { "track", "track", "Track to place it on (default: a new track)" },
                    { "position_seconds", "number", "Where to place it (default 0)" } }) },
        { "set_volume_automation", "Volume automation",
          "Draw a track's volume over time: breakpoints of {seconds, db}, joined by straight lines (in dB) and held "
          "before the first and after the last. Replaces the existing automation unless replace=false. "
          "An empty points list removes the automation (the fader is used again).",
          schema ({ { "track", "track", trackHelp, true },
                    { "points", "array", "Breakpoints, e.g. [{\"seconds\": 0, \"db\": -60}, {\"seconds\": 4, \"db\": 0}]", true },
                    { "replace", "boolean", "Replace existing points (default true)" } }) },
        { "set_master", "Master bus",
          "Master fader and the analog console emulation (saturation) on the master bus.",
          schema ({ { "volume_db", "number", "Master fader in dB" },
                    { "console", "boolean", "Console emulation on/off" },
                    { "drive_db", "number", "Console drive, 0-24 dB (how early saturation starts)" },
                    { "character", "number", "Console asymmetry 0-1 (more = more 2nd harmonic warmth)" },
                    { "curve", "string", "Saturation curve", false, { "tanh", "cubic" } } }) },
        { "bounce", "Bounce (export) the mix",
          "Mix the session down to a WAV/AIFF file in the session's Bounced Files folder. Runs in the background; "
          "get_status reports progress and then the file and its peak levels.",
          schema ({ { "start_seconds", "number", "Optional start (default 0)" },
                    { "end_seconds", "number", "Optional end (default: end of the last clip + 1 s)" } }) },
        { "undo", "Undo", "Undo the last edit (including edits made by AI tools).", schema ({}) },
        { "redo", "Redo", "Redo the last undone edit.", schema ({}) },
        { "save_session", "Save session", "Save the session to disk.", schema ({}) },
        { "show_window", "Show window", "Show the Edit window (timeline) or the Mix window (mixer).",
          schema ({ { "window", "string", "Which window", true, { "edit", "mix" } } }) },
    };
    return tools;
}

juce::String toJson (const juce::var& v) { return juce::JSON::toString (v, true); }
} // namespace

//==============================================================================
RemoteTools::RemoteTools (AppController& a) : app (a) {}

juce::var RemoteTools::describeTools() const
{
    juce::Array<juce::var> list;
    for (const auto& t : toolTable())
        list.add (makeObject ({ { "name", t.name }, { "title", t.title }, { "description", t.description },
                                { "inputSchema", t.inputSchema } }));
    return list;
}

RemoteTools::Result RemoteTools::call (const juce::String& name, const juce::var& args)
{
    if (name != "get_status" && name != "list_plugins" && app.getSession() == nullptr)
        return { false, {}, "No session is open. Ask the user to create or open one in the Dashboard." };

    if (name == "get_status")         return getStatus();
    if (name == "transport")          return transport (args);
    if (name == "set_track")          return setTrack (args);
    if (name == "create_track")       return createTrack (args);
    if (name == "delete_track")       return deleteTrack (args);
    if (name == "set_output")         return setOutput (args);
    if (name == "set_send")           return setSend (args);
    if (name == "remove_send")        return removeSend (args);
    if (name == "list_plugins")       return listPlugins (args);
    if (name == "insert_plugin")      return insertPlugin (args);
    if (name == "remove_plugin")      return removePlugin (args);
    if (name == "set_plugin_bypass")  return setPluginBypass (args);
    if (name == "list_clips")         return listClips (args);
    if (name == "split_clip")         return splitClip (args);
    if (name == "delete_clip")        return deleteClip (args);
    if (name == "move_clip")          return moveClip (args);
    if (name == "set_clip")           return setClip (args);
    if (name == "import_audio")       return importAudio (args);
    if (name == "set_volume_automation") return setVolumeAutomation (args);
    if (name == "set_master")         return setMaster (args);
    if (name == "bounce")             return bounce (args);
    if (name == "undo")               return undoRedo (args, true);
    if (name == "redo")               return undoRedo (args, false);
    if (name == "save_session")       return saveSession();
    if (name == "show_window")        return showWindow (args);
    return { false, {}, "Unknown tool \"" + name + "\". Call tools/list (MCP) for the available tools." };
}

//==============================================================================
// Helpers

juce::ValueTree RemoteTools::findTrack (const juce::var& reference, juce::String& error) const
{
    auto* session = app.getSession();
    if (reference.isVoid())
    {
        error = "Missing \"track\".";
        return {};
    }

    const auto text = reference.toString().trim();
    if (reference.isInt() || reference.isInt64() || reference.isDouble() || (text.isNotEmpty() && text.containsOnly ("0123456789")))
    {
        const int number = reference.isString() ? text.getIntValue() : (int) reference;
        auto t = session->getTrack (number - 1);
        if (! t.isValid())
            error = "There is no track " + juce::String (number) + "; the session has " + juce::String (session->getNumTracks()) + ".";
        return t;
    }

    for (const auto& t : session->getTracksNode())
        if (track::name (t).equalsIgnoreCase (text))
            return t;

    error = "No track is named \"" + text + "\". Call get_status for the track list.";
    return {};
}

juce::ValueTree RemoteTools::findClip (const juce::ValueTree& t, const juce::var& reference, juce::String& error) const
{
    const auto clips = track::clips (t);
    const auto text = reference.toString().trim();
    if (reference.isInt() || reference.isInt64() || reference.isDouble() || (text.isNotEmpty() && text.containsOnly ("0123456789")))
    {
        const int number = reference.isString() ? text.getIntValue() : (int) reference;
        auto c = clips.getChild (number - 1);
        if (! c.isValid())
            error = "Track \"" + track::name (t) + "\" has no clip " + juce::String (number) + " (it has "
                    + juce::String (clips.getNumChildren()) + ").";
        return c;
    }
    for (const auto& c : clips)
        if (clip::name (c).equalsIgnoreCase (text))
            return c;
    error = "No clip named \"" + text + "\" on track \"" + track::name (t) + "\". Call list_clips.";
    return {};
}

juce::UndoManager* RemoteTools::beginEdit (const juce::String& name)
{
    auto& um = app.getSession()->getUndoManager();
    um.beginNewTransaction ("AI: " + name);
    return &um;
}

juce::int64 RemoteTools::secondsToSamples (const juce::var& seconds) const
{
    return (juce::int64) std::llround (juce::jmax (0.0, (double) seconds) * app.getSession()->getSampleRate());
}

double RemoteTools::samplesToSeconds (juce::int64 samples) const
{
    return std::round ((double) samples / app.getSession()->getSampleRate() * 1000.0) / 1000.0;
}

juce::var RemoteTools::describeTrack (const juce::ValueTree& t, int number) const
{
    auto* session = app.getSession();
    const int out = track::outputBus (t);
    const auto outTrack = out != 0 ? session->getTrackById (out) : juce::ValueTree();

    juce::Array<juce::var> sends;
    for (const auto& s : track::sends (t))
    {
        const auto target = session->getTrackById (send::target (s));
        sends.add (makeObject ({ { "slot", slotLetter (send::slot (s)) },
                                 { "bus", target.isValid() ? track::name (target) : juce::String ("(missing)") },
                                 { "level_db", dbOrNull (send::levelDb (s)) },
                                 { "pan", send::pan (s) },
                                 { "pre_fader", send::preFader (s) },
                                 { "mute", send::mute (s) } }));
    }

    juce::Array<juce::var> inserts;
    for (const auto& node : track::inserts (t))
    {
        const int slot = node[ids::slot];
        const auto instance = app.getBridge().getInsert (track::id (t), slot);
        inserts.add (makeObject ({ { "slot", slotLetter (slot) },
                                   { "plugin", instance != nullptr ? instance->description.name : juce::String ("?") },
                                   { "bypassed", (bool) node.getProperty (ids::bypass, false) },
                                   { "loaded", instance != nullptr && instance->plugin != nullptr } }));
    }

    const int input = track::inputChannel (t);
    return makeObject ({ { "number", number },
                         { "name", track::name (t) },
                         { "kind", track::isAux (t) ? "aux" : "audio" },
                         { "channels", track::channels (t) },
                         { "volume_db", dbOrNull (track::volumeDb (t)) },
                         { "pan", track::pan (t) },
                         { "mute", track::mute (t) },
                         { "solo", track::solo (t) },
                         { "record_arm", track::recordArm (t) },
                         { "input", track::isAux (t) ? juce::var() : juce::var (input + 1) },
                         { "output", outTrack.isValid() ? track::name (outTrack) : juce::String ("master") },
                         { "sends", sends },
                         { "inserts", inserts },
                         { "clips", track::clips (t).getNumChildren() } });
}

//==============================================================================
// Tools

RemoteTools::Result RemoteTools::getStatus()
{
    auto& engine = app.getEngine();
    const auto& transport = engine.getTransport();
    const auto state = transport.getState();

    auto status = makeObject ({
        { "transport", state == Transport::State::recording ? "recording" : state == Transport::State::playing ? "playing" : "stopped" },
        { "position_seconds", std::round ((double) transport.getPosition() / juce::jmax (1.0, engine.getSampleRate()) * 1000.0) / 1000.0 },
        { "audio_device_running", engine.isDeviceRunning() },
        { "window", app.activeWindowQuery && app.activeWindowQuery() == WindowKind::mix ? "mix" : "edit" },
        { "plugin_latency_samples", engine.getPluginLatencySamples() },
    });

    auto bounceInfo = makeObject ({ { "running", app.isBouncing() } });
    if (app.isBouncing())
        set (bounceInfo, "progress", std::round (app.getBounceProgress() * 1000.0) / 1000.0);
    if (const auto& last = app.getLastBounce(); last.ok)
    {
        set (bounceInfo, "last_file", last.file.getFullPathName());
        set (bounceInfo, "last_peak_db_left", std::round (dsp::gainToDb (last.left.peak) * 100.0) / 100.0);
        set (bounceInfo, "last_peak_db_right", std::round (dsp::gainToDb (last.right.peak) * 100.0) / 100.0);
    }
    set (status, "bounce", bounceInfo);

    auto* session = app.getSession();
    if (session == nullptr)
    {
        set (status, "session", juce::var());
        return { true, status, {} };
    }

    const auto& s = session->getState();
    set (status, "session", makeObject ({ { "name", session->getName() },
                                          { "sample_rate", session->getSampleRate() },
                                          { "bit_depth", session->getBitDepth() },
                                          { "unsaved_changes", session->hasUnsavedChanges() },
                                          { "length_seconds", samplesToSeconds (app.getSessionEnd()) } }));
    set (status, "master", makeObject ({ { "volume_db", dbOrNull (s.getProperty (ids::masterVolumeDb, 0.0)) },
                                         { "console", (bool) s.getProperty (ids::consoleEnabled, true) },
                                         { "drive_db", (double) s.getProperty (ids::consoleDrive, 6.0) },
                                         { "character", (double) s.getProperty (ids::consoleCharacter, 0.35) },
                                         { "curve", (int) s.getProperty (ids::consoleCurve, 0) == 1 ? "cubic" : "tanh" } }));

    juce::Array<juce::var> tracks;
    for (int i = 0; i < session->getNumTracks(); ++i)
        tracks.add (describeTrack (session->getTrack (i), i + 1));
    set (status, "tracks", tracks);
    return { true, status, {} };
}

RemoteTools::Result RemoteTools::transport (const juce::var& args)
{
    const auto action = args["action"].toString();
    juce::Result r = juce::Result::ok();

    if (action == "play")                r = app.transportPlay();
    else if (action == "stop")           app.transportStop();
    else if (action == "record")         r = app.transportRecord();
    else if (action == "return_to_zero") app.returnToZero();
    else if (action == "locate")
    {
        if (! args.hasProperty ("seconds"))
            return { false, {}, "locate needs \"seconds\"." };
        app.locate (secondsToSamples (args["seconds"]));
    }
    else
        return { false, {}, "Unknown action \"" + action + "\" (use play, stop, record, locate or return_to_zero)." };

    if (r.failed())
        return { false, {}, r.getErrorMessage() };
    return { true, makeObject ({ { "done", action } }), {} };
}

RemoteTools::Result RemoteTools::setTrack (const juce::var& args)
{
    juce::String error;
    auto t = findTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };

    auto* um = beginEdit ("Change Track");
    if (args.hasProperty ("name") && args["name"].toString().trim().isNotEmpty())
        t.setProperty (ids::name, args["name"].toString().trim(), um);
    if (args.hasProperty ("volume_db"))  t.setProperty (ids::volumeDb, readDb (args["volume_db"]), um);
    if (args.hasProperty ("pan"))        t.setProperty (ids::pan, juce::jlimit (-1.0, 1.0, (double) args["pan"]), um);
    if (args.hasProperty ("mute"))       t.setProperty (ids::mute, (bool) args["mute"], um);
    if (args.hasProperty ("solo"))       t.setProperty (ids::solo, (bool) args["solo"], um);
    if (args.hasProperty ("record_arm"))
    {
        if (track::isAux (t) && (bool) args["record_arm"])
            return { false, {}, "Aux tracks cannot record." };
        t.setProperty (ids::recordArm, (bool) args["record_arm"], um);
    }
    if (args.hasProperty ("input"))
    {
        if (track::isAux (t))
            return { false, {}, "Aux tracks have no audio input; route or send other tracks to them instead." };
        t.setProperty (ids::inputChannel, juce::jmax (0, (int) args["input"]) - 1, um);
    }

    return { true, describeTrack (t, app.getSession()->indexOfTrack (t) + 1), {} };
}

RemoteTools::Result RemoteTools::createTrack (const juce::var& args)
{
    auto* session = app.getSession();
    const bool aux = args["kind"].toString() == "aux";
    const int channels = aux ? 2 : juce::jlimit (1, 2, args.hasProperty ("channels") ? (int) args["channels"] : 1);
    const int count = juce::jlimit (1, 64, args.hasProperty ("count") ? (int) args["count"] : 1);
    const auto requested = args["name"].toString().trim();

    juce::Array<juce::var> created;
    auto* um = beginEdit (count == 1 ? "New Track" : "New Tracks");
    for (int i = 0; i < count; ++i)
    {
        juce::String name = requested.isNotEmpty() ? (count == 1 ? requested : requested + " " + juce::String (i + 1))
                                                   : juce::String (aux ? "Aux " : "Audio ") + juce::String (session->getNumTracks() + 1);
        auto t = session->addTrack (name, channels, um, aux ? Session::TrackKind::aux : Session::TrackKind::audio);
        created.add (describeTrack (t, session->indexOfTrack (t) + 1));
    }
    return { true, makeObject ({ { "created", created } }), {} };
}

RemoteTools::Result RemoteTools::deleteTrack (const juce::var& args)
{
    juce::String error;
    auto t = findTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };
    const auto name = track::name (t);
    app.deleteTrack (t);
    return { true, makeObject ({ { "deleted", name } }), {} };
}

RemoteTools::Result RemoteTools::setOutput (const juce::var& args)
{
    juce::String error;
    auto t = findTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };

    int targetId = 0;
    const auto destination = args["destination"];
    if (! destination.toString().trim().equalsIgnoreCase ("master"))
    {
        const auto target = findTrack (destination, error);
        if (! target.isValid())
            return { false, {}, error };
        if (! track::isAux (target))
            return { false, {}, "\"" + track::name (target) + "\" is an audio track; outputs can only go to aux tracks (buses) or the master." };
        targetId = track::id (target);
    }

    if (! routing::setOutput (*app.getSession(), t, targetId, beginEdit ("Track Output")))
        return { false, {}, "That route would feed the bus back into itself (a loop), so it was refused." };
    return { true, describeTrack (t, app.getSession()->indexOfTrack (t) + 1), {} };
}

RemoteTools::Result RemoteTools::setSend (const juce::var& args)
{
    auto* session = app.getSession();
    juce::String error;
    auto t = findTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };

    const int slot = readSlot (args["slot"], Session::maxSends);
    if (slot < 0)
        return { false, {}, "Send slot must be A-E." };

    auto* um = beginEdit ("Send");
    if (args.hasProperty ("bus"))
    {
        const auto bus = findTrack (args["bus"], error);
        if (! bus.isValid())
            return { false, {}, error };
        if (! track::isAux (bus))
            return { false, {}, "\"" + track::name (bus) + "\" is not an aux track. Create one with create_track kind=aux." };
        if (! routing::setSend (*session, t, slot, track::id (bus), um))
            return { false, {}, "That send would create a loop (or targets the track itself), so it was refused." };
    }

    auto node = routing::findSend (t, slot);
    if (! node.isValid())
        return { false, {}, "Send " + slotLetter (slot) + " is empty; give \"bus\" to create it." };

    if (args.hasProperty ("level_db"))  node.setProperty (ids::levelDb, readDb (args["level_db"]), um);
    if (args.hasProperty ("pan"))       node.setProperty (ids::pan, juce::jlimit (-1.0, 1.0, (double) args["pan"]), um);
    if (args.hasProperty ("pre_fader")) node.setProperty (ids::preFader, (bool) args["pre_fader"], um);
    if (args.hasProperty ("mute"))      node.setProperty (ids::mute, (bool) args["mute"], um);

    return { true, describeTrack (t, session->indexOfTrack (t) + 1), {} };
}

RemoteTools::Result RemoteTools::removeSend (const juce::var& args)
{
    juce::String error;
    auto t = findTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };
    const int slot = readSlot (args["slot"], Session::maxSends);
    if (slot < 0 || ! routing::findSend (t, slot).isValid())
        return { false, {}, "There is no send in that slot." };
    routing::removeSend (t, slot, beginEdit ("Remove Send"));
    return { true, describeTrack (t, app.getSession()->indexOfTrack (t) + 1), {} };
}

RemoteTools::Result RemoteTools::listPlugins (const juce::var& args)
{
    const auto filter = args["filter"].toString().trim();
    juce::Array<juce::var> list;
    for (const auto& d : app.getPluginHost().getEffectPlugins())
        if (filter.isEmpty() || d.name.containsIgnoreCase (filter) || d.manufacturerName.containsIgnoreCase (filter))
            list.add (makeObject ({ { "name", d.name }, { "manufacturer", d.manufacturerName },
                                    { "format", d.pluginFormatName }, { "category", d.category } }));
    auto result = makeObject ({ { "plugins", list } });
    if (list.isEmpty())
        set (result, "note", "No effect plug-ins are scanned. The user can scan in Setup > Plug-in Manager.");
    return { true, result, {} };
}

RemoteTools::Result RemoteTools::insertPlugin (const juce::var& args)
{
    juce::String error;
    auto t = findTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };
    const int slot = readSlot (args["slot"], Session::maxInserts);
    if (slot < 0)
        return { false, {}, "Insert slot must be A-J or 1-10." };

    const auto wanted = args["plugin"].toString().trim();
    const juce::PluginDescription* match = nullptr;
    const auto plugins = app.getPluginHost().getEffectPlugins();
    for (const auto& d : plugins)
        if (d.name.equalsIgnoreCase (wanted))
            match = &d;
    for (const auto& d : plugins)
        if (match == nullptr && d.name.containsIgnoreCase (wanted))
            match = &d;
    if (match == nullptr)
        return { false, {}, "No scanned plug-in matches \"" + wanted + "\". Call list_plugins." };

    if (! app.getBridge().insertPlugin (t, slot, *match, error))
        return { false, {}, error };
    return { true, describeTrack (t, app.getSession()->indexOfTrack (t) + 1), {} };
}

RemoteTools::Result RemoteTools::removePlugin (const juce::var& args)
{
    juce::String error;
    auto t = findTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };
    const int slot = readSlot (args["slot"], Session::maxInserts);
    if (slot < 0 || ! app.getBridge().getInsertNode (t, slot).isValid())
        return { false, {}, "There is no plug-in in that slot." };
    app.getBridge().removePlugin (t, slot);
    return { true, describeTrack (t, app.getSession()->indexOfTrack (t) + 1), {} };
}

RemoteTools::Result RemoteTools::setPluginBypass (const juce::var& args)
{
    juce::String error;
    auto t = findTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };
    const int slot = readSlot (args["slot"], Session::maxInserts);
    if (slot < 0 || ! app.getBridge().getInsertNode (t, slot).isValid())
        return { false, {}, "There is no plug-in in that slot." };
    app.getBridge().setPluginBypassed (t, slot, (bool) args["bypass"]);
    return { true, describeTrack (t, app.getSession()->indexOfTrack (t) + 1), {} };
}

RemoteTools::Result RemoteTools::listClips (const juce::var& args)
{
    auto* session = app.getSession();
    juce::Array<juce::var> result;

    auto addTrack = [&] (const juce::ValueTree& t)
    {
        int number = 0;
        for (const auto& c : track::clips (t))
        {
            ++number;
            result.add (makeObject ({ { "track", track::name (t) },
                                      { "clip", number },
                                      { "name", clip::name (c) },
                                      { "start_seconds", samplesToSeconds (clip::start (c)) },
                                      { "end_seconds", samplesToSeconds (clip::end (c)) },
                                      { "gain_db", clip::gainDb (c) },
                                      { "fade_in_seconds", samplesToSeconds (clip::fadeIn (c)) },
                                      { "fade_out_seconds", samplesToSeconds (clip::fadeOut (c)) } }));
        }
    };

    if (args.hasProperty ("track"))
    {
        juce::String error;
        const auto t = findTrack (args["track"], error);
        if (! t.isValid())
            return { false, {}, error };
        addTrack (t);
    }
    else
    {
        for (const auto& t : session->getTracksNode())
            addTrack (t);
    }
    return { true, makeObject ({ { "clips", result } }), {} };
}

RemoteTools::Result RemoteTools::splitClip (const juce::var& args)
{
    auto* session = app.getSession();
    juce::String error;
    auto t = findTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };

    const auto at = secondsToSamples (args["seconds"]);
    auto* um = beginEdit ("Split Clip");
    juce::Array<juce::ValueTree> candidates;
    for (const auto& c : track::clips (t))
        candidates.add (c);

    int splits = 0;
    for (auto& c : candidates)
        splits += session->splitClip (c, at, um).isValid() ? 1 : 0;

    if (splits == 0)
        return { false, {}, "No clip on \"" + track::name (t) + "\" spans " + juce::String (samplesToSeconds (at)) + " s." };
    return { true, makeObject ({ { "split", splits } }), {} };
}

RemoteTools::Result RemoteTools::deleteClip (const juce::var& args)
{
    juce::String error;
    auto t = findTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };
    auto c = findClip (t, args["clip"], error);
    if (! c.isValid())
        return { false, {}, error };
    const auto name = clip::name (c);
    track::clips (t).removeChild (c, beginEdit ("Delete Clip"));
    return { true, makeObject ({ { "deleted", name } }), {} };
}

RemoteTools::Result RemoteTools::moveClip (const juce::var& args)
{
    juce::String error;
    auto t = findTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };
    auto c = findClip (t, args["clip"], error);
    if (! c.isValid())
        return { false, {}, error };

    auto* um = beginEdit ("Move Clip");
    if (args.hasProperty ("start_seconds"))
        c.setProperty (ids::start, secondsToSamples (args["start_seconds"]), um);

    if (args.hasProperty ("to_track"))
    {
        auto destination = findTrack (args["to_track"], error);
        if (! destination.isValid())
            return { false, {}, error };
        if (track::isAux (destination))
            return { false, {}, "Aux tracks cannot hold clips." };
        if (destination != t)
        {
            track::clips (t).removeChild (c, um);
            track::clips (destination).appendChild (c, um);
        }
    }
    return { true, makeObject ({ { "clip", clip::name (c) }, { "track", track::name (clip::owningTrack (c)) },
                                 { "start_seconds", samplesToSeconds (clip::start (c)) } }), {} };
}

RemoteTools::Result RemoteTools::setClip (const juce::var& args)
{
    juce::String error;
    auto t = findTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };
    auto c = findClip (t, args["clip"], error);
    if (! c.isValid())
        return { false, {}, error };

    auto* um = beginEdit ("Change Clip");
    const auto length = clip::length (c);
    if (args.hasProperty ("gain_db"))   c.setProperty (ids::gainDb, juce::jlimit (-60.0, 24.0, (double) args["gain_db"]), um);
    if (args.hasProperty ("fade_in_seconds"))
        c.setProperty (ids::fadeIn, juce::jlimit<juce::int64> (0, length - clip::fadeOut (c), secondsToSamples (args["fade_in_seconds"])), um);
    if (args.hasProperty ("fade_out_seconds"))
        c.setProperty (ids::fadeOut, juce::jlimit<juce::int64> (0, length - clip::fadeIn (c), secondsToSamples (args["fade_out_seconds"])), um);
    if (args.hasProperty ("name") && args["name"].toString().trim().isNotEmpty())
        c.setProperty (ids::name, args["name"].toString().trim(), um);

    return { true, makeObject ({ { "clip", clip::name (c) }, { "gain_db", clip::gainDb (c) },
                                 { "fade_in_seconds", samplesToSeconds (clip::fadeIn (c)) },
                                 { "fade_out_seconds", samplesToSeconds (clip::fadeOut (c)) } }), {} };
}

RemoteTools::Result RemoteTools::importAudio (const juce::var& args)
{
    const auto path = args["file"].toString().trim().unquoted();
    if (! juce::File::isAbsolutePath (path) || ! juce::File (path).existsAsFile())
        return { false, {}, "File not found: \"" + path + "\" (give an absolute path)." };

    juce::ValueTree target;
    if (args.hasProperty ("track"))
    {
        juce::String error;
        target = findTrack (args["track"], error);
        if (! target.isValid())
            return { false, {}, error };
    }

    app.importAudioFiles ({ juce::File (path) }, secondsToSamples (args["position_seconds"]), target);
    return { true, makeObject ({ { "importing", juce::File (path).getFileName() },
                                 { "note", "Copying and converting in the background; call list_clips in a moment." } }), {} };
}

RemoteTools::Result RemoteTools::setVolumeAutomation (const juce::var& args)
{
    juce::String error;
    auto t = findTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };

    const auto* points = args["points"].getArray();
    if (points == nullptr)
        return { false, {}, "\"points\" must be an array of {\"seconds\": number, \"db\": number}." };

    auto* um = beginEdit ("Volume Automation");
    const bool replace = ! args.hasProperty ("replace") || (bool) args["replace"];
    if (points->isEmpty())
    {
        automation::clear (t, um);
        return { true, makeObject ({ { "track", track::name (t) }, { "automation", "removed" } }), {} };
    }

    auto lane = automation::getOrCreateVolumeLane (t, um);
    if (replace)
        while (lane.getNumChildren() > 0)
            lane.removeChild (0, um);

    for (const auto& p : *points)
    {
        if (! p.hasProperty ("seconds") || ! p.hasProperty ("db"))
            return { false, {}, "Each point needs \"seconds\" and \"db\"." };
        automation::addPoint (lane, secondsToSamples (p["seconds"]), readDb (p["db"]), um);
    }

    if (! track::automationRead (t))
        t.setProperty (ids::automationMode, "read", um); // otherwise the drawing would be ignored

    juce::Array<juce::var> written;
    for (const auto& p : automation::points (lane))
        written.add (makeObject ({ { "seconds", samplesToSeconds (p.time) }, { "db", dbOrNull (p.db) } }));
    return { true, makeObject ({ { "track", track::name (t) }, { "points", written } }), {} };
}

RemoteTools::Result RemoteTools::setMaster (const juce::var& args)
{
    auto state = app.getSession()->getState();
    auto* um = beginEdit ("Master");
    if (args.hasProperty ("volume_db"))  state.setProperty (ids::masterVolumeDb, readDb (args["volume_db"]), um);
    if (args.hasProperty ("console"))    state.setProperty (ids::consoleEnabled, (bool) args["console"], um);
    if (args.hasProperty ("drive_db"))   state.setProperty (ids::consoleDrive, juce::jlimit (0.0, 24.0, (double) args["drive_db"]), um);
    if (args.hasProperty ("character"))  state.setProperty (ids::consoleCharacter, juce::jlimit (0.0, 1.0, (double) args["character"]), um);
    if (args.hasProperty ("curve"))      state.setProperty (ids::consoleCurve, args["curve"].toString() == "cubic" ? 1 : 0, um);
    return getStatus();
}

RemoteTools::Result RemoteTools::bounce (const juce::var& args)
{
    auto request = app.makeSessionBounceRequest();
    if (args.hasProperty ("start_seconds")) request.start = secondsToSamples (args["start_seconds"]);
    if (args.hasProperty ("end_seconds"))   request.end = secondsToSamples (args["end_seconds"]);
    if (request.end <= request.start)
        return { false, {}, "The session is empty, or end is not after start." };

    if (const auto started = app.startBounce (request); started.failed())
        return { false, {}, started.getErrorMessage() };
    return { true, makeObject ({ { "bouncing_to", request.destination.getFullPathName() },
                                 { "seconds", samplesToSeconds (request.end - request.start) },
                                 { "note", "Call get_status: bounce.progress, then bounce.last_file and peak levels." } }), {} };
}

RemoteTools::Result RemoteTools::undoRedo (const juce::var&, bool undo)
{
    auto& um = app.getSession()->getUndoManager();
    const auto description = undo ? um.getUndoDescription() : um.getRedoDescription();
    if (! (undo ? um.undo() : um.redo()))
        return { false, {}, undo ? "Nothing to undo." : "Nothing to redo." };
    return { true, makeObject ({ { undo ? "undone" : "redone", description } }), {} };
}

RemoteTools::Result RemoteTools::saveSession()
{
    if (const auto r = app.saveSession(); r.failed())
        return { false, {}, r.getErrorMessage() };
    return { true, makeObject ({ { "saved", app.getSession()->getSessionFile().getFullPathName() } }), {} };
}

RemoteTools::Result RemoteTools::showWindow (const juce::var& args)
{
    const auto wanted = args["window"].toString() == "mix" ? WindowKind::mix : WindowKind::edit;
    if (! app.toggleWindowHandler || ! app.activeWindowQuery)
        return { false, {}, "No window is open." };
    if (app.activeWindowQuery() != wanted)
        app.toggleWindowHandler();
    return { true, makeObject ({ { "window", wanted == WindowKind::mix ? "mix" : "edit" } }), {} };
}

//==============================================================================
// Front ends

std::string RemoteTools::handleJsonMessage (const std::string& message)
{
    const auto request = juce::JSON::parse (juce::String (message));
    const auto tool = request["tool"].toString();
    if (! request.isObject() || tool.isEmpty())
        return toJson (makeObject ({ { "ok", false }, { "error", "Expected {\"tool\": \"name\", \"arguments\": {...}}." } })).toStdString();

    const auto args = request["arguments"].isObject() ? request["arguments"] : makeObject ({});
    const auto result = call (tool, args);
    auto reply = makeObject ({ { "ok", result.ok }, { "tool", tool } });
    if (result.ok)
        set (reply, "result", result.data);
    else
        set (reply, "error", result.error);
    return toJson (reply).toStdString();
}

std::string RemoteTools::handleMcpRequest (const std::string& body, int& httpStatus)
{
    httpStatus = 200;
    const auto request = juce::JSON::parse (juce::String (body));

    auto error = [] (const juce::var& id, int code, const juce::String& message)
    {
        return toJson (makeObject ({ { "jsonrpc", "2.0" }, { "id", id },
                                     { "error", makeObject ({ { "code", code }, { "message", message } }) } })).toStdString();
    };

    if (! request.isObject())
        return error ({}, -32700, "Parse error: expected one JSON-RPC request object");

    const auto method = request["method"].toString();
    if (! request.hasProperty ("id"))
    {
        httpStatus = 202; // notification (e.g. notifications/initialized): accepted, no body
        return {};
    }

    const auto id = request["id"];
    const auto params = request["params"];
    juce::var result;

    if (method == "initialize")
    {
        const auto requested = params["protocolVersion"].toString();
        result = makeObject ({
            { "protocolVersion", supportedMcpVersions.contains (requested) ? requested : supportedMcpVersions[0] },
            { "capabilities", makeObject ({ { "tools", makeObject ({ { "listChanged", false } }) } }) },
            { "serverInfo", makeObject ({ { "name", "meridian" }, { "title", "Meridian DAW" },
                                          { "version", JUCE_APPLICATION_VERSION_STRING } }) },
            { "instructions", "You are controlling Meridian, a digital audio workstation the user has open. Call get_status "
                              "first. Tracks are referenced by number (1 = first) or name. Levels are in dB (0 = unity, "
                              "-144 = off). Every edit is undoable by the user; prefer small, explained changes." },
        });
    }
    else if (method == "ping")
    {
        result = makeObject ({});
    }
    else if (method == "tools/list")
    {
        result = makeObject ({ { "tools", describeTools() } });
    }
    else if (method == "tools/call")
    {
        const auto outcome = call (params["name"].toString(), params["arguments"].isObject() ? params["arguments"] : makeObject ({}));
        const auto text = outcome.ok ? toJson (outcome.data) : outcome.error;
        result = makeObject ({ { "content", juce::Array<juce::var> { makeObject ({ { "type", "text" }, { "text", text } }) } },
                               { "isError", ! outcome.ok } });
        if (outcome.ok && outcome.data.isObject())
            set (result, "structuredContent", outcome.data);
    }
    else
    {
        return error (id, -32601, "Method not found: " + method);
    }

    return toJson (makeObject ({ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } })).toStdString();
}

} // namespace meridian
