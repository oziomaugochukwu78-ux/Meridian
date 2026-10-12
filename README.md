# Meridian

An open-source digital audio workstation for Windows and macOS, written in C++20 on JUCE 9. It is laid out like a classic console-style DAW (Edit and Mix windows), and adds two things of its own:

- a 64-bit double-precision mix engine with compensated (double-double) summing and an oversampled analog-console stage on the master bus, and
- a local AI remote-control port: a WebSocket server that lets an AI agent or script drive the session with plain text commands.

"Meridian" is a working title, set in one place: `MERIDIAN_PRODUCT_NAME` in `CMakeLists.txt`.

## Status

This is an early foundation, not a finished DAW. Everything listed as working compiles, runs and is checked by the automated tests described under [Testing](#testing). No feature depends on anyone listening to it. See [ROADMAP.md](ROADMAP.md) for what comes next.

| Area | Working now | Planned |
|---|---|---|
| Sessions | Dashboard (create from template or blank, recent, open), save/load (atomic), full undo/redo, automatic repair of damaged session files on open | Save As, auto-backups, session import |
| Edit window | Tracks, clips with waveforms, Smart Tool (select/move/trim/fade zones), Slip and Grid modes, separate, delete, zoom, page-scroll, drag-and-drop import, volume automation drawing (Pencil tool) | Shuffle and Spot modes, Zoomer tool, playlists, more automation parameters |
| Mix window | Channel strips: 10 inserts, 5 sends (level, pan, pre/post, mute), output routing to master or any bus, I/O, automation mode, pan, R/S/M, console-taper fader, segmented meters; Aux Input (bus) tracks, bus-to-bus chains with loop protection, implicit solo; master strip with console controls | VCAs, mono buses, surround |
| Audio engine | Lock-free render graph, double-buffered disk streaming, record to disk with round-trip latency compensation, input monitoring, sample-accurate volume automation, ramped gain/mute/pan, automatic plug-in delay compensation across tracks, sends and buses, offline bounce to WAV/AIFF (File > Bounce Mix) | Multi-core track rendering, dither options, manual recording offset |
| Mixing maths | 64-bit compensated summing bus, SIMD (SSE2/AVX/NEON) mix kernels, constant-power pan, 4x-oversampled tanh/cubic console saturation | Per-track console channels |
| Plug-ins | VST3 and LV2 hosting (plus AU on macOS), scanning with crash protection, editors, bypass, state saved in the session | CLAP hosting (needs a CLAP-SDK format adapter), sidechains, instruments |
| Elastic Audio | Offline time-compress/expand and pitch shift with Rubber Band (R3 engine), Alt-drag TCE on clip edges | Real-time (preview) elastic mode, warp markers |
| AI remote | MCP server (25 tools; every argument checked against the published schema), JSON tools and text commands (PLAY, STOP, RECORD, TOGGLE_WINDOW, MUTE_TRACK_X, SET_VOLUME_TRACK_X_YdB, BOUNCE, STATUS, HELP) on 127.0.0.1; strict input validation (RFC 8259 JSON, UTF-8, nesting limit) | Presets applied by plain request (see the roadmap) |

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

While Meridian runs, port 47800 on 127.0.0.1 offers three interfaces to the same 24 tools:

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

Tools: `get_status`, `transport`, `set_track`, `create_track`, `delete_track`, `set_output`, `set_send`, `remove_send`, `list_plugins`, `insert_plugin`, `remove_plugin`, `set_plugin_bypass`, `list_clips`, `split_clip`, `delete_clip`, `move_clip`, `set_clip`, `import_audio`, `set_master`, `bounce`, `undo`, `redo`, `save_session` and `show_window`. Each tool has a JSON schema and a description written for the model. Tracks are referenced by number or name. Every AI edit lands in the undo history, labelled "AI: ...".

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
| F2 / F4 | Slip / Grid mode |
| F6 / F7 / F8 / F9 | Trim / Selector / Grabber / Smart Tool |
| Ctrl+Shift+N | new tracks |
| Ctrl+Shift+I | import audio |
| Ctrl+Alt+B | bounce mix to a file |

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
- console harmonics, DC and aliasing
- session save/load
- import with sample-rate conversion, including anti-aliasing
- Elastic Audio length and pitch
- VST3 hosting, using a bundled test plug-in (`tests/plugin`): effect, bypass, parameter, state recall and undo
- AI commands, live playback with disk streaming, and the Edit and Mix windows driven by simulated mouse input
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

**Reference renders (golden files).** Three fixed mixes are bounced and null-tested against the references in `tests/golden`. Together they exercise clip gain, fades, pan, automation, a pre-fader send, an aux bus, and both console curves. A render that differs from its reference by more than -120 dBFS fails. For scale, a 0.001 dB level change leaves a -84 dBFS residual, and the test checks that it detects one. So no change can alter the sound unnoticed. After a deliberate change to the sound, regenerate the references, listen to or measure the difference, and commit the new files with an explanation:

```bash
build\app\src\Meridian_artefacts\Debug\Meridian.exe --self-test build\self-test --update-golden
```

**Continuous integration** (`.github/workflows/ci.yml`) runs on every push to `main` and on every pull request:

| Job | What it runs |
|---|---|
| Core | Unit tests on Linux and macOS |
| Windows | Full application: unit tests, then the self test |
| Linux | Full application: unit tests, then the self test under a virtual display |
| macOS | Full application; started by hand |

Every job treats warnings as errors. Each run uploads the self-test report, progress log and screenshots.

## Licensing

Meridian's own code is intended for release under the **GNU AGPLv3**, as required for open-source use of JUCE 9. Its components:

- **JUCE 9:** AGPLv3 or commercial licence. The bundled ASIO SDK is GPLv3 and the VST3 SDK MIT.
- **Rubber Band Library:** GPLv2-or-later (compatible with AGPLv3) or commercial.
- **Catch2:** BSL-1.0 (tests only).

Shipping Meridian as a closed-source product would require commercial licences for JUCE and Rubber Band.

The user interface follows the conventions of console-style DAWs (Edit and Mix windows, coloured mute, solo and record keys) but uses no third-party trademarks, logos or artwork.
