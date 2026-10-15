#include "RemoteTools.h"

#include "app/AppController.h"
#include "model/Automation.h"
#include "model/MidiModel.h"
#include "model/Playlists.h"
#include "model/Routing.h"
#include "plugins/BuiltinEffects.h"

#include <dsp/SignalAnalysis.h>
#include <remote/JsonLimits.h>

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
        else if (juce::String (p.type) == "array" || juce::String (p.type) == "breakpoints")
        {
            // Automation breakpoints: {seconds, db} for volume, {seconds, value} for any lane.
            const char* valueName = juce::String (p.type) == "array" ? "db" : "value";
            set (property, "type", "array");
            set (property, "items", makeObject ({
                { "type", "object" },
                { "properties", makeObject ({ { "seconds", makeObject ({ { "type", "number" } }) },
                                              { valueName, makeObject ({ { "type", "number" } }) } }) },
                { "required", juce::Array<juce::var> { "seconds", valueName } } }));
        }
        else if (juce::String (p.type) == "notes")
        {
            // MIDI notes, positioned in beats (quarter notes) from the clip start.
            set (property, "type", "array");
            set (property, "items", makeObject ({
                { "type", "object" },
                { "properties", makeObject ({
                    { "pitch", makeObject ({ { "type", "integer" }, { "description", "MIDI note number: 60 = middle C, 69 = A 440 Hz" } }) },
                    { "start_beats", makeObject ({ { "type", "number" }, { "description", "Start, in beats from the clip start" } }) },
                    { "length_beats", makeObject ({ { "type", "number" }, { "description", "Length in beats (1 = a quarter note)" } }) },
                    { "velocity", makeObject ({ { "type", "number" }, { "description", "Loudness 0-1 (default 0.8)" } }) } }) },
                { "required", juce::Array<juce::var> { "pitch", "start_beats", "length_beats" } } }));
        }
        else if (juce::String (p.type) == "ranges")
        {
            // Stretches of a clip (the sustain pedal), in beats from the clip start.
            set (property, "type", "array");
            set (property, "items", makeObject ({
                { "type", "object" },
                { "properties", makeObject ({
                    { "start_beats", makeObject ({ { "type", "number" }, { "description", "Pedal down, in beats from the clip start" } }) },
                    { "length_beats", makeObject ({ { "type", "number" }, { "description", "How long it stays down, in beats" } }) } }) },
                { "required", juce::Array<juce::var> { "start_beats", "length_beats" } } }));
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
          "Play, stop, record (on armed tracks), return to zero, or move the playback cursor (to a time or a marker).",
          schema ({ { "action", "string", "What to do", true, { "play", "stop", "record", "locate", "return_to_zero" } },
                    { "seconds", "number", "Cursor position in seconds (for locate)" },
                    { "marker", "integer", "Marker number to go to (for locate, instead of seconds)" } }) },
        { "set_loop", "Loop playback",
          "Turn loop playback on or off and set the range. While playing, the range repeats seamlessly (not while recording).",
          schema ({ { "enabled", "boolean", "Loop playback on/off", true },
                    { "start_seconds", "number", "Loop start in seconds (default: the current range)" },
                    { "end_seconds", "number", "Loop end in seconds" } }) },
        { "set_punch", "Punch recording",
          "Record only between start and end: recording plays from wherever it starts (a pre-roll), writes takes "
          "only within the range, then keeps playing. Useful to fix one phrase without touching the rest.",
          schema ({ { "enabled", "boolean", "Punch recording on/off", true },
                    { "start_seconds", "number", "Punch-in (default: the current range)" },
                    { "end_seconds", "number", "Punch-out" } }) },
        { "set_click", "Click and count-in",
          "The metronome click (accented on beat 1 of each bar, at the session tempo) and the count-in before recording.",
          schema ({ { "enabled", "boolean", "Click on/off while playing and recording" },
                    { "level_db", "number", "Click level, -60 to 0 dB (default -6)" },
                    { "count_in_bars", "integer", "Bars of click before recording starts, 0-8 (0 = none)" } }) },
        { "add_marker", "Add a marker",
          "Add a named marker (memory location) at a time, to jump back to later. Returns its number.",
          schema ({ { "name", "string", "Marker name, e.g. \"Chorus\"" },
                    { "seconds", "number", "Position in seconds (default: the playback cursor)" } }) },
        { "list_markers", "List markers",
          "Every marker with its number, name and position, in time order.", schema ({}) },
        { "delete_marker", "Delete a marker",
          "Remove a marker by number (undoable).",
          schema ({ { "marker", "integer", "Marker number", true } }) },
        { "set_track", "Change a track",
          "Set any of a track's mixer settings. Level changes glide smoothly (no clicks). Only the given fields change.",
          schema ({ { "track", "track", trackHelp, true },
                    { "name", "string", "New name" },
                    { "volume_db", "number", "Fader level in dB, -144 (off) to +12; 0 is unity" },
                    { "pan", "number", "-1 (left) to +1 (right); 0 is centre" },
                    { "mute", "boolean", "Mute" },
                    { "solo", "boolean", "Solo" },
                    { "record_arm", "boolean", "Record-enable: audio tracks record their input, instrument tracks record MIDI from the keyboard (and play it live)" },
                    { "input", "integer", "Audio input channel, 1-based (0 = no input)" },
                    { "automation_mode", "string", "off, read (play the lanes), touch / latch / write (record control moves while playing)",
                      false, { "off", "read", "touch", "latch", "write" } } }) },
        { "duplicate_track", "Duplicate a track",
          "Copy a track (clips, automation, sends, plug-ins with their settings) to a new track right after it, "
          "named <name>.dup1. Audio files are shared, not copied.",
          schema ({ { "track", "track", trackHelp, true } }) },
        { "playlist", "Playlists and comping",
          "A track's playlists: alternate versions of its clips (takes). Only the active playlist plays. "
          "list: names and which is active. new: start an empty playlist (the current one is kept, e.g. before "
          "recording another take). duplicate: start a copy of the current one. switch: make \"name\" active. "
          "rename: \"name\" to \"new_name\". delete: remove \"name\" (not the active one). comp: copy what "
          "playlist \"name\" has between start_seconds and end_seconds onto the active playlist, replacing what "
          "was there (to build one performance from the best parts of several takes).",
          schema ({ { "track", "track", trackHelp, true },
                    { "action", "string", "What to do", true, { "list", "new", "duplicate", "switch", "rename", "delete", "comp" } },
                    { "name", "string", "The playlist to switch to, rename, delete or comp from" },
                    { "new_name", "string", "rename: the new name" },
                    { "start_seconds", "number", "comp: start of the range" },
                    { "end_seconds", "number", "comp: end of the range" } }) },
        { "move_track", "Move a track",
          "Change a track's position in the track list (1 = first). The mix is unchanged; only the order is.",
          schema ({ { "track", "track", trackHelp, true },
                    { "position", "integer", "New position, 1-based", true } }) },
        { "create_track", "Create tracks",
          "Create audio tracks (play and record audio), aux tracks (buses that receive sends, e.g. for a shared "
          "reverb), instrument tracks (play MIDI notes through Meridian's built-in synthesiser) or drum tracks (the "
          "built-in drum machine). Add notes with add_midi_clip and add_notes; shape the sound with set_synth.",
          schema ({ { "kind", "string", "Track type", false, { "audio", "aux", "instrument", "drums" } },
                    { "channels", "integer", "1 = mono, 2 = stereo (aux and instrument tracks are always stereo)" },
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
        { "remove_send", "Remove a send", "Remove one of a track's sends (undoable). The bus it fed is kept.",
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
        { "list_plugin_parameters", "List a plug-in's controls",
          "Every control of the plug-in in an insert slot: its id, name and current value. Built-in effects: "
          "\"Meridian EQ\" (hpOn, hpFreq, lsFreq, lsGain, p1Freq/p1Gain/p1Q to p3..., hsFreq, hsGain, lpOn, lpFreq, "
          "output) and \"Meridian Reverb\" (size, decay, damping, predelay, mix, width). For Meridian's dynamics "
          "(Compressor, Limiter, Gate, De-Esser) it also gives gain_reduction_db: how far it is turning the "
          "signal down right now. For \"Meridian Tuner\" it gives pitch: the note being played, cents off "
          "and frequency (the reference A4 is its \"reference\" control).",
          schema ({ { "track", "track", trackHelp, true }, { "slot", "string", "Insert slot A-J (or 1-10)", true } }) },
        { "set_plugin_parameter", "Set a plug-in control",
          "Set one control of the plug-in in an insert slot, by id or name, in its own units (Hz, dB, seconds, "
          "0-1 amounts; 1 or 0 for on/off). Give \"normalized\" (0-1) instead for controls that do not accept "
          "typed values. Not part of the undo history.",
          schema ({ { "track", "track", trackHelp, true },
                    { "slot", "string", "Insert slot A-J (or 1-10)", true },
                    { "parameter", "string", "Control id or name (from list_plugin_parameters)", true },
                    { "value", "number", "New value in the control's units" },
                    { "normalized", "number", "New value as 0-1" } }) },
        { "set_plugin_bypass", "Bypass a plug-in", "Bypass (true) or enable (false) an insert.",
          schema ({ { "track", "track", trackHelp, true },
                    { "slot", "string", "Insert slot A-J (or 1-10)", true },
                    { "bypass", "boolean", "true = bypassed", true } }) },
        { "list_clips", "List clips", "Audio and MIDI clips on one track (or all tracks), with positions in seconds.",
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
        { "set_automation", "Automation",
          "Draw any automatable control over time: breakpoints of {seconds, value}, joined by straight lines and held "
          "before the first and after the last. parameter: \"volume\" (dB), \"pan\" (-1 left .. +1 right), "
          "\"mute\" (0 or 1), \"send A\"..\"send E\" (dB), or \"insert A: <control>\" for a plug-in control "
          "(value in the control's own units, as list_plugin_parameters shows). Replaces the lane unless "
          "replace=false; an empty points list removes it. The track is switched to Read if its automation was off.",
          schema ({ { "track", "track", trackHelp, true },
                    { "parameter", "string", "What to automate, e.g. \"pan\", \"send A\", \"insert B: Mix\"", true },
                    { "points", "breakpoints", "Breakpoints, e.g. [{\"seconds\": 0, \"value\": -1}, {\"seconds\": 4, \"value\": 1}]", true },
                    { "replace", "boolean", "Replace existing points (default true)" } }) },
        { "set_tempo", "Set the tempo",
          "Tempo in BPM (quarter notes per minute). Without at_bar it sets the starting tempo; with at_bar it adds a "
          "tempo change there (the tempo holds until the next change). MIDI notes and the click follow the tempo map. "
          "clear_changes=true removes every change (and time signature) first.",
          schema ({ { "bpm", "number", "Tempo, 20-999 BPM", true },
                    { "at_bar", "integer", "Bar where the change starts, 1-based (default: the start)" },
                    { "at_beat", "number", "Beat within that bar, 1-based (default 1)" },
                    { "clear_changes", "boolean", "Remove all tempo and time-signature changes first" } }) },
        { "set_meter", "Set the time signature",
          "Time signature from a bar on, e.g. 3/4 or 6/8 (beats are the denominator's notes: eighths in 6/8). "
          "The click accents the first beat of each bar, and bars, the grid and count-ins follow it.",
          schema ({ { "numerator", "integer", "Beats per bar, 1-32", true },
                    { "denominator", "integer", "Beat note: 1, 2, 4, 8, 16 or 32", true },
                    { "at_bar", "integer", "Bar where it starts, 1-based (default 1)" } }) },
        { "add_midi_clip", "Create a MIDI clip",
          "Create an empty MIDI clip on an instrument track, positioned in beats (quarter notes; at 4/4 a bar is 4 "
          "beats). Returns the clip number for add_notes.",
          schema ({ { "track", "track", "Instrument track number or name", true },
                    { "start_beat", "number", "Where the clip starts, in beats from the session start (0 = beginning)" },
                    { "length_beats", "number", "Clip length in beats (default 16 = 4 bars)" },
                    { "name", "string", "Clip name" } }) },
        { "add_notes", "Add notes",
          "Write notes into a MIDI clip. Positions and lengths are in beats from the clip start; pitch is a MIDI "
          "note number (60 = middle C, 62 = D, 64 = E, 65 = F, 67 = G, 69 = A, 71 = B, 72 = the next C). "
          "On drum tracks the pitch picks the drum (General MIDI): 36 kick, 37 side stick, 38 snare, 39 clap, "
          "42 closed hi-hat, 44 pedal hi-hat, 46 open hi-hat, 41 low tom, 45 mid tom, 48 high tom, 49 crash, "
          "51 ride, 56 cowbell. Notes outside the clip are kept but not heard until the clip is lengthened. "
          "\"sustain\" holds the sustain pedal down over stretches of the clip (released keys keep ringing, "
          "as on a piano); it replaces the clip's pedal.",
          schema ({ { "track", "track", "Instrument track number or name", true },
                    { "clip", "track", "Clip number (from list_clips) or name", true },
                    { "notes", "notes", "Notes to add, e.g. [{\"pitch\": 60, \"start_beats\": 0, \"length_beats\": 1}] ([] for none)", true },
                    { "replace", "boolean", "Remove the clip's existing notes first (default false)" },
                    { "sustain", "ranges", "Where the sustain pedal is down, e.g. [{\"start_beats\": 0, \"length_beats\": 4}] ([] lifts it everywhere)" } }) },
        { "list_notes", "List notes", "The notes of a MIDI clip, and where its sustain pedal is down, in beats from the clip start.",
          schema ({ { "track", "track", "Instrument track number or name", true },
                    { "clip", "track", "Clip number (from list_clips) or name", true } }) },
        { "set_synth", "Shape an instrument's sound",
          "Change an instrument track's sound: the synthesiser's settings, or (engine \"drums\") the drum machine's "
          "tune, decay and tone. Only the given settings change.",
          schema ({ { "track", "track", "Instrument or drum track number or name", true },
                    { "engine", "string", "synth (melodic) or drums (General MIDI drum kit)", false, { "synth", "drums" } },
                    { "tune_semitones", "number", "Drums: pitch of every drum, -12 to +12 semitones" },
                    { "decay_scale", "number", "Drums: tail length, 0.25 (tight) to 4 (long)" },
                    { "tone", "number", "Drums: 0 (dark) to 1 (bright)" },
                    { "waveform", "string", "Oscillator: sine (pure, soft), saw (bright, rich), square (hollow)", false,
                      { "sine", "saw", "square" } },
                    { "detune_cents", "number", "Second oscillator detune, 0-50 cents (0 = single, thinner; 7 = gentle width)" },
                    { "attack", "number", "Seconds to reach full level (0.001 = sharp, 0.5 = slow swell)" },
                    { "decay", "number", "Seconds to fall 60 dB toward the sustain level" },
                    { "sustain", "number", "Level held while the note is down, 0-1" },
                    { "release", "number", "Seconds to fade 60 dB after the note ends" },
                    { "cutoff_hz", "number", "Low-pass filter cutoff, 20-20000 Hz (lower = darker)" },
                    { "resonance", "number", "Filter resonance 0-1" },
                    { "filter_env_octaves", "number", "How far each note opens the filter, in octaves (-8 to 8)" },
                    { "gain_db", "number", "Synth output level, -60 to +12 dB" } }) },
        { "set_master", "Master bus",
          "Master fader and the analog console emulation (saturation) on the master bus.",
          schema ({ { "volume_db", "number", "Master fader in dB" },
                    { "console", "boolean", "Console emulation on/off" },
                    { "drive_db", "number", "Console drive, 0-24 dB (how early saturation starts)" },
                    { "character", "number", "Console asymmetry 0-1 (more = more 2nd harmonic warmth)" },
                    { "curve", "string", "Saturation curve", false, { "tanh", "cubic" } } }) },
        { "bounce", "Bounce (export) the mix",
          "Mix the session down to a file in the session's Bounced Files folder. Runs in the background; get_status "
          "reports progress, then the file, its peak levels, integrated loudness (LUFS), loudness range and true peak. "
          "normalize_lufs gains the whole mix to that loudness (e.g. -14 for streaming), never past ceiling_dbtp.",
          schema ({ { "start_seconds", "number", "Optional start (default 0)" },
                    { "end_seconds", "number", "Optional end (default: end of the last clip + 1 s)" },
                    { "format", "string", "File format (default: the session's)", false, { "wav", "aiff", "flac" } },
                    { "bit_depth", "integer", "16, 24 or 32 (float; not FLAC). Default: the session's" },
                    { "dither", "boolean", "TPDF dither when writing 16 or 24 bits (default: on for 16-bit)" },
                    { "noise_shaping", "boolean", "With dither: move its noise away from 2-5 kHz, where hearing is most "
                                                  "sensitive (less audible hiss on 16-bit files; default off)" },
                    { "normalize_lufs", "number", "Integrated loudness to reach, e.g. -14 (streaming) or -23 (broadcast)" },
                    { "ceiling_dbtp", "number", "True-peak ceiling for normalising (default -1 dBTP)" } }) },
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

namespace
{
bool isFiniteNumber (const juce::var& v)
{
    return (v.isInt() || v.isInt64() || v.isDouble()) && std::isfinite ((double) v);
}

/** A string that is entirely one finite decimal number, e.g. "-6.5". */
bool isNumericString (const juce::var& v)
{
    if (! v.isString())
        return false;
    const auto s = v.toString().trim();
    return s.isNotEmpty() && s.containsOnly ("+-.0123456789eE") && s.containsAnyOf ("0123456789")
           && std::isfinite (s.getDoubleValue());
}

bool isDbWord (const juce::var& v)
{
    const auto s = v.toString().trim();
    return v.isString() && (s.startsWithIgnoreCase ("-inf") || s.equalsIgnoreCase ("off"));
}

/** Checks one argument against its schema entry. Returns an error, or empty. */
juce::String checkArgument (const juce::String& name, const juce::var& value, const juce::var& property)
{
    const auto type = property["type"];
    const bool dbParameter = name.endsWith ("_db") || name == "db";

    if (type.isArray()) // "track": a number or a name
        return isFiniteNumber (value) || value.isString() ? juce::String() : "\"" + name + "\" must be a track number or name";

    const auto t = type.toString();
    if (t == "number" || t == "integer")
    {
        if (dbParameter && (value.isVoid() || isDbWord (value)))
            return {};
        if (! isFiniteNumber (value) && ! isNumericString (value))
            return "\"" + name + "\" must be a finite number";
        return {};
    }
    if (t == "boolean")
        return value.isBool() || (isFiniteNumber (value) && (juce::exactlyEqual ((double) value, 0.0) || juce::exactlyEqual ((double) value, 1.0)))
                   ? juce::String() : "\"" + name + "\" must be true or false";
    if (t == "string")
        return value.isString() || isFiniteNumber (value) ? juce::String() : "\"" + name + "\" must be a string";
    if (t == "array")
    {
        // Every entry must match the item schema: an object with the required
        // fields, each field checked like a top-level argument.
        const auto* items = value.getArray();
        if (items == nullptr)
            return "\"" + name + "\" must be an array";
        const auto itemSchema = property["items"];
        for (int i = 0; i < items->size(); ++i)
        {
            const auto& item = items->getReference (i);
            const auto where = "entry " + juce::String (i + 1) + " of \"" + name + "\"";
            if (! item.isObject())
                return where + " must be an object";
            if (const auto* required = itemSchema["required"].getArray())
                for (const auto& r : *required)
                    if (! item.hasProperty (r.toString()))
                        return where + " needs \"" + r.toString() + "\"";
            if (auto* fields = itemSchema["properties"].getDynamicObject())
                for (const auto& f : fields->getProperties())
                    if (item.hasProperty (f.name))
                        if (const auto error = checkArgument (f.name.toString(), item[f.name], f.value); error.isNotEmpty())
                            return where + ": " + error;
        }
        return {};
    }
    return {};
}

/** Validates arguments against a tool's published input schema, so no tool
    ever sees a NaN, an infinity, or a value of the wrong type. Arguments the
    schema does not mention are ignored. */
juce::String validateArguments (const juce::var& schema, const juce::var& args)
{
    if (! args.isVoid() && ! args.isObject())
        return "arguments must be a JSON object";

    if (const auto* required = schema["required"].getArray())
        for (const auto& r : *required)
            if (! args.hasProperty (r.toString()))
                return "missing required argument \"" + r.toString() + "\"";

    if (auto* object = args.getDynamicObject())
        if (auto* properties = schema["properties"].getDynamicObject())
            for (const auto& a : object->getProperties())
                if (properties->hasProperty (a.name))
                    if (const auto error = checkArgument (a.name.toString(), a.value, properties->getProperty (a.name)); error.isNotEmpty())
                        return error;
    return {};
}
} // namespace

RemoteTools::Result RemoteTools::call (const juce::String& name, const juce::var& args)
{
    for (const auto& tool : toolTable())
        if (name == tool.name)
            if (const auto error = validateArguments (tool.inputSchema, args); error.isNotEmpty())
                return { false, {}, "Invalid arguments for " + name + ": " + error + "." };

    if (name != "get_status" && name != "list_plugins" && app.getSession() == nullptr)
        return { false, {}, "No session is open. Ask the user to create or open one in the Dashboard." };

    if (name == "get_status")         return getStatus();
    if (name == "transport")          return transport (args);
    if (name == "set_loop")           return setLoop (args);
    if (name == "set_click")          return setClick (args);
    if (name == "set_punch")          return setPunch (args);
    if (name == "add_marker")         return addMarker (args);
    if (name == "list_markers")       return listMarkers();
    if (name == "delete_marker")      return deleteMarker (args);
    if (name == "set_track")          return setTrack (args);
    if (name == "duplicate_track")    return duplicateTrack (args);
    if (name == "move_track")         return moveTrack (args);
    if (name == "playlist")           return playlist (args);
    if (name == "create_track")       return createTrack (args);
    if (name == "delete_track")       return deleteTrack (args);
    if (name == "set_output")         return setOutput (args);
    if (name == "set_send")           return setSend (args);
    if (name == "remove_send")        return removeSend (args);
    if (name == "list_plugins")       return listPlugins (args);
    if (name == "insert_plugin")      return insertPlugin (args);
    if (name == "remove_plugin")      return removePlugin (args);
    if (name == "set_plugin_bypass")  return setPluginBypass (args);
    if (name == "list_plugin_parameters") return listPluginParameters (args);
    if (name == "set_plugin_parameter")   return setPluginParameter (args);
    if (name == "list_clips")         return listClips (args);
    if (name == "split_clip")         return splitClip (args);
    if (name == "delete_clip")        return deleteClip (args);
    if (name == "move_clip")          return moveClip (args);
    if (name == "set_clip")           return setClip (args);
    if (name == "import_audio")       return importAudio (args);
    if (name == "set_volume_automation") return setVolumeAutomation (args);
    if (name == "set_automation")     return setAutomation (args);
    if (name == "set_tempo")          return setTempo (args);
    if (name == "set_meter")          return setMeter (args);
    if (name == "add_midi_clip")      return addMidiClip (args);
    if (name == "add_notes")          return addNotes (args);
    if (name == "list_notes")         return listNotes (args);
    if (name == "set_synth")          return setSynth (args);
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
    // Up to 24 hours: far beyond any session, and far from int64 overflow.
    constexpr double maxSeconds = 24.0 * 60.0 * 60.0;
    const double s = (double) seconds;
    return (juce::int64) std::llround ((std::isfinite (s) ? juce::jlimit (0.0, maxSeconds, s) : 0.0) * app.getSession()->getSampleRate());
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
    auto result = makeObject ({ { "number", number },
                                { "name", track::name (t) },
                                { "kind", track::isAux (t) ? "aux" : track::isInstrument (t) ? "instrument" : "audio" },
                                { "channels", track::channels (t) },
                                { "volume_db", dbOrNull (track::volumeDb (t)) },
                                { "pan", track::pan (t) },
                                { "mute", track::mute (t) },
                                { "solo", track::solo (t) },
                                { "record_arm", track::recordArm (t) },
                                { "input", track::isAudio (t) ? juce::var (input + 1) : juce::var() },
                                { "output", outTrack.isValid() ? track::name (outTrack) : juce::String ("master") },
                                { "sends", sends },
                                { "inserts", inserts },
                                { "clips", track::clips (t).getNumChildren() } });

    if (track::isDrumKit (t))
    {
        const auto d = midimodel::readDrumParams (track::synth (t));
        set (result, "synth", makeObject ({ { "engine", "drums" }, { "tune_semitones", d.tuneSemitones },
                                            { "decay_scale", d.decayScale }, { "tone", d.tone }, { "gain_db", d.gainDb } }));
    }
    else if (track::isInstrument (t))
    {
        const auto p = midimodel::readSynthParams (track::synth (t));
        set (result, "synth", makeObject ({ { "engine", "synth" }, { "waveform", midimodel::waveformName (p.waveform) },
                                            { "detune_cents", p.detuneCents }, { "attack", p.attack }, { "decay", p.decay },
                                            { "sustain", p.sustain }, { "release", p.release }, { "cutoff_hz", p.cutoffHz },
                                            { "resonance", p.resonance }, { "filter_env_octaves", p.filterEnvOctaves },
                                            { "gain_db", p.gainDb } }));
    }
    return result;
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
        set (bounceInfo, "last_integrated_lufs", std::round (last.integratedLufs * 10.0) / 10.0);
        set (bounceInfo, "last_loudness_range_lu", std::round (last.loudnessRange * 10.0) / 10.0);
        set (bounceInfo, "last_true_peak_dbtp", std::round (last.truePeakDbtp * 10.0) / 10.0);
        if (! juce::exactlyEqual (last.appliedGainDb, 0.0))
            set (bounceInfo, "last_normalization_gain_db", std::round (last.appliedGainDb * 100.0) / 100.0);
        if (last.limitedByCeiling)
            set (bounceInfo, "last_limited_by_true_peak_ceiling", true);
    }
    set (status, "bounce", bounceInfo);

    auto* session = app.getSession();
    if (session == nullptr)
    {
        set (status, "session", juce::var());
        return { true, status, {} };
    }

    set (status, "loop_and_click", describeTransportSettings());
    set (status, "tempo_map", describeTempoMap());
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
        if (args.hasProperty ("marker"))
        {
            if (! app.locateToMarker ((int) args["marker"]))
                return { false, {}, "There is no marker " + args["marker"].toString() + ". Call list_markers." };
        }
        else if (args.hasProperty ("seconds"))
        {
            app.locate (secondsToSamples (args["seconds"]));
        }
        else
        {
            return { false, {}, "locate needs \"seconds\" or \"marker\"." };
        }
    }
    else
        return { false, {}, "Unknown action \"" + action + "\" (use play, stop, record, locate or return_to_zero)." };

    if (r.failed())
        return { false, {}, r.getErrorMessage() };
    return { true, makeObject ({ { "done", action } }), {} };
}

RemoteTools::Result RemoteTools::duplicateTrack (const juce::var& args)
{
    juce::String error;
    auto t = findTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };
    auto copy = app.duplicateTrack (t);
    app.getSession()->getUndoManager().setCurrentTransactionName ("AI: Duplicate Track");
    return { true, describeTrack (copy, app.getSession()->indexOfTrack (copy) + 1), {} };
}

RemoteTools::Result RemoteTools::playlist (const juce::var& args)
{
    juce::String error;
    auto t = findTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };
    if (track::isAux (t))
        return { false, {}, "\"" + track::name (t) + "\" is an aux track: it has no clips, so no playlists." };

    auto* session = app.getSession();
    const auto action = args["action"].toString();
    const auto name = args["name"].toString().trim();
    auto report = [&t] (juce::var extra)
    {
        juce::Array<juce::var> all;
        for (const auto& n : playlists::names (t))
            all.add (n);
        auto result = makeObject ({ { "track", track::name (t) }, { "active", playlists::activeName (t) }, { "playlists", all },
                                    { "clips", track::clips (t).getNumChildren() } });
        if (auto* object = extra.getDynamicObject())
            for (const auto& property : object->getProperties())
                result.getDynamicObject()->setProperty (property.name, property.value);
        return Result { true, result, {} };
    };
    auto needsName = [&]
    {
        return "Give \"name\": one of " + playlists::names (t).joinIntoString (", ") + ".";
    };

    if (action == "list")
        return report ({});
    if (action == "new" || action == "duplicate")
    {
        playlists::create (*session, t, action == "duplicate", beginEdit (action == "new" ? "New Playlist" : "Duplicate Playlist"));
        return report ({});
    }
    if (action == "switch")
    {
        if (! playlists::names (t).contains (name))
            return { false, {}, needsName() };
        playlists::activate (t, name, beginEdit ("Switch Playlist"));
        return report ({});
    }
    if (action == "rename")
    {
        if (! playlists::names (t).contains (name))
            return { false, {}, needsName() };
        if (! playlists::rename (t, name, args["new_name"].toString(), beginEdit ("Rename Playlist")))
            return { false, {}, "\"new_name\" must be a name no other playlist of this track has." };
        return report ({});
    }
    if (action == "delete")
    {
        if (name == playlists::activeName (t))
            return { false, {}, "\"" + name + "\" is the active playlist: switch to another one first." };
        if (! playlists::find (t, name).isValid())
            return { false, {}, needsName() };
        playlists::remove (t, name, beginEdit ("Delete Playlist"));
        return report ({});
    }
    if (action == "comp")
    {
        if (! playlists::find (t, name).isValid())
            return { false, {}, name == playlists::activeName (t) ? juce::String ("\"" + name + "\" is already the active playlist: comp from another one.")
                                                                  : needsName() };
        if (! args.hasProperty ("start_seconds") || ! args.hasProperty ("end_seconds"))
            return { false, {}, "comp needs start_seconds and end_seconds." };
        const auto start = std::max<juce::int64> (0, secondsToSamples (args["start_seconds"]));
        const auto end = secondsToSamples (args["end_seconds"]);
        if (end <= start)
            return { false, {}, "end_seconds must be after start_seconds." };
        const int copied = playlists::promote (*session, t, name, start, end, beginEdit ("Comp"));
        return report (makeObject ({ { "clips_copied", copied } }));
    }
    return { false, {}, "Unknown action \"" + action + "\": list, new, duplicate, switch, rename, delete or comp." };
}

RemoteTools::Result RemoteTools::moveTrack (const juce::var& args)
{
    juce::String error;
    auto t = findTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };
    const int position = args["position"];
    const int count = app.getSession()->getNumTracks();
    if (position < 1 || position > count)
        return { false, {}, "position must be 1-" + juce::String (count) + "." };
    app.moveTrack (t, position - 1);
    app.getSession()->getUndoManager().setCurrentTransactionName ("AI: Move Track");
    return { true, describeTrack (t, app.getSession()->indexOfTrack (t) + 1), {} };
}

