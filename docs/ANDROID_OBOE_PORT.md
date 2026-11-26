# Android / Oboe Port Guide (WO-020)

This repository now separates the platform-neutral DSP/engine layer from the
desktop GUI layer so an Android client can link the same core without VST3 or
desktop window headers.

## Core boundary

- `tcs_core` (STATIC) — `RiffLibrary`, `RecordingManager`, `MixdownExporter`,
  `PluginPresetManager`, `AudioToMidiTranscriber`. Built with
  `JUCE_PLUGINHOST_VST3=0` and `TCS_ENABLE_DESKTOP_VST3=0`.
- `tcs_core_isolation` (STATIC, `TCS_BUILD_CORE_ISOLATION_CHECK=ON`) — compiles
  `src/CoreIsolationCheck.cpp` against the core with `TCS_CORE_ISOLATION=1`,
  proving at compile time that the DSP/Edit layer needs no desktop UI headers.
- Platform-neutral header-only plugins (`CvRackPlugins.h`,
  `HardwareRackPlugins.h`, `SidechainCompressorPlugin.h`) are safe to include
  from the core; they contain zero includes of `StudioPluginWindow.h`,
  `MainComponent.h`, `MixerComponent.h`, `ModularRackComponent.h`,
  `ArrangementView.h` or `PianoRollComponent.h`.

## Android configure

```bash
cmake -B build-android -S . \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24 \
  -DTCS_BUILD_DESKTOP_APP=OFF \
  -DTCS_ENABLE_DESKTOP_VST3=OFF \
  -DTCS_BUILD_CORE_ISOLATION_CHECK=ON
cmake --build build-android --target tcs_core tcs_core_isolation -j 4
```

`ANDROID` forces both desktop options OFF in `CMakeLists.txt`, so the `AIDAW`
GUI target is not generated at all. `JUCE_USE_ANDROID_OBOE=1` and
`JUCE_PLUGINHOST_VST3=0` are applied to `tcs_core` through
`TCS_ANDROID_OBOE_DEFINITIONS` — an AIDAW-scoped define would be unreachable
because the desktop app is disabled on Android. `PlatformConfig.h` resolves
`TCS_AUDIO_BACKEND_NAME` to `"Oboe"`, `TCS_HAS_DESKTOP_VST3` to `0` and
`TCS_HAS_DESKTOP_UI` to `0` on Android.

This repository provides the **core boundary only**. A runnable Android client
(JNI bridge, AndroidManifest/Gradle project, Oboe callback wiring and touch UI)
is the separate next-phase work described in the checklist below; no first-party
Android application sources exist yet.

## Excluded from Android builds

`src/Main.cpp`, `MainComponent.*`, `ArrangementView.*`, `PianoRollComponent.*`,
`MixerComponent.*`, `ModularRackComponent.*`, `WaveformEditorComponent.*`,
`MidiKeyboardDock.*`, `StudioLookAndFeel.h`, `StudioPluginWindow.h`,
`BgmGeneratorComponent.*`, `DawIpcServer.*`, `AutomationManager.*`,
`AudioToMidiTranscriber.*` is included in the core; the TCP JSON-RPC bridge
(`DawIpcServer`) stays desktop-only.

Note: `BgmArrangementSolver.*` is listed in the work order's audit set but does
not exist in this repository; the neutral BGM solver logic currently lives in
`RiffLibrary.*` / `BgmGeneratorComponent.*`, and only the neutral part is in
`tcs_core`.

## Touch-UI / Oboe integration checklist

1. Build `tcs_core` + `tcs_core_isolation` for `arm64-v8a` and `armeabi-v7a`.
2. Implement a JNI bridge that forwards Oboe input buffers into
   `RecordingManager::commitRecordedAudioTake` and output callbacks into the
   Tracktion `Edit` render path.
3. Replace desktop popup menus and `juce::FileChooser` with Android intents for
   session open/save and take import.
4. Map the existing `ArrangementView` take chip interactions to a touch
   selector (tap = cycle take, long-press = flatten comp).
5. Keep all `Edit` mutation on the JUCE message thread; never touch engine state
   from the Oboe audio callback (existing IPC marshal pattern applies).
6. Run `python -B -m unittest discover -s mcp -p "test_*.py" -v` in CI; the core
   boundary tests are platform independent.
