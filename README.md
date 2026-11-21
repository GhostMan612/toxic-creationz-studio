# Toxic Creationz Studio — AI DAW

A native Windows desktop DAW built with **C++20, JUCE 9, and Tracktion Engine**, featuring VST3 plugin hosting, an FL Studio-style piano roll, Reason-style modular audio/MIDI/CV racks, time-stretched riff BGM layering, Audacity-style non-destructive waveform DSP, master WAV mixdown export, live stereo channel metering with 3-band EQ, and an out-of-process **Python FastMCP AI Co-Producer** over a local JSON-RPC 2.0 TCP bridge (`127.0.0.1:9450`).

```text
+-------------------------------------------------------------------+
| Toxic Creationz Studio (C++20 / JUCE 9 / Tracktion Engine)        |
|  Timeline + Piano Roll + Waveform Editor + 3-Band EQ Mixer/Meters |
|  Modular CV/Audio Rack (LFO, Wobble, 1-to-4 Splitter) + BGM Solver|
|  Master Mixdown Renderer (te::Renderer) + Virtual MIDI Step Rec   |
+---------------------------------+---------------------------------+
                                  |
            127.0.0.1:9450 (newline-delimited JSON-RPC 2.0)
                                  |
+---------------------------------v---------------------------------+
| Out-of-Process Python Worker (mcp/daw_mcp_server.py)              |
|  FastMCP Co-Producer Tools  <-->  mcp/ai_audio_worker.py          |
|  (Arioso optional backend or deterministic 44.1 kHz synth stems)  |
+-------------------------------------------------------------------+
```

> Keeping Python AI workers and optional generative models strictly out-of-process across localhost TCP keeps the C++ desktop engine modular and free of Python runtime coupling.

---

## Implemented Work Orders (`WO-001` through `WO-011`)

| Phase | Highlights |
| --- | --- |
| **WO-001 – WO-002** | Atomic `.tracktionedit` session persistence (`New`, `Open`, `Save`, `Save As`), interruptible background VST3 scanner thread (`SafePointer` UI marshal + generation gating), and headless `--smoke-test`. |
| **WO-003** | Multi-track arrangement timeline (`ArrangementView`) and grid-snapping piano roll (`PianoRollComponent`) with full `UndoManager` integration. |
| **WO-004** | Multi-channel `MixerComponent` with VST3 / 4OSC insert chains, pan, dB volume faders, mute/solo, and audio/MIDI device settings dialog. |
| **WO-005** | Reason-style `ModularRackComponent` backed by `te::RackType` with virtual cable patching (`CvLfoPlugin`, `CvWobbleFilterPlugin`) and grid collision avoidance. |
| **WO-006** | Riff-layering `BgmArrangementSolver`, starter WAV riff pack provisioner (`RiffLibrary`), and time-stretched / pitch-shifted `WaveAudioClip` arrangement. |
| **WO-007** | Native TCP JSON-RPC 2.0 server (`DawIpcServer` on `127.0.0.1:9450`) and FastMCP bridge (`mcp/daw_mcp_server.py`). |
| **WO-008** | Non-destructive `WaveformEditorComponent` with selection scrubbing and bounce-to-take DSP (`Normalize`, `Reverse`, `Fade In`, `Fade Out`, `Silence`, `Trim`, `Split`) plus `audio.process_clip`. |
| **WO-009** | Portable relative audio resolution (`Edit::filePathResolver`), `MixdownExporter` stereo WAV mixdowns (`Export WAV` / `audio.export_wav`), and MCP rack patching (`rack.get_state`, `rack.patch`) + stem import (`audio.import_file`). |
| **WO-010** | Two-row `1280x820` studio header, metronome `Click` & `Loop` range transport, `MidiKeyboardDock` (live synth audition + quantized `Step Rec`), and `scripts/package-windows.ps1` release archiving. |
| **WO-011** | `CvUtilityPlugin` 1-to-4 CV attenuverter/splitter (`+ CV Splitter`), per-track `te::EqualiserPlugin` 3-band EQ (`mixer.set_eq`), live L/R `LevelMeterPlugin` mixer metering, and out-of-process hybrid AI audio worker (`mcp/ai_audio_worker.py`). |

---

## Prerequisites

- **OS:** Windows 10 / 11 (x64)
- **Compiler:** Visual Studio 2022 (`Desktop development with C++`)
- **Build System:** CMake >= 3.22
- **Python (optional, for MCP Co-Producer):** Python >= 3.10

