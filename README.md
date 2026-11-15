# Toxic Creationz Studio — AI DAW

Phase 1 scaffold for an AI-assisted desktop DAW: JUCE + Tracktion Engine, with
VST3 hosting and a starter arrangement (4 audio tracks, a 4OSC lead synth, a
1-bar MIDI clip).

The full architecture and roadmap live in [docs/BRIEFING.md](docs/BRIEFING.md).

## Prerequisites

- CMake >= 3.22
- Visual Studio 2022 (or newer) with the "Desktop development with C++" workload
- Git with LFS disabled (JUCE/Tracktion are plain git submodules)

## Fetch dependencies

```bash
git submodule update --init --recursive
```

This populates `third_party/JUCE` (9.x) and `third_party/tracktion_engine`
(`tracktion_core`, `tracktion_graph`, `tracktion_engine`).

## Configure and build

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The executable lands in `build/AIDAW_artefacts/Release/` (JUCE's default layout).

## Layout

```
CMakeLists.txt            # JUCE + Tracktion wiring, VST3 host flags
src/Main.cpp              # JUCEApplication + DocumentWindow
src/MainComponent.h/.cpp  # Engine, Edit, transport, VST3 scanner, UI
third_party/JUCE/         # submodule
third_party/tracktion_engine/  # submodule
docs/BRIEFING.md          # architecture blueprint and roadmap
```

## Notes / next steps

- Phase 2: modular rack with audio/MIDI/CV over `te::RackType`.
- Phase 3: riff-layering engine + local loop retrieval.
- Phase 4: native JSON-RPC MCP bridge (`127.0.0.1:9450`) + Python FastMCP worker.
- Phase 5: non-destructive waveform editor, then Android/Oboe evaluation.