juce::var RemoteTools::describeTransportSettings() const
{
    const auto& s = app.getSession()->getState();
    return makeObject ({ { "loop_enabled", (bool) s.getProperty (ids::loopEnabled, false) },
                         { "loop_start_seconds", samplesToSeconds (s.getProperty (ids::loopStart, 0)) },
                         { "loop_end_seconds", samplesToSeconds (s.getProperty (ids::loopEnd, 0)) },
                         { "punch_enabled", (bool) s.getProperty (ids::punchEnabled, false) },
                         { "punch_in_seconds", samplesToSeconds (s.getProperty (ids::punchIn, 0)) },
                         { "punch_out_seconds", samplesToSeconds (s.getProperty (ids::punchOut, 0)) },
                         { "click_enabled", (bool) s.getProperty (ids::clickEnabled, false) },
                         { "click_level_db", (double) s.getProperty (ids::clickLevelDb, -6.0) },
                         { "count_in_bars", (int) s.getProperty (ids::countInBars, 0) } });
}

RemoteTools::Result RemoteTools::setLoop (const juce::var& args)
{
    const auto& s = app.getSession()->getState();
    const auto start = args.hasProperty ("start_seconds") ? secondsToSamples (args["start_seconds"]) : (juce::int64) s.getProperty (ids::loopStart, 0);
    const auto end = args.hasProperty ("end_seconds") ? secondsToSamples (args["end_seconds"]) : (juce::int64) s.getProperty (ids::loopEnd, 0);
    const bool enabled = args["enabled"];
    if (enabled && end <= start)
        return { false, {}, "The loop range is empty: give start_seconds and end_seconds with end after start." };
    app.setLoop (enabled, start, end);
    return { true, describeTransportSettings(), {} };
}

