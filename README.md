# Meridian

An open-source digital audio workstation for Windows and macOS, written in C++20 on JUCE 9. It is laid out like a classic console-style DAW (Edit and Mix windows), and adds two things of its own:

- a 64-bit double-precision mix engine with compensated (double-double) summing and an oversampled analog-console stage on the master bus, and
- a local AI remote-control port: a WebSocket server that lets an AI agent or script drive the session with plain text commands.

"Meridian" is a working title, set in one place: `MERIDIAN_PRODUCT_NAME` in `CMakeLists.txt`.

## Status

This is an early foundation, not a finished DAW. Everything listed as working compiles and runs; the core maths and the remote protocol are covered by unit tests.

| Area | Working now | Planned |
|---|---|---|
| Sessions | Dashboard (create from template or blank, recent, open), save/load, full undo/redo | Save As, session import |
| Edit window | Tracks, clips with waveforms, Smart Tool (select/move/trim/fade zones), Slip and Grid modes, separate, delete, zoom, page-scroll, drag-and-drop import | Shuffle and Spot modes, Zoomer and Pencil tools, automation lane drawing, playlists |
| Mix window | Channel strips: 10 inserts, I/O, automation mode, pan, R/S/M, console-taper fader, segmented meters; master strip with console controls | Sends, aux/bus tracks, routing matrix, VCAs |
| Audio engine | Lock-free render graph, double-buffered disk streaming, record to disk, input monitoring, sample-accurate volume automation, ramped gain/mute/pan | Plug-in delay compensation, multi-core track rendering, bounce/export |
| Mixing maths | 64-bit compensated summing bus, SIMD (SSE2/AVX/NEON) mix kernels, constant-power pan, 4x-oversampled tanh/cubic console saturation | Per-track console channels |
| Plug-ins | VST3 and LV2 hosting (plus AU on macOS), scanning with crash protection, editors, bypass, state saved in the session | CLAP hosting (needs a CLAP-SDK format adapter), sidechains, instruments |
| Elastic Audio | Offline time-compress/expand and pitch shift with Rubber Band (R3 engine), Alt-drag TCE on clip edges | Real-time (preview) elastic mode, warp markers |
| AI remote | WebSocket on 127.0.0.1: PLAY, STOP, RECORD, TOGGLE_WINDOW, MUTE_TRACK_X, SET_VOLUME_TRACK_X_YdB, STATUS, HELP | A Model Context Protocol (MCP) server bridging to this port |

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

**Mixer controls.** Volume, pan, mute, solo and arm are atomics in a per-track `TrackRealtimeState`. Every change, from the UI or a remote command, is ramped on the audio thread: 30 ms for faders, 5 ms for mute and solo, 20 ms for pan. Nothing ever clicks.

**Disk streaming.** Each audible clip segment owns two buffers (halves). One 64-bit atomic word per half encodes state and file position, so ownership passes between the audio and disk threads without locks (see `engine/ClipStream.h`).

### The summing bus

Each bus channel is a pair of double arrays `(sum, comp)`. Every addition goes through Knuth's TwoSum, which computes the rounding error of the add exactly and accumulates it in `comp`. The pair carries about 106 significand bits, the precision of a 128-bit accumulator, while every operation stays a vectorisable double operation. The result is independent of track order and accurate to the last bit of a double. Fast-math compiler flags would delete the error terms, so the core refuses to compile with them.

To be precise about the benefit: floating-point buses never clip internally, even at 32 bits. What the extra precision removes is rounding error that grows with track count and level differences.

### The console stage

`MasterBusProcessor`: 4x oversampling with linear-phase FIR half-band filters, then a unity-small-signal-gain wave-shaper (tanh or cubic soft-clip, with an asymmetry control for 2nd-order harmonics), then a DC blocker. The dry path is delayed by the oversampler's latency, so switching the console on and off is a phase-coherent 50 ms crossfade.

## AI remote control

When the app runs, it listens on `ws://127.0.0.1:47800/`. Send one command per text message; every reply is JSON.

| Command | Effect |
|---|---|
| `PLAY` / `STOP` | start or stop the transport |
| `RECORD` | record on all armed tracks |
| `TOGGLE_WINDOW` | switch between the Edit and Mix windows |
| `MUTE_TRACK_<X>` | toggle mute on track X (1 = first track); 5 ms ramp |
| `SET_VOLUME_TRACK_<X>_<Y>dB` | glide track X's fader to Y dB (`-INF`, or up to `+12`); 30 ms ramp |
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

## Licensing

Meridian's own code is intended for release under the **GNU AGPLv3**, as required for open-source use of JUCE 9. Its components:

- **JUCE 9:** AGPLv3 or commercial licence. The bundled ASIO SDK is GPLv3 and the VST3 SDK MIT.
- **Rubber Band Library:** GPLv2-or-later (compatible with AGPLv3) or commercial.
- **Catch2:** BSL-1.0 (tests only).

Shipping Meridian as a closed-source product would require commercial licences for JUCE and Rubber Band.

The user interface follows the conventions of console-style DAWs (Edit and Mix windows, coloured mute, solo and record keys) but uses no third-party trademarks, logos or artwork.