### Fetch Submodules

```powershell
git submodule update --init --recursive
```

This populates `third_party/JUCE` and `third_party/tracktion_engine` (`tracktion_core`, `tracktion_graph`, `tracktion_engine`).

---

## Build, Smoke Test & Windows Packaging

```powershell
# 1. Configure & build Release (VS 2022 x64)
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release -j 8

# 2. Run headless end-to-end engine & IPC smoke test
Start-Process -FilePath ".\build\AIDAW_artefacts\Release\AI DAW.exe" -ArgumentList "--smoke-test" -NoNewWindow -Wait -PassThru

# 3. Run zero-dependency Python MCP worker unit tests
python -B -m unittest discover -s mcp -p "test_*.py" -v

# 4. Package portable Windows Release archive (dist/ToxicCreationzStudio-win64.zip)
.\scripts\package-windows.ps1 -BuildConfig Release -OutputDir dist
```

---

## AI Co-Producer MCP Server Setup (`127.0.0.1:9450`)

1. Launch **Toxic Creationz Studio** (`AI DAW.exe`); the native IPC server automatically listens on `127.0.0.1:9450`.
2. Install Python dependencies and start the MCP server (for Claude Desktop, Cursor, or Antigravity):

```powershell
pip install -r mcp/requirements.txt
python mcp/daw_mcp_server.py
```

### Exposed MCP Tools

- **Session & Transport:** `get_session_state`, `control_transport` (`play`, `stop`, `bpm`, `beat`, `click`, `loop`, `loop_start_beat`, `loop_end_beat`)
- **Tracks & Piano Roll:** `create_instrument_track`, `write_piano_roll_notes`, `get_piano_roll_notes`
- **Mixer & EQ:** `set_mixer_channel`, `set_track_eq` (`low_db`, `mid_db`, `high_db` in `[-18, +18]` dB)
- **Modular Rack:** `get_modular_racks`, `patch_modular_rack`
- **Audio, BGM & Export:** `generate_bgm_arrangement`, `process_audio_clip`, `generate_ai_audio_stem`, `import_audio_stem`, `export_master_wav`

`generate_ai_audio_stem` delegates WAV synthesis to `mcp/ai_audio_worker.py` (automatically using `arioso` when installed, or falling back to a stereo 44.1 kHz key/tempo-synced synthesizer), saves stems to `~/Documents/Toxic Creationz Studio/GeneratedStems/`, and imports them cleanly via `audio.import_file`.

---

## Repository Layout

```text
CMakeLists.txt                   # JUCE 9 + Tracktion Engine build configuration
.github/workflows/windows-build.yml  # Windows CI build, --smoke-test, Python unit tests & zip artifact
scripts/package-windows.ps1      # Portable Win64 release zipper (exe + mcp + README)
docs/SHARED_BRIEF.md             # Pipeline status & Work Order engineering records
mcp/
  daw_mcp_server.py              # FastMCP server speaking JSON-RPC 2.0 over 127.0.0.1:9450
  ai_audio_worker.py             # Hybrid AI stem generator (Arioso optional + offline synth fallback)
  test_mcp_server.py             # Zero-dependency unittest suite
  requirements.txt               # mcp, pydantic
src/
  Main.cpp                       # JUCEApplication + headless --smoke-test regression suite
  MainComponent.h/.cpp           # Engine/Edit lifecycle, two-row studio header, filePathResolver, IPC
  ArrangementView.h/.cpp         # Multi-track timeline & loop range editor
  PianoRollComponent.h/.cpp      # Grid-snapping MIDI editor
  MixerComponent.h/.cpp          # Channel strips, 3-band EQ controls, live peak meters & VST3 slots
  CvRackPlugins.h                # Built-in CvLfoPlugin, CvWobbleFilterPlugin & CvUtilityPlugin (1-to-4)
  ModularRackComponent.h/.cpp    # Reason-style te::RackType patchbay GUI
  RiffLibrary.h/.cpp             # Starter riff pack provisioner & key/energy metadata
  BgmGeneratorComponent.h/.cpp   # Generative riff-layering UI & solver
  DawIpcServer.h/.cpp            # Localhost TCP JSON-RPC 2.0 server
  WaveformEditorComponent.h/.cpp # Non-destructive waveform viewer & bounce-to-take DSP
  MixdownExporter.h/.cpp         # Synchronous offline stereo WAV renderer
  MidiKeyboardDock.h/.cpp        # Virtual QWERTY/click keyboard with live audition & Step Rec
```