RemoteTools::Result RemoteTools::setPunch (const juce::var& args)
{
    const auto& s = app.getSession()->getState();
    const auto in = args.hasProperty ("start_seconds") ? secondsToSamples (args["start_seconds"]) : (juce::int64) s.getProperty (ids::punchIn, 0);
    const auto out = args.hasProperty ("end_seconds") ? secondsToSamples (args["end_seconds"]) : (juce::int64) s.getProperty (ids::punchOut, 0);
    const bool enabled = args["enabled"];
    if (enabled && out <= in)
        return { false, {}, "The punch range is empty: give start_seconds and end_seconds with end after start." };
    app.setPunch (enabled, in, out);
    return { true, describeTransportSettings(), {} };
}

RemoteTools::Result RemoteTools::setClick (const juce::var& args)
{
    const auto& s = app.getSession()->getState();
    if (args.hasProperty ("count_in_bars"))
    {
        const int bars = args["count_in_bars"];
        if (bars < 0 || bars > 8)
            return { false, {}, "count_in_bars must be 0-8." };
        app.setCountInBars (bars);
    }
    const bool enabled = args.hasProperty ("enabled") ? (bool) args["enabled"] : (bool) s.getProperty (ids::clickEnabled, false);
    const double level = args.hasProperty ("level_db") ? (double) args["level_db"] : (double) s.getProperty (ids::clickLevelDb, -6.0);
    if (level < -60.0 || level > 0.0)
        return { false, {}, "level_db must be between -60 and 0." };
    app.setClick (enabled, level);
    return { true, describeTransportSettings(), {} };
}

