# Meridian roadmap

This plan runs from today's foundation to the scope of a professional post-production DAW. The tiers below borrow the familiar Artist, Studio and Ultimate grouping only as a yardstick for scope. Meridian's own product has no tiers and is open source.

## Ground rules

1. **No feature is "done" until a machine has measured it.** Nobody listens to Meridian before release, so every claim needs a numeric acceptance test: null tests, level accuracy, THD+N, frequency response, latency, timing. Each feature lands with its tests in the self test or the unit tests, and CI runs them on every push.
2. **The sound only changes on purpose.** The golden-file null tests (`tests/golden`) fail on any change to the audio larger than -120 dBFS. An intentional change updates the references in the same commit and says why.
3. **Nothing from outside can crash it.** Session files, imported media, plug-in state and remote commands are all untrusted. Every new parser or input path gets a fuzz test and a repair or refuse rule, as session loading and the remote port already have.
4. **Beginner-friendly by default, everything available underneath.** Defaults follow established DAW conventions and need no settings changes. Depth is never removed, only tucked away (see the Simple/Pro modes below).
5. **The real-time thread stays real-time.** On the audio thread there are no locks, allocations or I/O. This is enforced by review today and by tooling from milestone 1.

## Where Meridian is today (milestone 0, done)

- Edit and Mix windows, Dashboard, Playback Engine and other dialogs
- Lock-free engine, disk streaming, recording with latency compensation
- Buses and sends, plug-in delay compensation, VST3/LV2 hosting
- Volume automation, Elastic Audio (Rubber Band), offline bounce
- 64-bit compensated summing, oversampled console saturation
- AI control over MCP, WebSocket JSON and text commands

Verification at this stage:
- 65 unit tests, including fuzzing
- 184 end-to-end self-test checks, including golden null tests, a 128-track stress test, damaged-file and hostile-input robustness, and zero tolerance for internal assertions
- CI on Windows, Linux and macOS

## Milestone 1: verification backbone (in progress)

Done:
- golden-file null tests
- fuzzing
- session repair
- stress and robustness tests
- assertion capture
- CI on three platforms
- an AddressSanitizer + UBSan job in CI
- the soak test (`--soak <minutes>`), with the engine's own real-time load measurement
- a self-contained portable Windows package

To finish before the feature work below:

| Item | Acceptance |
|---|---|
| Soak test, extended: 2 hours, then 8 hours, of playback with automation and plug-ins. A 20-minute run passed every check, but memory crept by about 0.05–0.15 MB per minute, with one 2.6 MB step; a longer run must show whether that creep is a leak or allocator behaviour | 0 budget overruns; memory flat over the second half; dropouts attributable to Meridian: none |
| Real-time safety tooling: Clang RealtimeSanitizer (`-fsanitize=realtime`) in CI, on top of the existing ASan/UBSan job | No reports across the whole self test |
| Comparison with another DAW: the same session rendered in Ardour (open source) and null-tested against Meridian | Residual below -90 dBFS for gain, pan law and summing scenarios (pan laws are matched explicitly) |
| Plug-in compatibility suite: load, process, save and restore state for a list of popular free VST3 plug-ins, each in a crash-protected child process | No crashes; restored state renders identically |
| Crash reporter: minidump plus the last session actions, saved locally; sending them anywhere needs the user's consent | Reports appear for deliberately injected crashes |
| ~~Session safety: Save As, periodic backups in `Session File Backups/` (newest 5 kept), recovery prompt after a crash~~ Done | Crash-injection test recovers the last backup: passes (a real second process is killed mid-session) |

## Milestone 2: Artist-level

What a home producer, songwriter or podcaster needs.

**Editing and workflow**
- clip gain line and handles
- done: track reorder and duplicate
- done: markers, loop playback, punch-in/punch-out recording
- playlists with comping
- bars|beats grid and ruler
- done: tempo and meter map (tempo changes, time signatures); still to do: tempo ramps, editing changes in the ruler
- done: click with a count-in

**MIDI and instruments**
- done: MIDI input recording (keyboard, live play-through, latency compensated); VST3 instrument hosting; still to do: sustain pedal
- done: a piano-roll editor, quantise, and velocity editing
- sample-accurate MIDI timing

Acceptance: MIDI timing jitter of zero samples in offline renders, and at most one buffer live.

**Automation**
- done: pan, mute, send level and plug-in parameters; write modes (Write, Touch, Latch)
- still to do: trim mode, drawing the new lanes in the Edit window, plug-in control touch from plug-in windows

**Built-in processing**
- done: Meridian EQ, Compressor, Limiter (true peak), Gate, De-Esser, Delay, Reverb
- still to do: tuner, gain-reduction meters
- each ships with a measured specification, checked in tests: frequency response within ±0.1 dB of design, compressor static curve, limiter true-peak ceiling

**Loudness and delivery**
- done: LUFS, loudness range and true-peak meters (ITU-R BS.1770-4 / EBU R128), checked against the EBU Tech 3341/3342 signals; TPDF dither; FLAC export; normalisation to a loudness target under a true-peak ceiling
- still to do: noise-shaped dither; MP3 export (needs an encoder library, e.g. LAME)

