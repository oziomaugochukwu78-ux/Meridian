# Changelog

## 0.1.0 (first public release)

The first release: a working DAW for Windows (64-bit Windows 10 or 11) that Claude and other AI apps can drive.

**Recording and editing**
- Audio and MIDI recording with punch-in, pre/post-roll, count-in and click; record-to-disk with latency compensation
- Playlists (alternate takes) and comping
- Smart Tool; Shuffle, Slip, Spot and Grid modes; cut, copy, paste, duplicate, separate, nudge
- Fades and crossfades (equal power or equal gain), batch fades, Strip Silence
- Mix and edit groups
- Tempo map with ramps and time signatures; markers; a timecode ruler
- MIDI editor (piano roll) with quantize, transpose, velocity, sustain, pitch and mod wheel
- Offline time stretch and pitch shift (Rubber Band)

**Mixing**
- 64-bit compensated summing, an oversampled analog-console stage on the master with presets
- 10 inserts and 5 sends per track, aux buses, automatic delay compensation
- Automation of volume, pan, mute, sends and any plug-in control; Off/Read/Touch/Latch/Write/Trim modes
- Built-in effects: EQ, Compressor, Limiter (true peak), Gate, De-Esser, Delay, Reverb, Lo-Fi, Tuner
- VST3 and LV2 plug-in hosting
- Loudness metering (EBU R128)

**Instruments**
- Built-in synth, a drum machine on the General MIDI map, and a sampler for recorded sounds
- MIDI keyboards, played and recorded

**Export**
- Bounce to WAV, AIFF, FLAC and MP3, with loudness reported and optional normalisation under a true-peak ceiling; dither for 16-bit

**AI remote control**
- An MCP server with 49 tools on 127.0.0.1 (this computer only); every AI edit is one undo step
- `Connect to Claude.cmd` adds Meridian to Claude Desktop in one double-click

**Sessions**
- A Dashboard with genre templates, automatic backups and crash recovery

**Not yet** (see [ROADMAP.md](ROADMAP.md)): folder tracks, track freeze, track presets, retrospective MIDI record, AAC export, a playlist lane view, Beat Detective, real-time Elastic Audio. Linux and macOS build in CI but are not yet packaged for download. The Windows build is not code-signed, so Windows shows a "Windows protected your PC" prompt the first time (More info > Run anyway).
