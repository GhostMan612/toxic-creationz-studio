# Ghost Tutor Knowledge Bundle

Short, first-time-user answers. The in-app Ghost Tutor embeds the same text as a
compiled fallback so it keeps working when the app is moved standalone.

## record-arming
**What it is:** Arm a track and record audio or MIDI with a count-in.
**Where:** The `Rec` button and per-track arm button in the main transport/mixer.
1. Click a track's arm button.
2. Toggle `Count-In` if you want a one-bar lead-in.
3. Press `Rec`, play, then `Stop`.
4. The take appears as a clip on the armed track.
**RPC/MCP:** `recording.arm_track`, `recording.start`.

## piano-roll-step-rec
**What it is:** Note editor for MIDI clips with a step recorder.
**Where:** The `Keys` toggle and `Step Rec` button.
1. Select a MIDI clip or create one with `+ Track`.
2. Open `Keys` to show the piano roll.
3. Click a grid cell to add a note; drag its edge to resize.
4. Use `Step Rec` to tap notes in time.
**RPC/MCP:** `midi.generate_progression`, `midi.quantize_scale`.

## mixer-eq-meters
**What it is:** Channel strips with gain, pan, EQ and live meters.
**Where:** The `Mixer` toggle.
1. Open `Mixer`.
2. Drag the fader for level and the pan knob for stereo placement.
3. Use the three-band EQ controls to shape tone.
4. Watch the meter to keep peaks below 0 dBFS.
**RPC/MCP:** `mixer.set_gain`, `mixer.auto_balance_lufs`.

## rack-patching-hardware
**What it is:** Modular rack with CV and boutique hardware modules.
**Where:** The `Rack` toggle.
1. Open `Rack` and click `+` to add a module.
2. Drag from an output pin to an input pin to patch.
3. Right-click a cable to disconnect it.
4. Choose from 808, sampler, tape, sidechain and vocal modules.
**RPC/MCP:** `rack.add_module`, `rack.patch`.

## bgm-solver
**What it is:** Riff-layering BGM generator with genre, key and song-form controls.
**Where:** The `BGM Gen` bar.
1. Pick a genre and key.
2. Choose scale, bars and a seed; hit `Randomize` for variety.
3. Click `Generate BGM` to lay out stems.
**RPC/MCP:** `bgm.generate`, `midi.generate_song_arrangement`.

## waveform-bounce
**What it is:** Non-destructive waveform editing and bounce-to-take DSP.
**Where:** The `Wave Edit` toggle.
1. Select an audio clip and open `Wave Edit`.
2. Choose a range and apply normalize, reverse or fade.
3. The result is stored as a take; the original stays intact.
**RPC/MCP:** `audio.process_clip`.

## mixdown-export
**What it is:** Stereo WAV master mixdown export.
**Where:** The `Export WAV` button.
1. Set the export range or use the default edit length.
2. Click `Export WAV` and choose a destination.
3. The file is written as 44.1 kHz / 16-bit PCM.
**RPC/MCP:** `audio.export_mix`, `audio.export_seamless_loop`.

## transcription
**What it is:** Audio-to-MIDI and polyphonic pitch transcription.
**Where:** The `Wave-to-MIDI` action.
1. Select an audio clip.
2. Run transcription; the YIN or basic-pitch backend writes a MIDI clip.
3. Quantize the result to a key and scale.
**RPC/MCP:** `audio.transcribe_to_midi`, `midi.quantize_scale`.

## voice-acting-booth
**What it is:** VO teleprompter, ADR cues, punch-and-roll and Character FX.
**Where:** The `VO Booth` toggle.
1. Open `VO Booth` and load a practice script.
2. Arm the vocal track and read the teleprompter.
3. Use `Punch and Roll` to replace a take.
4. Apply a Character FX preset from the rack.
**RPC/MCP:** `vo.set_script`, `vo.build_demo_reel_layout`.

## cross-platform
**What it is:** `tcs_core` stays platform-neutral; desktop UI lives in `AIDAW`.
**Where:** Build scripts and CI.
1. Windows uses MSVC with JUCE 9 and Tracktion Engine.
2. macOS and Ubuntu build the core isolation target.
3. Android/Oboe defines apply to the core only.
**RPC/MCP:** none (build-time only).

## game-audio-forge
**What it is:** Procedural game SFX, adaptive stems and Godot 4 manifests.
**Where:** The `Game Forge` dock and `BGM Gen` bar.
1. Forge seeded SFX variations with a category and count.
2. Export a seamless loop or adaptive stems.
3. Export a Godot audio pack with `audio_manifest.json`.
**RPC/MCP:** `game.forge_sfx_batch`, `game.export_godot_pack`.

## live-soundboard
**What it is:** Pathfinder God live RPG soundboard and GM voice modes.
**Where:** The `RPG Board` toggle.
1. Pick a biome and set the encounter mood.
2. Tap a 4×4 pad to fire a procedural SFX.
3. Assign custom WAVs to the six user slots.
4. Swap the GM voice preset without stopping ambience.
**RPC/MCP:** `rpg.set_scene`, `rpg.apply_encounter_cue`.