**Presets, layers 1 and 2** (see the design below)
- smart defaults
- factory presets
- genre session templates
- the Simple and Pro modes

## Milestone 3: Studio-level

What a professional mixing and tracking room needs.

- **Scale:**
  - track rendering spread across all cores, with no change to the output (the golden tests prove it)
  - 512+ tracks
  - track freeze and commit
- **Mixing:**
  - VCAs and mix groups
  - sidechain inputs
  - mono and LCR buses
  - clip effects
  - pre/post-fader inserts
- **Advanced editing:**
  - real-time Elastic Audio with warp markers
  - groove quantise and transient detection (beat slicing)
  - Shuffle and Spot edit modes
- **Plug-in formats:** CLAP hosting through a format adapter; ARA2 for pitch and timing editors.
- **Control surfaces:** MIDI learn; Mackie Control / HUI protocol support.
- **Session interchange:** importing tracks, routing and plug-in settings from other Meridian sessions.
- **Presets, layer 3:** everything below, including user-shareable presets and custom shortcuts.

## Milestone 4: Ultimate-level

What post-production, film and immersive work needs.

- **Video:**
  - a video track decoded with FFmpeg: frame-accurate scrubbing, timecode ruler, video offset
  - audio bounce to a video container
  - linked as LGPL shared libraries, so the licences stay compatible
  - acceptance: A/V offset measured with flash/beep test clips, within ±1 ms
- **Timecode sync:** MTC and LTC in and out, with reference-clock awareness.
- **Surround and immersive:**
  - 5.1, 7.1 and 7.1.4 buses, an object panner, and fold-down monitoring
  - ADM BWF export (ITU-R BS.2076), so mixes open in immersive-audio renderers
  - acceptance: panner gains checked against the VBAP/ITU layout equations, and fold-down coefficients checked against ITU-R BS.775
  - Dolby Atmos renderer integration and certification need a licence from Dolby, so they are listed as a dependency, not promised
- **AAF/OMF interchange:**
  - import and export of picture-editor timelines (clips, fades, clip gain, basic automation)
  - built on an open-source AAF library, chosen after a licence review
  - acceptance: round-trip tests against sample AAFs from common picture editors
- **Large-session reliability:** very high voice counts, with field-recorder workflows (multichannel takes and metadata).

---

## Design: presets and customization

Three layers, one data model, and one AI command set.

### Layer 1: smart defaults (nothing to set up)

- **New sessions:**
  - 48 kHz / 24-bit WAV
  - the console stage on the "Warm Tape" preset at its gentlest setting, which leaves a clean mix effectively untouched (unity small-signal gain is already guaranteed by the saturation design)
  - a -0.3 dBTP limiter in the bounce path, off until loudness delivery exists
- **New tracks:**
  - named and coloured by type
  - input chosen from the first free device input
  - monitoring and buffer size picked for the job: 128 samples while recording, 1024 while mixing, switched automatically by "Low latency recording"
- Every default is a named value in one place (`DefaultSettings`), so a test can check each one.

### Layer 2: presets (one click, or one sentence to the AI)

| Kind | Factory content | Applies to |
|---|---|---|
| `console` | Clean, Warm Tape, Vintage Console, Gritty | master console stage |
| `template` | Hip-Hop, Rock Band, Singer-Songwriter, Electronic, Podcast (2 hosts + guest), Film Dialogue/Music/Effects | a new session: tracks, buses, sends, inserts, colours |
| `recipe` | Rap Vocal, Acoustic Guitar, Drum Bus, Podcast Voice, Voice-over | adds a configured track, or changes one |
| `layout` | Simple, Pro | which controls the UI shows |
| `theme` | Classic Dark Grey (default), Light, High Contrast | colour tokens in `Theme.h` |

**Console presets are defined by measurable targets, not adjectives.** Each one sets drive, asymmetry (character) and curve, and is tested against a measured target band. The test uses a 1 kHz sine at -6 dBFS through the master with the console on:

| Preset | Curve | Target THD | Harmonic balance | Perceptual intent |
|---|---|---|---|---|
| Clean | (stage bypassed) | < 0.001 % | — | transparent |
| Warm Tape | tanh | 0.3–1 % | H2 ≥ H3 | even-order "warmth", soft peak rounding |
| Vintage Console | cubic | 0.1–0.5 % | H3 ≥ H2 | gentle odd-order "density" |
| Gritty | tanh | 3–8 % | H3 dominant | audible drive |

The parameter values are tuned until each preset falls in its band, and the self test keeps them there. Level matching is built into the curve, so comparing presets is never fooled by "louder sounds better". This rests on established psychoacoustics:
- low-order even harmonics are heard as warmth;
- odd harmonics as edge or grit;
- level differences as small as 0.2 dB bias listeners' preferences.

**Simple and Pro modes.** Simple hides the following without disabling any of them:
- the insert and send lists, until something is in them
- I/O selectors
- automation mode
- the Smart Tool zone hints

