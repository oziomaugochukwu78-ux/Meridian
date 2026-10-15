# Meridian

An open-source digital audio workstation for Windows and macOS, written in C++20 on JUCE 9. It is laid out like a classic console-style DAW (Edit and Mix windows), and adds two things of its own:

- a 64-bit double-precision mix engine with compensated (double-double) summing and an oversampled analog-console stage on the master bus, and
- a local AI remote-control port: a WebSocket server that lets an AI agent or script drive the session with plain text commands.

"Meridian" is a working title, set in one place: `MERIDIAN_PRODUCT_NAME` in `CMakeLists.txt`.

## Status

This is an early foundation, not a finished DAW. Everything listed as working compiles, runs and is checked by the automated tests described under [Testing](#testing). No feature depends on anyone listening to it. See [ROADMAP.md](ROADMAP.md) for what comes next.

| Area | Working now | Planned |
|---|---|---|
| Sessions | Dashboard (create from template or blank, recent, open; genre templates for Hip-Hop, Electronic, Band Tracking, Singer-Songwriter, Stereo Stems, Podcast, Voice-Over and Film (DX/MX/FX stems), each with its tracks, effect chains, reverb/delay buses and sends, routing, tempo and console preset), save/load (atomic), Save As (to any folder; clip paths re-stored so audio is still found), full undo/redo, automatic repair of damaged session files on open; automatic backups of unsaved work every 5 minutes into `Session File Backups/` (newest 5 kept); crash recovery: if Meridian did not close normally, the newer backup is offered when the session is next opened | Session import |
| Edit window | Tracks, clips with waveforms, Smart Tool (select/move/trim/fade zones), clip gain (a handle on each audio clip: drag up or down, Shift for fine, double-click for 0 dB; the waveform and a gain line show it), Shuffle, Slip, Spot and Grid modes (Shuffle: deleting closes the gap, trimming moves the clips after it, with their automation, and a moved clip drops in at the nearest clip boundary; Spot: click a clip to type its start time), separate, delete, zoom, page-scroll, drag-and-drop import, automation drawing with the Pencil on the lane each track header's view selector picks (volume, pan, mute, a send level or any plug-in control, each on its own scale: the fader taper, left to right, steps, the control's range), markers (memory locations) in a ruler row, with the loop range shown; track Duplicate (.dup1, with plug-in settings) and Move Up/Down; playlists (alternate takes on a track: new, duplicate, switch, rename, delete, from a selector in each track header) and comping (Comp Selection From: the selected range of another playlist replaces the active one's, sample-accurate, one undo step) | Zoomer tool, a playlist lane view for comping, trim mode |
| Mix window | Channel strips: 10 inserts, 5 sends (level, pan, pre/post, mute), Inserts F-J and Sends A-E can be hidden (Window menu, remembered) to give the faders room on short screens, and a Simple Mode shows inserts and sends only once a track uses them and hides the I/O and automation-mode controls (nothing is disabled), output routing to master or any bus, I/O, automation modes Off/Read/Touch/Latch/Write/Trim (Trim: the fader rides the volume automation as a dB offset; Coalesce Trim in the track menu writes the offset into the lane) (holding a fader, pan, send level or a plug-in knob in its own window records it; a held control overrides its lane), pan, R/S/M, console-taper fader, segmented meters; Aux Input (bus) tracks, bus-to-bus chains with loop protection, implicit solo; master strip with console controls and factory console presets (Clean, Warm Tape, Vintage Console, Gritty; new sessions start on Warm Tape) | VCAs, mono buses, surround |
| Audio engine | Lock-free render graph, double-buffered disk streaming, record to disk with round-trip latency compensation, input monitoring, automation of volume, pan and send levels (sample-accurate), mute (stepped, declicked) and any plug-in control (per block), ramped gain/mute/pan, automatic plug-in delay compensation across tracks, sends and buses, offline bounce to WAV/AIFF/FLAC (File > Bounce Mix) with loudness reported (integrated LUFS, loudness range, true peak), optional loudness normalisation under a true-peak ceiling, and TPDF dither for 16-bit, optionally noise-shaped (the hiss moved away from 2-5 kHz; the bounce tool's noise_shaping); a live loudness meter on the master (momentary, short-term, integrated, true peak; EBU R128); seamless loop playback (sample-accurate wrap; the loop start is kept loaded from disk so the jump never waits); click (accented downbeat, exact beat samples) and count-in of 1 or 2 bars before recording; punch recording (takes written only within the punch range, sample-accurate, with pre- and post-roll) | Multi-core track rendering, MP3 export, manual recording offset, loop recording |
| Mixing maths | 64-bit compensated summing bus, SIMD (SSE2/AVX/NEON) mix kernels, constant-power pan, 4x-oversampled tanh/cubic console saturation (drive -12 to +24 dB), with factory presets each tuned to a measured THD band and harmonic balance | Per-track console channels |
| Plug-ins | VST3 and LV2 hosting (plus AU on macOS), scanning with crash protection, editors, bypass, state saved in the session; instrument plug-ins in insert A of an instrument track, played sample-accurately from its MIDI clips and the keyboard | CLAP hosting (needs a CLAP-SDK format adapter), sidechains, multi-output instruments |
| Built-in effects | Meridian EQ: 7 bands (high-pass, low shelf, 3 peaks, high shelf, low-pass) plus output gain. It matches its design curve within 0.05 dB and passes audio bit-for-bit when flat. Meridian Compressor: threshold, ratio, soft knee, attack, release, makeup and mix (parallel compression). Its static curve is exact on steady signals, attack and release are the true 63% times, and at 1:1 it passes audio bit for bit. Meridian Limiter: look-ahead brickwall with true-peak detection (4x interpolation). No sample can exceed the ceiling (a proof is in the code); true peaks stay within 0.2 dB of it on programme material; the gain ramps so it cannot click; and its 1.8 ms latency is compensated. Meridian Gate: threshold with hysteresis (no chatter), range, attack, hold, release; bit-exact when open, exactly the range when closed, hold exact to the sample. Meridian De-Esser: threshold, frequency (2–12 kHz) and range. It listens above the frequency and turns down a high shelf by (level − threshold) × 3/4 dB, never more than the range; below the band, audio passes bit for bit, and the cut matches the shelf's designed response. Meridian Delay: up to 2 s, feedback, ping-pong, high-cut in the loop; echoes exact to the sample and each repeat exactly the feedback quieter. Meridian Reverb: 8-line feedback delay network with size, decay, damping, pre-delay, mix and width; its decay time measures within 10% of the setting, and the left/right tails are decorrelated. All appear in the plug-in list without a scan and are controlled by AI tools. The dynamics (Compressor, Limiter, Gate, De-Esser) show a gain-reduction meter in their windows and report it to the AI tools; on a steady tone each reads its specification's reduction to within 0.15 dB. Meridian Tuner: note, cents and frequency with an adjustable reference (A4 = 430–450 Hz); audio passes bit for bit; sines from 30 Hz to 2 kHz and bright sawtooths read within 1 cent at 44.1, 48 and 96 kHz, and silence and noise read as no pitch; the AI tools can read it too. | Channel strip, tempo-synced delay times |
| Instruments and MIDI | Instrument tracks playing MIDI clips through a built-in polyphonic synth with measured quality: in tune, sample-accurate, folded aliasing under -87 dB; drum tracks with a built-in synthesised drum machine on the General MIDI map (kick, snare, clap, side stick, closed/pedal/open hi-hat with choke, three toms, crash, ride, cowbell; tune, decay and tone controls); a tempo map (tempo changes at any bar and beat) and time signatures (3/4, 6/8, ... from any bar), with notes stored in beats so they follow both (a clip keeps musical time wherever it sits in the tempo map); the click, ruler, grid, counter, count-in and MIDI editor follow them too; a Tempo ruler shows every tempo and time-signature change, and adds (double-click), edits, ramps or deletes (right-click) them, one undo step each; tempo ramps (an accelerando or ritardando glides linearly to the next change, with exact closed-form timing; set_tempo's ramp); double-click a lane to create a MIDI clip; MIDI keyboards: every MIDI input is opened (also when plugged in later), a record-armed instrument track plays what you play (one block of latency, constant, no jitter) and records it into a new MIDI clip, placed where you played it (latency compensated); the sustain pedal (in clips, from the keyboard, recorded, sent to plug-in instruments as CC 64, shown in the MIDI editor); the pitch wheel (two semitones either way) and the mod wheel (vibrato up to half a semitone), in clips, from the keyboard, recorded, sent to plug-in instruments (pitch wheel, CC 1), and drawn in the MIDI editor's controller lane; MIDI editor (piano roll: add, move, resize, delete, multi-select, quantize, transpose, velocity); AI tools to write notes and shape the sound | Other controllers, MIDI input selection per track, note audition |
| Elastic Audio | Offline time-compress/expand and pitch shift with Rubber Band (R3 engine), Alt-drag TCE on clip edges | Real-time (preview) elastic mode, warp markers |
| AI remote | MCP server (46 tools; every argument checked against the published schema), JSON tools and text commands (PLAY, STOP, RECORD, TOGGLE_WINDOW, MUTE_TRACK_X, SET_VOLUME_TRACK_X_YdB, BOUNCE, STATUS, HELP) on 127.0.0.1; strict input validation (RFC 8259 JSON, UTF-8, nesting limit) | Presets applied by plain request (see the roadmap) |

## Building

### Requirements

- **Windows:** Visual Studio 2022 with *Desktop development with C++* (MSVC, Windows SDK), CMake 3.25+.
- **macOS:** Xcode 15+, CMake 3.25+.
- **Linux:** GCC 12+ or Clang 15+, CMake, Ninja, and JUCE's Linux dependencies.

JUCE, Rubber Band and Catch2 are downloaded automatically at configure time (pinned releases). No Steinberg SDK downloads are needed: JUCE 9 bundles the VST3 SDK and the ASIO SDK.

> **Windows path length:** keep the project in a short path (e.g. `Documents\Meridian`). Deep folders exceed Windows' 260-character limit inside the build tree.

### Commands

```bash
cmake --preset windows-msvc
cmake --build --preset windows-release
ctest --preset windows-debug
```

Without presets:

```bash
cmake -S . -B build/app -G "Visual Studio 17 2022" -A x64
cmake --build build/app --config Release --target Meridian
```

### Portable package

This builds a self-contained ZIP: `Meridian.exe` with the C++ runtime linked in, a quick-start guide, the documentation and third-party licences. It runs on any 64-bit Windows 10/11 PC with nothing to install.

```bash
cmake -S . -B build/dist -A x64 -DMERIDIAN_BUILD_TESTS=OFF
cmake --build build/dist --config Release
cpack --config build/dist/CPackConfig.cmake -C Release -B build/dist/package
```

The output is `build/dist/package/Meridian-<version>-windows-x64.zip`.

The core library and its tests build in seconds without JUCE:

```bash
cmake -S . -B build/core -DMERIDIAN_BUILD_APP=OFF
cmake --build build/core --config Debug
ctest --test-dir build/core -C Debug
```

### Options

| Option | Default | Meaning |
|---|---|---|
| `MERIDIAN_BUILD_APP` | ON | Off builds only the dependency-free core and its tests |
| `MERIDIAN_BUILD_TESTS` | ON | Catch2 unit tests |
| `MERIDIAN_WITH_RUBBERBAND` | ON | Elastic Audio via Rubber Band |
| `MERIDIAN_ENABLE_ASIO` | ON | ASIO driver type on Windows |
| `MERIDIAN_ENABLE_AVX2` | OFF | Compile with AVX2 (excludes pre-2013 CPUs) |
| `MERIDIAN_WARNINGS_AS_ERRORS` | OFF | Treat warnings in Meridian's own code as errors |
| `MERIDIAN_STATIC_RUNTIME` | ON | Windows: link the C++ runtime into the exe, so no Visual C++ Redistributable is needed |
| `MERIDIAN_ENABLE_ASAN` | OFF | AddressSanitizer for every target (plus UBSan on GCC/Clang). On MSVC, build RelWithDebInfo; this needs Visual Studio's "C++ AddressSanitizer" component |

## Architecture

```
src/
  core/            dependency-free C++20, unit-tested
    dsp/           mix kernels (SIMD, compensated summing), ramps, pan law, fader taper,
                   meter scale, saturation curve, DC blocker, automation curves
    remote/        WebSocket protocol (handshake, framing, UTF-8), SHA-1, Base64,
                   command parser, command dispatcher, JSON writer
  model/           session document (juce::ValueTree), templates
  engine/          AudioEngine, RenderGraph, ClipStream + DiskStreamer, MasterBusProcessor,
                   EngineBridge (session -> engine), Transport, lock-free queue
  plugins/         plug-in host, plug-in editor windows
  media/           import with sample-rate conversion, Elastic Audio rendering
  remote/          WebSocket server on JUCE sockets
  ui/              look and feel, transport bar, meters, Edit window, Mix window, dialogs
  app/             application, main window, menus and key commands, AppController
tests/             Catch2 tests for src/core
```

### Threads

| Thread | Does | Never does |
|---|---|---|
| Audio (device callback) | renders the current `RenderGraph` | lock, allocate, free, block, touch the session |
| Message (UI) | owns the session, builds graphs, runs remote commands | wait on the audio thread |
| Disk streamer | fills `ClipStream` buffers ahead of the playhead | — |
| Record writer | drains recording FIFOs to disk | — |
| Remote server + clients | socket I/O, WebSocket framing | touch application state |
| Media jobs | import conversion, Elastic Audio renders | touch the session |

**Graph hand-off.** The message thread builds an immutable `RenderGraph` snapshot and publishes it with one atomic exchange. At the start of a block the audio thread adopts the newest graph and returns the previous one through a wait-free queue, and a timer on the message thread deletes it. Plug-in instances, file readers and buffers are therefore always freed off the audio thread.

**Buses and sends.** Aux tracks are stereo buses. Each bus input is a compensated double-double sum, like the master bus. The render graph orders tracks so every bus is processed after everything feeding it (a topological sort), and the model refuses any route that would form a loop. Post-fader sends follow the track's fader and mute; pre-fader sends follow only mute and solo. Soloing a track keeps the buses it feeds audible ("implicit solo").

**Plug-in delay compensation.** Every graph rebuild computes the latency arriving at each summing point: each bus input and the master. It sums the latencies of the non-bypassed plug-ins along every path through tracks, sends and buses, then delays each path by exactly the difference to the slowest path into the same destination. The delay lines are kept across rebuilds, so editing during playback never drops audio. A plug-in that changes its latency, or a bypass, triggers recompensation. Bounces drop the total latency from their head, so files stay sample-aligned with the timeline. Recorded takes are moved earlier by the round-trip latency (device input + output + engine), so they line up with what the performer heard. Setup > Playback Engine can turn compensation off for troubleshooting.

**Mixer controls.** Volume, pan, mute, solo and arm are atomics in a per-track `TrackRealtimeState`. Every change, from the UI or a remote command, is ramped on the audio thread: 30 ms for faders, 5 ms for mute and solo, 20 ms for pan. Nothing ever clicks.

**Disk streaming.** Each audible clip segment owns two buffers (halves). One 64-bit atomic word per half encodes state and file position, so ownership passes between the audio and disk threads without locks (see `engine/ClipStream.h`).

### The summing bus

Each bus channel is a pair of double arrays `(sum, comp)`. Every addition goes through Knuth's TwoSum, which computes the rounding error of the add exactly and accumulates it in `comp`. The pair carries about 106 significand bits, the precision of a 128-bit accumulator, while every operation stays a vectorisable double operation. The result is independent of track order and accurate to the last bit of a double. Fast-math compiler flags would delete the error terms, so the core refuses to compile with them.

To be precise about the benefit: floating-point buses never clip internally, even at 32 bits. What the extra precision removes is rounding error that grows with track count and level differences.

### The console stage

`MasterBusProcessor`: 4x oversampling with linear-phase FIR half-band filters, then a unity-small-signal-gain wave-shaper (tanh or cubic soft-clip, with an asymmetry control for 2nd-order harmonics), then a DC blocker. The dry path is delayed by the oversampler's latency, so switching the console on and off is a phase-coherent 50 ms crossfade.

## AI remote control

While Meridian runs, port 47800 on 127.0.0.1 offers three interfaces to the same 46 tools:

| Interface | Address | For |
|---|---|---|
| **MCP** (Model Context Protocol, streamable HTTP) | `http://127.0.0.1:47800/mcp` | AI apps: Claude Code, Claude Desktop, any MCP client |
| WebSocket + JSON tools | `ws://127.0.0.1:47800/`, send `{"tool": "...", "arguments": {...}}` | scripts and custom agents |
| WebSocket + text commands | `ws://127.0.0.1:47800/`, send `PLAY`, `MUTE_TRACK_2`, ... | the simplest clients |

### Connecting an AI app (MCP)

Claude Code:

```bash
claude mcp add --transport http meridian http://127.0.0.1:47800/mcp
```

Claude Desktop and other stdio-only clients can use the `mcp-remote` bridge: add a server with command `npx` and arguments `mcp-remote http://127.0.0.1:47800/mcp` in the client's MCP settings.

Tools: `get_status`, `transport`, `set_loop`, `set_punch`, `set_click`, `add_marker`, `list_markers`, `delete_marker`, `set_track`, `create_track`, `delete_track`, `duplicate_track`, `move_track`, `set_output`, `set_send`, `remove_send`, `list_plugins`, `insert_plugin`, `remove_plugin`, `set_plugin_bypass`, `list_plugin_parameters`, `set_plugin_parameter`, `list_clips`, `split_clip`, `delete_clip`, `move_clip`, `set_clip`, `import_audio`, `set_volume_automation`, `set_automation`, `set_tempo`, `set_meter`, `add_midi_clip`, `add_notes`, `list_notes`, `set_synth`, `set_master`, `adjust`, `set_up`, `list_presets`, `bounce`, `undo`, `redo`, `save_session` and `show_window`. With the MIDI tools, an AI can compose: create an instrument track, write notes in beats, shape the sound, and bounce the result. With the plug-in parameter tools it can mix: read any plug-in's controls and set them in their own units (for example `{"parameter": "p2Gain", "value": 6}` for +6 dB on the EQ's second band). Each tool has a JSON schema and a description written for the model, and every call is checked against that schema before it runs. Tracks are referenced by number or name. Every AI edit lands in the undo history, labelled "AI: ...".

### Text commands

Send one command per text message; every reply is JSON.

| Command | Effect |
|---|---|
| `PLAY` / `STOP` | start or stop the transport |
| `RECORD` | record on all armed tracks |
| `TOGGLE_WINDOW` | switch between the Edit and Mix windows |
| `MUTE_TRACK_<X>` | toggle mute on track X (1 = first track); 5 ms ramp |
| `SET_VOLUME_TRACK_<X>_<Y>dB` | glide track X's fader to Y dB (`-INF`, or up to `+12`); 30 ms ramp |
| `BOUNCE` | mix the session down to `<session>/Bounced Files/`; progress and peak levels appear in `STATUS` |
| `STATUS` | transport state and every track, as JSON |
| `HELP` | the command list |

Commands are case-insensitive. Remote edits go through the undo history like any other edit, so you can undo what the AI did.

```text
> SET_VOLUME_TRACK_2_-6.5dB
< {"ok":true,"command":"SET_VOLUME_TRACK_2_-6.5dB","message":"Track 2 \"Bass\" ramping to -6.5 dB"}
> MUTE_TRACK_9
< {"ok":false,"command":"MUTE_TRACK_9","error":"There is no track 9; the session has 4."}
```

Quick test from PowerShell 7:

```powershell
$ws = [System.Net.WebSockets.ClientWebSocket]::new()
$ws.ConnectAsync([Uri]'ws://127.0.0.1:47800/', [Threading.CancellationToken]::None).Wait()
$msg = [Text.Encoding]::UTF8.GetBytes('STATUS')
$ws.SendAsync([ArraySegment[byte]]$msg, 'Text', $true, [Threading.CancellationToken]::None).Wait()
```

**Security.** The server binds to the loopback interface only. Requests carrying a browser `Origin` header are refused unless that origin is allowed (setting `remoteControlAllowedOrigins`), because any website could otherwise script a localhost socket. Set the environment variable `MERIDIAN_REMOTE_TOKEN` to require `?token=<value>` or `Authorization: Bearer <value>` from clients. `MERIDIAN_REMOTE_PORT` changes the port.

## Keyboard

| Key | Action |
|---|---|
| Space | play / stop |
| F12 or Ctrl+Space | record |
| Home / Return | return to zero |
| Ctrl+= | toggle Edit / Mix |
| Ctrl+E | separate clip at selection or cursor |
| Delete | delete selected clip or time range |
| Ctrl+Shift+T | Elastic Audio on the selected clip |
| Ctrl+[ / Ctrl+] | zoom out / in (also Ctrl+wheel) |
| F1 / F2 / F3 / F4 | Shuffle / Slip / Spot / Grid mode |
| F6 / F7 / F8 / F9 / F10 | Trim / Selector / Grabber / Smart Tool / Pencil (draw automation on the lane the track shows) |
| Ctrl+S / Ctrl+Shift+S | save / save as |
| Ctrl+Shift+L | loop playback (the selection, or the last loop range) |
| Ctrl+Shift+P | punch recording over the selection |
| Ctrl+Shift+K | click on/off (count-in: Transport menu) |
| Ctrl+M | add a marker at the playhead (Transport > Go to Marker; click a marker in the ruler to go there, right-click to rename or delete) |
| Ctrl+Shift+N | new tracks |
| Ctrl+Shift+I | import audio |
| Ctrl+Alt+B | bounce mix to a file |

**MIDI editor** (double-click a MIDI clip, or right-click it > Edit Notes):

| Action | Effect |
|---|---|
| click empty grid | add a note (snapped to the grid); drag while adding to set its length |
| drag a note | move it in time and pitch (with every selected note) |
| drag a note's right edge | change its length |
| shift-click | add a note to, or remove it from, the selection |
| double-click a note | delete it |
| Delete / Backspace | delete the selected notes |
| Ctrl+A | select all notes |
| wheel / Shift+wheel / Ctrl+wheel | scroll pitch / scroll time / zoom time |
| controller lane (bottom) | choose Bend or Mod at its left; click or drag to set the wheel from each grid position on (the pitch wheel snaps to its centre); Alt-click a point to delete it |
| Space | play / stop |

## Testing

Meridian is developed without human listening tests, so every quality claim must be measured by a machine. The checks are in layers, all automated, and all run on every push (see CI below).

**Unit tests** (`tests/`, Catch2): the mixing maths, ramps, pan law, saturation, automation, signal analysis, and the WebSocket and command protocol. No JUCE or audio hardware needed. They include **fuzz tests**: several hundred thousand random and mutated inputs, with fixed seeds so any failure reproduces, are fed through every parser that reads network input (WebSocket frames, HTTP handshakes, commands, JSON). Each must survive without a crash and keep its output contract.

```bash
ctest --test-dir build/core -C Debug
```

**End-to-end self test**, built into the app:

```bash
build\app\src\Meridian_artefacts\Debug\Meridian.exe --self-test build\self-test
```

It drives the real application and checks the results numerically. Each scenario is bounced offline and the audio measured against exact expected values:

- levels, pan law, mute, solo and summing
- fades and volume automation
- separate, delete and undo, including a seamless split point
- playlists and comping: a new playlist is silent, each take plays only on its own playlist, switching back is identical to the bit, and a comp plays the other take exactly within its range and the original exactly outside it (bit-identical on both sides, edges on the exact samples); undo, duplicate, rename, delete, save and reload; the track header's selector driven through its menu
- console harmonics, DC and aliasing
- list_presets (AI tool): the four console presets with their measured character (Warm Tape 0.665 % THD), eight templates and six recipes
- set_up (AI tool): a rap vocal on input 2 is armed, with the EQ high-pass on at 80 Hz, a 3:1 compressor, a de-esser and a -18 dB send to a fully wet Reverb bus, which a second recipe shares
- adjust (AI tool): warmth more raises H2 over H3 and THD, and the THD it reports is what the bounced mix measures; grit more moves the balance toward H3; clean more lowers THD; one undo step
- console presets, through a real bounce of a 1 kHz sine at -6 dBFS: Clean under 0.001 % THD, Warm Tape 0.3-1 % with H2 over H3, Vintage Console 0.1-0.5 % with H3 over H2, Gritty 3-8 % with H3 dominant; a later control change reads "Custom", and undo returns to the preset
- session save/load
- import with sample-rate conversion, including anti-aliasing
- Elastic Audio length and pitch
- VST3 hosting, using bundled test plug-ins (`tests/plugin`): effect, bypass, parameter, state recall and undo; and an instrument (pure sines from MIDI): a note sounds from exactly its sample to exactly its note-off, at the right pitch and level, with the built-in synth silent
- pitch wheel and mod wheel: the wheel at +1 raises A4 by exactly two semitones (493.88 Hz) from its point, the next clip plays at rest (440 Hz), the mod wheel gives vibrato of exactly half a semitone either way; save and reload, and repair of damaged wheel data
- tempo map and time signatures: bar positions across 4/4, 3/4 and 6/8; a note written on bar 5 sounds at exactly 7.500 s after a tempo change; the click through the real graph lands every beat on its exact sample with accents on bar starts; a 6/8 count-in clicks eighths; Grid mode snaps to the mapped beats; a MIDI clip placed after a tempo change keeps musical time (its note on bar 5 still sounds at 7.500 s); the MIDI editor draws and snaps to the session's bars (a 1-bar note in 3/4 is three beats); undo; damaged maps repaired
- loudness and export: the meter passes the EBU Tech 3341 cases 1-5 and 3342 loudness-range cases 1-4 (unit tests), reproduces the BS.1770 K-weighting coefficients, and reads true peak within 0.02 dB; a bounce's reported loudness matches the hand-computed gating to 0.006 LU and an independent measurement of the file; normalising lands on -14.000 LUFS, or stops at the -1 dBTP ceiling and says so; 16-bit TPDF dither gives exactly LSB/2 RMS error and keeps a -100 dBFS tone that rounding erases; noise-shaped dither gives exactly the shaping filter's total error power, at least 12 dB less noise from 2 to 5 kHz and more near Nyquist (unit tests), and keeps the -100 dBFS tone; 24-bit FLAC reads back within half a step; the live master meter reads exactly what an offline measurement reads
- automation, measured on bounces: pan lands hard left then hard right at exactly the pan law; mute is silent between its points; a send and a plug-in control step by exactly the drawn 12 dB; recorded passes in Write (one undo step, thinned, plays back the recorded level), Touch (only while held, then a glide back) and Latch (from the first touch to the stop); holding a control overrides its lane; damaged lanes are repaired
- MIDI keyboard on a bare engine: an armed instrument track plays a key at the next block, keys a quarter block apart sound exactly 128 samples apart (no jitter), unarmed tracks ignore the keyboard, recording captures each key at its exact timeline sample; in the whole application (with an audio device) recording three keys makes one MIDI clip with those notes, their lengths and spacing (one undo step)
- loop, click and count-in, sample-exact on a bare engine: clicks on every beat sample; a loop wrapping inside blocks (and a 100-sample loop wrapping 50 times in 5000 samples) puts every repeat on its exact sample; recording never loops; a count-in clicks on its own grid while the playhead waits; the pinned loop-start buffer serves the jump at once (and without it the same jump would miss); markers through the AI tools, undo, save and validation
- session safety: Save As in the same and another folder (the moved session must play identically); backups rotate to the newest 5 and never touch the session file; and **crash injection**: a second Meridian process opens the session, makes an edit it never saves, writes a backup and is killed; re-opening must offer that backup, and recovering it must restore the edit
- every bounce starts with plug-ins reset (no reverb tail, echoes or open gate from earlier playback): the same session bounces to bit-identical files
- built-in effects through the real plug-in path: an EQ band set by an AI tool to +6 dB must measure +6.00 dB, and must still measure it after save and re-open; reverb on a bus, fed by a send and excited with noise (ISO 3382 interrupted-noise method), must decay in the set RT60 with decorrelated left/right tails; the compressor must follow its static curve; the limiter must hold its ceiling, and its look-ahead latency must be compensated so a limited track still sums coherently with an unlimited copy
- AI commands, live playback with disk streaming, and the Edit and Mix windows driven by simulated mouse input
- automation lanes with the simulated mouse: the header's view selector switches the lane; Pencil clicks write hard left and hard right on the pan lane (and the drawn pan plays: the right side silent), exact 0/1 steps on the mute lane, -6 dB on a send lane at the fader scale's -6 dB, and a plug-in control's full range ("A: Band 2 Gain"); removing the send or the plug-in returns the view to volume
- tempo ramps: 120 gliding to 240 BPM over a bar puts the next bar at exactly 2 ln 2 s, and a note there sounds on that sample (unit tests: exact closed forms, round trips both ways)
- session templates: a Hip-Hop session has its six tracks of the right kinds, 90 BPM, Warm Tape, the EQ/Compressor/De-Esser chain on the vocal and a send at -18 dB to a fully wet reverb bus; a Film session routes dialogue, music and effects to the DX, MX and FX buses
- the Tempo ruler: a change added at bar 5 sets 90.5 bpm and 3/4 from exactly its first tick, one undo removes both, Delete removes them, and bar 1 sets the starting tempo
- Mix window sections: hiding Inserts F-J and Sends A-E from the Window menu gives the fader exactly the 196 px they free; showing them restores it; Simple Mode: a bare strip needs 305 px, an insert in use brings its inserts back (98 px), and Pro mode shows everything (680 px)
- Shuffle and Spot modes with the simulated mouse: deleting a clip or a range closes the gap, trimming either edge moves every later clip and the automation by exactly the trim, a dragged clip reorders at the nearest boundary, and each is one undo step; Spot opens its dialog instead of moving, places a clip at an exact typed time or sample, and refuses text that is not a time
- **stress:** 128 tracks and 8 buses must still sum to the exact expected level, and must render faster than real time (checked in Release builds)
- **robustness:**
  - damaged session files (random bytes, truncated, from a future version, and a hand-crafted file breaking every rule) are refused or repaired, never crash the app, and still render;
  - 600 randomly damaged session variants must each end up refused or repaired, and repaired ones must pass an independent validator;
  - 3000 random AI tool calls and 2000 mutated MCP requests must never crash the app or leave the session invalid
- **internal consistency:** the run fails if any JUCE assertion fires anywhere in the program, including at start-up (`JUCE_LOG_ASSERTIONS`)

It writes the following to the output folder:
- `report.txt` and `report.json`
- `progress.log`, written line by line, so a crash shows where it happened
- every bounce
- PNG screenshots of every window and dialog

It exits with 0 only if every check passed. Checks that cannot run on a machine (for example live playback on a server without a sound card, with `--allow-no-audio-device`) are reported as SKIP, never as PASS.

**Reference renders (golden files).** Six fixed mixes are bounced and null-tested against the references in `tests/golden`. Together they exercise clip gain, fades, pan, automation, a pre-fader send, an aux bus, both console curves, the synth, the drum machine, and the EQ and reverb. A render that differs from its reference by more than -120 dBFS fails. For scale, a 0.001 dB level change leaves a -84 dBFS residual, and the test checks that it detects one. So no change can alter the sound unnoticed. After a deliberate change to the sound, regenerate the references, listen to or measure the difference, and commit the new files with an explanation:

```bash
build\app\src\Meridian_artefacts\Debug\Meridian.exe --self-test build\self-test --update-golden
```

**Soak test.** This mode checks long-running stability:

```bash
build\app\src\Meridian_artefacts\Release\Meridian.exe --soak 20 build\soak
```

It loops a demanding session through the real audio device for the given number of minutes. The session has:
- 24 tracks with moving automation
- sends into 4 buses, two of them with a latent plug-in
- the console stage on

The master fader stays fully down, so nothing is audible. Every 10 seconds it records:
- disk underruns
- driver-reported dropouts
- CPU load
- process memory
- the engine's worst callback time, as a share of the buffer period

The run fails if Meridian ever overruns its real-time budget, if the playhead stalls, if streaming falls behind, or if memory grows over the second half of the run. Dropouts reported by the driver while Meridian stayed inside its budget are reported as warnings: the operating system or driver caused them, not Meridian. The output is `soak.csv` and `soak-report.txt`.

**Continuous integration** (`.github/workflows/ci.yml`) runs on every push to `main` and on every pull request:

| Job | What it runs |
|---|---|
| Core | Unit tests on Linux and macOS |
| Windows | Full application: unit tests, then the self test |
| Linux | Full application: unit tests, then the self test under a virtual display |
| Sanitizers | AddressSanitizer and UndefinedBehaviorSanitizer build on Linux, running the unit tests and the whole self test. Any memory error, leak or undefined behaviour fails the run |
| macOS | Full application; started by hand |

Every job treats warnings as errors. Each run uploads the self-test report, progress log and screenshots.

## Licensing

Meridian's own code is intended for release under the **GNU AGPLv3**, as required for open-source use of JUCE 9. Its components:

- **JUCE 9:** AGPLv3 or commercial licence. The bundled ASIO SDK is GPLv3 and the VST3 SDK MIT.
- **Rubber Band Library:** GPLv2-or-later (compatible with AGPLv3) or commercial.
- **Catch2:** BSL-1.0 (tests only).

Shipping Meridian as a closed-source product would require commercial licences for JUCE and Rubber Band.

The user interface follows the conventions of console-style DAWs (Edit and Mix windows, coloured mute, solo and record keys) but uses no third-party trademarks, logos or artwork.