juce::var RemoteTools::describeMarker (const juce::ValueTree& m) const
{
    return makeObject ({ { "marker", (int) m[ids::number] }, { "name", m[ids::name].toString() },
                         { "seconds", samplesToSeconds ((juce::int64) m[ids::start]) } });
}

RemoteTools::Result RemoteTools::addMarker (const juce::var& args)
{
    const auto position = args.hasProperty ("seconds") ? secondsToSamples (args["seconds"])
                                                       : app.getEngine().getTransport().getPosition();
    const auto m = app.getSession()->addMarker (args["name"].toString(), position, beginEdit ("Add Marker"));
    return { true, describeMarker (m), {} };
}

RemoteTools::Result RemoteTools::listMarkers()
{
    juce::Array<juce::var> list;
    for (const auto& m : app.getSession()->getMarkers())
        list.add (describeMarker (m));
    return { true, makeObject ({ { "markers", list } }), {} };
}

RemoteTools::Result RemoteTools::deleteMarker (const juce::var& args)
{
    const auto m = app.getSession()->getMarkerByNumber ((int) args["marker"]);
    if (! m.isValid())
        return { false, {}, "There is no marker " + args["marker"].toString() + ". Call list_markers." };
    app.getSession()->removeMarker (m, beginEdit ("Delete Marker"));
    return { true, makeObject ({ { "deleted", (int) args["marker"] } }), {} };
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
    if (args.hasProperty ("automation_mode"))
        t.setProperty (ids::automationMode, args["automation_mode"].toString(), um); // checked against the schema's list
    if (args.hasProperty ("input"))
    {
        if (! track::isAudio (t))
            return { false, {}, track::isAux (t) ? "Aux tracks have no audio input; route or send other tracks to them instead."
                                                 : "Instrument tracks have no audio input; they play their MIDI clips." };
        t.setProperty (ids::inputChannel, juce::jmax (0, (int) args["input"]) - 1, um);
    }

    return { true, describeTrack (t, app.getSession()->indexOfTrack (t) + 1), {} };
}

