# RELEASE RUNBOOK — Toxic Creationz Studio v1.0.0

- **Repository:** `C:\Toxic Creationz Studio` (`GhostMan612/toxic-creationz-studio`)
- **Branch:** `main` (synced with `origin/main` at verified HEAD `bddfb78`, zero open PRs)
- **Tag:** `v1.0.0`
- **Artifact:** `dist\ToxicCreationzStudio-win64.zip` (rebuild via `package-windows.ps1` if missing)
- **Executor:** Local Executor (Hermes / Antigravity)

## 1. Pre-Flight Check
Verify `main` is at `bddfb78`, clean (apart from untracked `Muse.txt` and `docs/TO_HERMES.md`), and `dist\ToxicCreationzStudio-win64.zip` exists:

```powershell
git checkout main
git pull origin main
git log -1 --oneline
git status --short
Test-Path .\dist\ToxicCreationzStudio-win64.zip
```

If `dist\ToxicCreationzStudio-win64.zip` is missing, build Release (`AIDAW`) and run `.\scripts\package-windows.ps1 -BuildConfig Release -OutputDir dist`.

## 2. Tag `v1.0.0` Directly on `main` (`bddfb78`) & Push Tag

```powershell
git tag -a v1.0.0 -m "Toxic Creationz Studio v1.0.0"
git -c credential.helper= -c credential.helper='!gh auth git-credential' push origin v1.0.0
```

## 3. Publish GitHub Release `v1.0.0` with Portable Windows Archive

```powershell
gh release create v1.0.0 .\dist\ToxicCreationzStudio-win64.zip `
  --repo GhostMan612/toxic-creationz-studio `
  --title "Toxic Creationz Studio v1.0.0" `
  --notes "## Toxic Creationz Studio v1.0.0

First full production desktop release (C++20 / Tracktion Engine c629312 / JUCE 9.0.5 + Python FastMCP Co-Producer).

### Highlights (WO-001 – WO-020 + PR #17 / PR #18)
- **Core Desktop DAW:** Multi-track arrangement timeline, piano roll, 3-band EQ mixer with Aux FX buses & live peak/RMS metering, live audio/MIDI recording with 1-bar count-in, drawable automation curves, VST3 plugin hosting (`StudioPluginWindow`), and dark studio theme (`StudioLookAndFeel`).
- **Reason-Style Modular Rack & Native Hardware Suite:** Visual CV/Audio/MIDI `RackType` patchbay with `CvLfoPlugin`, `CvWobbleFilterPlugin`, `CvUtilityPlugin` (1-to-4 splitter), `CvEnvelopeFollowerPlugin`, `CvStepModulatorPlugin`, `Tr808DrumPlugin` (5-out + CV Accent + CHat choke), `VintageSamplerPlugin` (12-bit DAC + ZOH decimation + WAV loader), `AnalogTapePlugin` (wow/flutter + 85 Hz head bump + asymmetric hysteresis), `SidechainCompressorPlugin` (Pin 3/4 ducking), and `PluginPresetManager` (`.trkpreset` factory banks).
- **Recording, Comping & Waveform DSP:** Non-destructive waveform bounce-to-take editor, multi-take loop comping (`TakeLane` + equal-power crossfade flatten), monophonic YIN & polyphonic Constant-Q Audio-to-MIDI transcription.
- **AI Co-Producer & BS.1770 Loudness (FastMCP + JSON-RPC 2.0 on `127.0.0.1:9450`):** Track/rack/MIDI/mixer automation, harmonic progression solver, scale quantizer, true BS.1770 K-weighted gated LUFS auto-balance (`src/LoudnessMeter.h`), section duplicator, and out-of-process AI audio/stem worker (`32/32` Python unit tests passing).
- **Cross-Platform & Android/Oboe Readiness:** Guarded MSVC/macOS/Linux/Android CMake matrix, PR cross-platform CI, PE/zip packaging validation, and verified VST3-free `tcs_core` isolation.

### Verification (`main` @ `bddfb78`)
- `0` compiler errors / `0` compiler warnings (MSVC Release x64)
- `5/5` consecutive headless `--smoke-test` passes (including BS.1770 `-9.28 LUFS` check)
- Isolated AddressSanitizer (`RelWithDebInfo` ASAN) smoke test exit `0`
- `32/32` Python MCP unit tests passing"
```

## 4. Spin Fresh Phase 7 Branch Off `main` (`bddfb78`) & Commit Blueprints

```powershell
git checkout -b dev/v1.1-vas main
git add docs/blueprints/RELEASE-v1.0.0.md docs/blueprints/WO-021.md docs/blueprints/WO-022.md
git commit -m "docs(blueprints): add v1.0.0 release runbook and Phase 7 VAS work orders (WO-021, WO-022)"
git -c credential.helper= -c credential.helper='!gh auth git-credential' push -u origin dev/v1.1-vas
```