The Dashboard also leads with genre templates. Pro is the full console view. The mode is a `layout` preset, so a user can define their own in between.

### Layer 3: full customization

- Every preset parameter is editable in the UI. "Save as preset…" writes a user preset, and "Export…" writes a single file to share.
- Custom keyboard shortcuts use a `keymap` preset kind. The default keymap follows long-standing DAW conventions (Space play/stop, F12 record, Ctrl+E separate, and so on), and the editor flags conflicts.
- Factory presets are read-only. Editing one creates a user copy.

### Data model: plain, versioned, shareable files

Each preset is one UTF-8 JSON file with the extension `.meridianpreset`. JSON was chosen over XML for three reasons: it is readable in any text editor, easy to paste into a chat or a gist, and it is what AI tools produce natively.

```json
{
  "format": "meridian-preset",
  "version": 1,
  "kind": "console",
  "id": "factory.console.warm-tape",
  "name": "Warm Tape",
  "description": "Soft, even-order saturation that rounds peaks gently.",
  "tags": ["warm", "tape", "master"],
  "author": "Meridian",
  "settings": { "curve": "tanh", "driveDb": 6.0, "character": 0.35 },
  "measured": { "thdPercentAtMinus6dBFS": 0.6, "h2OverH3Db": 4.1 }
}
```

- **`version` and migration.** Loading runs a chain of migrators (v1→v2→…), each a pure function with its own unit tests. Fields a newer version added are kept, not dropped, so old builds never destroy data they do not understand.
- **Validation.** Every kind has a schema, the same idea as the remote tools' argument schemas. Files are validated and repaired on load, exactly as sessions are (`SessionRepair`). Out-of-range values are clamped and reported, and unknown kinds are refused. Preset loading is fuzz-tested like session loading.
- **Where files live:**
  - factory presets: bundled with the application, read-only
  - user presets: `Documents/Meridian/Presets/<kind>/`
  - presets in use: copied into the session, so a session opened on another machine sounds the same without the preset files
- **Applying a preset** is one undo step, named "Preset: Warm Tape". The UI marks a preset as modified once any of its values is changed.
- **`measured`** is written by the measurement harness, not by hand, so a shared preset carries proof of what it does.

### AI command set

These extend the existing MCP tools. Every call is validated against its schema and recorded as an "AI: …" undo step, and every reply says in plain words what changed.

| Tool | Purpose |
|---|---|
| `list_presets {kind?, query?}` | find presets by kind, name or tag |
| `describe_preset {id}` | the settings plus measured character, so the AI can explain choices |
| `apply_preset {id, track?}` | apply a console, recipe, layout or theme preset |
| `create_session_from_template {template, name}` | start a session from a genre template |
| `set_up {goal}` | goal recipes, e.g. "record a rap vocal": a mono vocal track on input 1, record-armed, low-latency monitoring, high-pass 80 Hz, a 3:1 compressor, a send to a reverb bus at -18 dB |
| `adjust {target, attribute, direction, amount?}` | relative, perceptual changes: "make the master warmer" means `{target: "master", attribute: "warmth", direction: "more"}` |
| `save_preset {kind, name, from}` / `export_preset {id}` | capture the current state as a preset, or produce the shareable file |

`adjust` maps each perceptual attribute to parameters through a fixed, tested table:
- **warmth:** asymmetry ↑ and drive ↑ on a tanh curve, H2 rises
- **grit:** drive ↑, H3 rises
- **cleaner:** drive ↓
- **brighter/darker:** EQ tilt, from milestone 2

Each step is sized to a small but measurable change, such as +0.5 dB H2/H1, and capped at the extremes of the factory presets. The reply reports the measured before and after. Goal requests the table does not cover are answered by the AI combining `describe_preset` and `apply_preset`, never by guessing at raw parameters.

### Delivery plan

| Milestone | Preset work |
|---|---|
| 2 | Data model, validator and migrator; `console`, `template`, `layout` and `theme` kinds; factory console presets tuned to their measured bands; `list_presets`, `describe_preset`, `apply_preset` and `create_session_from_template` |
| 2 | `recipe` kind (needs the built-in EQ and compressor), `set_up`, `adjust` |
| 3 | User presets, export/import, a preset browser, the `keymap` kind and shortcut editor, `save_preset`/`export_preset` |

## Risks and constraints

- **Licensing.** JUCE 9 is used under the AGPLv3, so Meridian must stay open source. A closed-source or commercial edition would need a commercial JUCE licence. Licence reviews are also needed for FFmpeg (LGPL build only), the AAF library, and the Rubber Band licence for any closed edition. Dolby Atmos integration needs a Dolby licence.
- **Hardware.** Building and testing the macOS version needs a Mac, as CI does. Live-device checks (latency, dropouts) need real audio interfaces. CI servers have no sound card, so those checks are skipped there and run on developer machines.
- **Machine-only verification has limits.** Tests prove correctness, stability and measurable character. They cannot prove that a workflow is pleasant. To limit that risk, Meridian follows long-established DAW conventions instead of inventing new interaction patterns, and keeps the defaults conservative.