RemoteTools::Result RemoteTools::createTrack (const juce::var& args)
{
    auto* session = app.getSession();
    const auto kindName = args["kind"].toString();
    const bool aux = kindName == "aux", drums = kindName == "drums", instrument = kindName == "instrument" || drums;
    const int channels = aux || instrument ? 2 : juce::jlimit (1, 2, args.hasProperty ("channels") ? (int) args["channels"] : 1);
    const int count = juce::jlimit (1, 64, args.hasProperty ("count") ? (int) args["count"] : 1);
    const auto requested = args["name"].toString().trim();

    juce::Array<juce::var> created;
    auto* um = beginEdit (count == 1 ? "New Track" : "New Tracks");
    for (int i = 0; i < count; ++i)
    {
        juce::String name = requested.isNotEmpty() ? (count == 1 ? requested : requested + " " + juce::String (i + 1))
                                                   : juce::String (aux ? "Aux " : drums ? "Drums " : instrument ? "Synth " : "Audio ")
                                                         + juce::String (session->getNumTracks() + 1);
        auto t = session->addTrack (name, channels, um, aux ? Session::TrackKind::aux
                                                        : drums ? Session::TrackKind::drums
                                                        : instrument ? Session::TrackKind::instrument : Session::TrackKind::audio);
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
    for (const auto& d : app.getPluginHost().getKnownPlugins().getTypes())
        if (filter.isEmpty() || d.name.containsIgnoreCase (filter) || d.manufacturerName.containsIgnoreCase (filter))
            list.add (makeObject ({ { "name", d.name }, { "manufacturer", d.manufacturerName },
                                    { "format", d.pluginFormatName }, { "category", d.category },
                                    { "instrument", d.isInstrument } }));
    auto result = makeObject ({ { "plugins", list },
                                { "note", "Instruments (instrument: true) go in insert A of an instrument track, where they "
                                          "play its MIDI instead of the built-in synth." } });
    if (list.isEmpty())
        set (result, "note", "No plug-ins are scanned. The user can scan in Setup > Plug-in Manager.");
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
    const auto plugins = app.getPluginHost().getKnownPlugins().getTypes();
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

juce::AudioPluginInstance* RemoteTools::findPlugin (const juce::var& args, juce::String& error) const
{
    auto t = findTrack (args["track"], error);
    if (! t.isValid())
        return nullptr;
    const int slot = readSlot (args["slot"], Session::maxInserts);
    const auto instance = slot >= 0 ? app.getBridge().getInsert (track::id (t), slot) : nullptr;
    if (instance == nullptr || instance->plugin == nullptr)
    {
        error = "There is no loaded plug-in in that slot of \"" + track::name (t) + "\".";
        return nullptr;
    }
    return instance->plugin.get();
}

namespace
{
juce::String parameterId (juce::AudioProcessorParameter& p)
{
    if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (&p))
        return hosted->getParameterID();
    return juce::String (p.getParameterIndex());
}

juce::var describeParameter (juce::AudioProcessorParameter& p)
{
    return makeObject ({ { "id", parameterId (p) }, { "name", p.getName (64) },
                         { "value", p.getCurrentValueAsText() }, { "normalized", std::round (p.getValue() * 10000.0) / 10000.0 } });
}
} // namespace

RemoteTools::Result RemoteTools::listPluginParameters (const juce::var& args)
{
    juce::String error;
    auto* plugin = findPlugin (args, error);
    if (plugin == nullptr)
        return { false, {}, error };
    juce::Array<juce::var> list;
    for (auto* p : plugin->getParameters())
        list.add (describeParameter (*p));
    auto result = makeObject ({ { "plugin", plugin->getName() }, { "parameters", list } });
    if (const auto* meter = dynamic_cast<const GainReductionMeter*> (plugin))
        result.getDynamicObject()->setProperty ("gain_reduction_db", std::round (meter->getGainReductionDb() * 100.0) / 100.0);
    if (auto* tuner = dynamic_cast<PitchReadout*> (plugin))
    {
        const auto reading = tuner->readPitch();
        result.getDynamicObject()->setProperty ("pitch", reading.found
            ? makeObject ({ { "note", reading.note }, { "cents", std::round (reading.cents * 10.0) / 10.0 },
                            { "frequency_hz", std::round (reading.frequencyHz * 100.0) / 100.0 } })
            : juce::var ("none (silence, or not a single clear note)"));
    }
    return { true, result, {} };
}

RemoteTools::Result RemoteTools::setPluginParameter (const juce::var& args)
{
    juce::String error;
    auto* plugin = findPlugin (args, error);
    if (plugin == nullptr)
        return { false, {}, error };
    if (! args.hasProperty ("value") && ! args.hasProperty ("normalized"))
        return { false, {}, "Give \"value\" (in the control's units) or \"normalized\" (0-1)." };

    const auto wanted = args["parameter"].toString().trim();
    juce::AudioProcessorParameter* target = nullptr;
    for (auto* p : plugin->getParameters())
        if (parameterId (*p).equalsIgnoreCase (wanted) || p->getName (64).equalsIgnoreCase (wanted))
            target = p;
    if (target == nullptr)
        return { false, {}, "\"" + plugin->getName() + "\" has no control \"" + wanted + "\". Call list_plugin_parameters." };

    const float normalized = args.hasProperty ("normalized")
                                 ? juce::jlimit (0.0f, 1.0f, (float) (double) args["normalized"])
                                 : juce::jlimit (0.0f, 1.0f, target->getValueForText (juce::String ((double) args["value"])));
    target->setValueNotifyingHost (normalized);
    return { true, makeObject ({ { "plugin", plugin->getName() }, { "parameter", describeParameter (*target) } }), {} };
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
                                      { "kind", clip::isMidi (c) ? "midi" : "audio" },
                                      { "notes", clip::isMidi (c) ? juce::var (clip::notes (c).getNumChildren()) : juce::var() },
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
        if (track::isInstrument (destination) != clip::isMidi (c))
            return { false, {}, clip::isMidi (c) ? "MIDI clips can only go on instrument tracks."
                                                 : "Audio clips cannot go on instrument tracks." };
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
        if (! track::isAudio (target))
            return { false, {}, "Audio can only be imported onto audio tracks (or leave out \"track\" to create one)." };
    }

    app.importAudioFiles ({ juce::File (path) }, secondsToSamples (args["position_seconds"]), target);
    return { true, makeObject ({ { "importing", juce::File (path).getFileName() },
                                 { "note", "Copying and converting in the background; call list_clips in a moment." } }), {} };
}

RemoteTools::Result RemoteTools::setAutomation (const juce::var& args)
{
    juce::String error;
    auto t = findTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };
    const auto* points = args["points"].getArray();
    if (points == nullptr)
        return { false, {}, "\"points\" must be an array of {\"seconds\": number, \"value\": number}." };

    // Which lane, and how to turn a value in the control's units into the lane's.
    const auto wanted = args["parameter"].toString().trim();
    juce::String parameter;
    juce::AudioProcessorParameter* pluginControl = nullptr;
    if (wanted.equalsIgnoreCase ("volume") || wanted.equalsIgnoreCase ("pan") || wanted.equalsIgnoreCase ("mute"))
    {
        parameter = wanted.toLowerCase();
    }
    else if (wanted.startsWithIgnoreCase ("send "))
    {
        const int slot = readSlot (wanted.fromFirstOccurrenceOf (" ", false, false).trim(), Session::maxSends);
        if (slot < 0 || ! routing::findSend (t, slot).isValid())
            return { false, {}, "\"" + track::name (t) + "\" has no send " + wanted.fromFirstOccurrenceOf (" ", false, false).trim() + "." };
        parameter = automation::sendParameter (slot);
    }
    else if (wanted.startsWithIgnoreCase ("insert ") && wanted.contains (":"))
    {
        const auto slotText = wanted.fromFirstOccurrenceOf (" ", false, false).upToFirstOccurrenceOf (":", false, false).trim();
        const auto control = wanted.fromFirstOccurrenceOf (":", false, false).trim();
        auto* plugin = findPlugin (makeObject ({ { "track", args["track"] }, { "slot", slotText } }), error);
        if (plugin == nullptr)
            return { false, {}, error };
        const auto& all = plugin->getParameters();
        for (int i = 0; i < all.size() && pluginControl == nullptr; ++i)
            if (parameterId (*all[i]).equalsIgnoreCase (control) || all[i]->getName (64).equalsIgnoreCase (control))
            {
                pluginControl = all[i];
                parameter = automation::pluginParameter (readSlot (slotText, Session::maxInserts), i);
            }
        if (pluginControl == nullptr)
            return { false, {}, "\"" + plugin->getName() + "\" has no control \"" + control + "\". Call list_plugin_parameters." };
    }
    else
    {
        return { false, {}, "Unknown parameter \"" + wanted + "\". Use volume, pan, mute, \"send A\" or \"insert A: <control>\"." };
    }

    auto* um = beginEdit ("Automation");
    if (points->isEmpty())
    {
        automation::clearLane (t, parameter, um);
        return { true, makeObject ({ { "track", track::name (t) }, { "parameter", wanted }, { "automation", "removed" } }), {} };
    }

    auto lane = automation::getOrCreateLane (t, parameter, um);
    if (! args.hasProperty ("replace") || (bool) args["replace"])
        lane.removeAllChildren (um);
    for (const auto& p : *points)
    {
        if (! p.hasProperty ("seconds") || ! p.hasProperty ("value"))
            return { false, {}, "Each point needs \"seconds\" and \"value\"." };
        const double value = pluginControl != nullptr ? (double) pluginControl->getValueForText (p["value"].toString())
                                                      : (double) p["value"];
        automation::addPoint (lane, secondsToSamples (p["seconds"]), value, um);
    }
    if (track::automationMode (t) == "off")
        t.setProperty (ids::automationMode, "read", um); // otherwise the drawing would be ignored

    juce::Array<juce::var> written;
    for (const auto& p : automation::points (lane))
        written.add (makeObject ({ { "seconds", samplesToSeconds (p.time) },
                                   { "value", pluginControl != nullptr ? juce::var (pluginControl->getText ((float) p.value, 32))
                                                                       : juce::var (p.value) } }));
    return { true, makeObject ({ { "track", track::name (t) }, { "parameter", wanted }, { "points", written } }), {} };
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
        written.add (makeObject ({ { "seconds", samplesToSeconds (p.time) }, { "db", dbOrNull (p.value) } }));
    return { true, makeObject ({ { "track", track::name (t) }, { "points", written } }), {} };
}

//==============================================================================
// Instruments and MIDI

namespace
{
constexpr double maxBeats = 100000.0; // ~7 hours at 240 BPM: far beyond any session

int64_t beatsToTicks (double beats)
{
    return (int64_t) std::llround (juce::jlimit (0.0, maxBeats, beats) * (double) midi::ticksPerQuarter);
}
} // namespace

juce::ValueTree RemoteTools::findInstrumentTrack (const juce::var& reference, juce::String& error) const
{
    auto t = findTrack (reference, error);
    if (t.isValid() && ! track::isInstrument (t))
    {
        error = "\"" + track::name (t) + "\" is not an instrument track. Create one with create_track kind=instrument.";
        return {};
    }
    return t;
}

RemoteTools::Result RemoteTools::setTempo (const juce::var& args)
{
    const double bpm = args["bpm"];
    if (bpm < midi::TempoMap::minBpm || bpm > midi::TempoMap::maxBpm)
        return { false, {}, "Tempo must be between 20 and 999 BPM." };
    auto& session = *app.getSession();
    auto* um = beginEdit ("Tempo");
    if ((bool) args["clear_changes"])
        midimodel::clearTempoAndMeterChanges (session, um);

    juce::int64 tick = 0;
    if (args.hasProperty ("at_bar"))
    {
        const int bar = args["at_bar"];
        const double beat = args.hasProperty ("at_beat") ? (double) args["at_beat"] : 1.0;
        const auto meter = midimodel::meterMap (session);
        const auto& m = meter.meterAt (bar - 1);
        if (bar < 1 || beat < 1.0 || beat >= m.numerator + 1.0)
            return { false, {}, "at_bar must be 1 or more and at_beat within the bar (1-" + juce::String (m.numerator) + ")." };
        tick = meter.barToTicks (bar - 1) + (juce::int64) std::llround ((beat - 1.0) * (double) midi::MeterMap::ticksPerBeat (m));
    }
    midimodel::setTempoChange (session, tick, bpm, um);
    return { true, describeTempoMap(), {} };
}

RemoteTools::Result RemoteTools::setMeter (const juce::var& args)
{
    const int numerator = args["numerator"], denominator = args["denominator"];
    const int bar = args.hasProperty ("at_bar") ? (int) args["at_bar"] : 1;
    const bool powerOfTwo = denominator >= 1 && denominator <= 32 && (denominator & (denominator - 1)) == 0;
    if (numerator < 1 || numerator > 32 || ! powerOfTwo || bar < 1)
        return { false, {}, "numerator 1-32, denominator 1, 2, 4, 8, 16 or 32, at_bar 1 or more." };
    midimodel::setMeterChange (*app.getSession(), bar - 1, numerator, denominator, beginEdit ("Time Signature"));
    return { true, describeTempoMap(), {} };
}

juce::var RemoteTools::describeTempoMap() const
{
    const auto& session = *app.getSession();
    const auto tempo = midimodel::tempoMap (session);
    const auto meter = midimodel::meterMap (session);
    juce::Array<juce::var> tempos, meters;
    for (const auto& c : tempo.getChanges())
    {
        const auto p = meter.positionOf (c.tick);
        tempos.add (makeObject ({ { "bar", (int) p.bar + 1 }, { "beat", p.beat + 1 }, { "bpm", c.bpm },
                                  { "seconds", std::round (tempo.ticksToSeconds (c.tick) * 1000.0) / 1000.0 } }));
    }
    for (const auto& c : meter.getChanges())
        meters.add (makeObject ({ { "bar", (int) c.bar + 1 }, { "time_signature", juce::String (c.numerator) + "/" + juce::String (c.denominator) } }));
    return makeObject ({ { "tempo_changes", tempos }, { "time_signatures", meters } });
}

RemoteTools::Result RemoteTools::addMidiClip (const juce::var& args)
{
    juce::String error;
    auto t = findInstrumentTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };

    const double startBeat = args.hasProperty ("start_beat") ? (double) args["start_beat"] : 0.0;
    const double lengthBeats = args.hasProperty ("length_beats") ? (double) args["length_beats"] : 16.0;
    if (startBeat < 0.0 || startBeat > maxBeats)
        return { false, {}, "start_beat must be 0 or more." };
    if (lengthBeats <= 0.0 || lengthBeats > maxBeats)
        return { false, {}, "length_beats must be more than 0." };

    auto* session = app.getSession();
    const auto tempo = midimodel::tempoMap (*session);
    const double rate = session->getSampleRate();
    const auto start = tempo.ticksToSamples (beatsToTicks (startBeat), rate);
    const auto end = tempo.ticksToSamples (beatsToTicks (startBeat + lengthBeats), rate);
    const auto name = args["name"].toString().trim();

    auto c = session->addMidiClip (t, (juce::int64) start, std::max<juce::int64> (1, (juce::int64) (end - start)),
                                   name.isNotEmpty() ? name : juce::String ("MIDI"),
                                   beginEdit ("MIDI Clip"));
    return { true, makeObject ({ { "track", track::name (t) }, { "clip", track::clips (t).indexOf (c) + 1 },
                                 { "name", clip::name (c) }, { "start_seconds", samplesToSeconds (clip::start (c)) },
                                 { "length_beats", lengthBeats } }), {} };
}

RemoteTools::Result RemoteTools::addNotes (const juce::var& args)
{
    juce::String error;
    auto t = findInstrumentTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };
    auto c = findClip (t, args["clip"], error);
    if (! c.isValid())
        return { false, {}, error };
    if (! clip::isMidi (c))
        return { false, {}, "That clip is not a MIDI clip." };

    const auto* list = args["notes"].getArray();
    if (list == nullptr || list->size() > 10000)
        return { false, {}, "\"notes\" must be an array of at most 10000 notes." };

    // Positions are given from the clip start; notes are stored from the
    // clip's content origin, which a trimmed clip has moved earlier.
    auto* session = app.getSession();
    const auto tempo = midimodel::tempoMap (*session);
    const auto offsetTicks = tempo.ticksFromOrigin (clip::contentOrigin (c), clip::start (c), session->getSampleRate());

    std::vector<midi::Note> notes;
    for (const auto& n : *list)
    {
        const double pitch = n["pitch"], start = n["start_beats"], length = n["length_beats"];
        const double velocity = n.hasProperty ("velocity") ? (double) n["velocity"] : 0.8;
        if (pitch < 0.0 || pitch > 127.0 || ! juce::exactlyEqual (pitch, std::floor (pitch)))
            return { false, {}, "Each pitch must be a whole number 0-127 (60 = middle C)." };
        if (start < 0.0 || start > maxBeats || length <= 0.0 || length > maxBeats)
            return { false, {}, "Each note needs start_beats of 0 or more and length_beats of more than 0." };
        if (velocity <= 0.0 || velocity > 1.0)
            return { false, {}, "velocity must be more than 0 and at most 1." };
        notes.push_back ({ (int) pitch, (float) velocity, offsetTicks + beatsToTicks (start), juce::jmax<int64_t> (1, beatsToTicks (length)) });
    }

    std::vector<midi::PedalRange> sustain;
    const bool setsSustain = args.hasProperty ("sustain");
    if (setsSustain)
    {
        const auto* ranges = args["sustain"].getArray();
        if (ranges == nullptr || ranges->size() > 10000)
            return { false, {}, "\"sustain\" must be an array of at most 10000 {start_beats, length_beats}." };
        for (const auto& r : *ranges)
        {
            const double start = r["start_beats"], length = r["length_beats"];
            if (start < 0.0 || start > maxBeats || length <= 0.0 || length > maxBeats)
                return { false, {}, "Each sustain range needs start_beats of 0 or more and length_beats of more than 0." };
            sustain.push_back ({ offsetTicks + beatsToTicks (start), juce::jmax<int64_t> (1, beatsToTicks (length)) });
        }
    }

    auto* um = beginEdit ("Add Notes");
    if (setsSustain)
        midimodel::writeSustain (c, sustain, um);
    const bool replace = args.hasProperty ("replace") && (bool) args["replace"];
    if (replace)
    {
        midi::NoteSequence sequence;
        for (const auto& n : notes)
            sequence.add (n);
        midimodel::writeNotes (c, sequence, um);
    }
    else
    {
        for (const auto& n : notes)
            midimodel::addNote (c, n, um);
    }
    return { true, makeObject ({ { "clip", clip::name (c) }, { "added", (int) notes.size() },
                                 { "total_notes", clip::notes (c).getNumChildren() } }), {} };
}

RemoteTools::Result RemoteTools::listNotes (const juce::var& args)
{
    juce::String error;
    auto t = findInstrumentTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };
    auto c = findClip (t, args["clip"], error);
    if (! c.isValid())
        return { false, {}, error };

    auto* session = app.getSession();
    const auto offsetTicks = midimodel::tempoMap (*session).ticksFromOrigin (clip::contentOrigin (c), clip::start (c),
                                                                             session->getSampleRate());
    juce::Array<juce::var> result;
    const auto notes = midimodel::readNotes (c); // named: a temporary would die before the loop runs
    for (const auto& n : notes.getNotes())
        result.add (makeObject ({ { "pitch", n.pitch },
                                  { "start_beats", (double) (n.start - offsetTicks) / (double) midi::ticksPerQuarter },
                                  { "length_beats", (double) n.length / (double) midi::ticksPerQuarter },
                                  { "velocity", std::round (n.velocity * 1000.0f) / 1000.0 } }));
    juce::Array<juce::var> pedal;
    for (const auto& r : midimodel::readSustain (c))
        pedal.add (makeObject ({ { "start_beats", (double) (r.start - offsetTicks) / (double) midi::ticksPerQuarter },
                                 { "length_beats", (double) r.length / (double) midi::ticksPerQuarter } }));
    return { true, makeObject ({ { "clip", clip::name (c) }, { "notes", result }, { "sustain", pedal } }), {} };
}

RemoteTools::Result RemoteTools::setSynth (const juce::var& args)
{
    juce::String error;
    auto t = findInstrumentTrack (args["track"], error);
    if (! t.isValid())
        return { false, {}, error };

    auto synthNode = track::synth (t);
    auto* um = beginEdit ("Synth");
    auto setClamped = [&] (const char* argument, const juce::Identifier& property, double lo, double hi)
    {
        if (args.hasProperty (argument))
            synthNode.setProperty (property, juce::jlimit (lo, hi, (double) args[argument]), um);
    };
    if (args.hasProperty ("engine"))
        synthNode.setProperty (ids::engine, args["engine"].toString() == "drums" ? "drums" : "synth", um);
    setClamped ("tune_semitones", ids::tune, -12.0, 12.0);
    setClamped ("decay_scale", ids::decayScale, 0.25, 4.0);
    setClamped ("tone", ids::tone, 0.0, 1.0);
    if (args.hasProperty ("waveform"))
        synthNode.setProperty (ids::waveform, midimodel::waveformName (midimodel::waveformFromName (args["waveform"].toString())), um);
    setClamped ("detune_cents", ids::detune, 0.0, 50.0);
    setClamped ("attack", ids::attack, 0.0005, 10.0);
    setClamped ("decay", ids::decay, 0.001, 10.0);
    setClamped ("sustain", ids::sustain, 0.0, 1.0);
    setClamped ("release", ids::release, 0.001, 20.0);
    setClamped ("cutoff_hz", ids::cutoff, 20.0, 20000.0);
    setClamped ("resonance", ids::resonance, 0.0, 1.0);
    setClamped ("filter_env_octaves", ids::filterEnv, -8.0, 8.0);
    setClamped ("gain_db", ids::gainDb, -60.0, 12.0);

    return { true, describeTrack (t, app.getSession()->indexOfTrack (t) + 1), {} };
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
    if (args.hasProperty ("bit_depth"))
    {
        request.bitDepth = args["bit_depth"];
        if (request.bitDepth != 16 && request.bitDepth != 24 && request.bitDepth != 32)
            return { false, {}, "bit_depth must be 16, 24 or 32." };
        request.dither = request.bitDepth == 16;
    }
    if (args.hasProperty ("format"))
    {
        request.format = args["format"].toString();
        if (request.format == "flac" && request.bitDepth == 32)
            return { false, {}, "FLAC stores 16 or 24 bits; give bit_depth 16 or 24." };
        request.destination = request.destination.getParentDirectory().getNonexistentChildFile (
            request.destination.getFileNameWithoutExtension(), request.format == "aiff" ? ".aif" : "." + request.format, false);
    }
    if (args.hasProperty ("dither"))
        request.dither = args["dither"];
    if (args.hasProperty ("noise_shaping"))
        request.noiseShaping = args["noise_shaping"];
    if (args.hasProperty ("normalize_lufs"))
    {
        request.normalise = true;
        request.targetLufs = args["normalize_lufs"];
        request.ceilingDbtp = args.hasProperty ("ceiling_dbtp") ? (double) args["ceiling_dbtp"] : -1.0;
        if (request.targetLufs < -60.0 || request.targetLufs > 0.0 || request.ceilingDbtp < -20.0 || request.ceilingDbtp > 0.0)
            return { false, {}, "normalize_lufs must be -60..0 and ceiling_dbtp -20..0." };
    }

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
    if (remote::exceedsJsonNesting (message))
        return toJson (makeObject ({ { "ok", false },
                                     { "error", "JSON nested deeper than " + juce::String (remote::maxJsonNesting) + " levels." } })).toStdString();
    if (! remote::isValidJson (message)) // strict check: only well-formed JSON reaches the parser
        return toJson (makeObject ({ { "ok", false }, { "error", "The message is not valid JSON." } })).toStdString();

    const auto request = juce::JSON::parse (juce::String::fromUTF8 (message.data(), (int) message.size()));
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
    auto error = [] (const juce::var& id, int code, const juce::String& message)
    {
        return toJson (makeObject ({ { "jsonrpc", "2.0" }, { "id", id },
                                     { "error", makeObject ({ { "code", code }, { "message", message } }) } })).toStdString();
    };

    if (remote::exceedsJsonNesting (body))
        return error ({}, -32700, "Parse error: JSON nested deeper than " + juce::String (remote::maxJsonNesting) + " levels");
    if (! remote::isValidJson (body)) // HTTP bodies arrive unchecked from the network
        return error ({}, -32700, "Parse error: the request body is not valid JSON (RFC 8259, UTF-8)");

    const auto request = juce::JSON::parse (juce::String::fromUTF8 (body.data(), (int) body.size()));

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
