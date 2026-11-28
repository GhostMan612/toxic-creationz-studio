# Toxic Creationz Studio — Phase 7–9 Master Suite Expansion Map

Post-`v1.0.0` (`main` @ `bddfb78`) roadmap turning Toxic Creationz Studio into a multi-suite audio workstation feeding your voice acting, Godot 4 game dev (`TacPlanShooter`, `Sovereign-Nodes`, `godot4-asset-vault`), and tabletop RPG (`Pathfinder God`) ecosystems.

| Phase | Branch | Work Orders | Suite & Target Ecosystem | Core Deliverables | Python Tests |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Phase 7** | `dev/v1.1-vas` | `WO-021`, `WO-022` | **Voice Acting Studio Suite** *(Anime/Cartoon ADR, ACX, Indie Games)* | `VocalChannelPlugin`, Punch-and-Roll & A/B/C Audition Export, BS.1770/ACX `VocalSpecAnalyzer`, `ScriptPrompterComponent`, monitor-only Anime ADR 3-beep cue, Character FX presets, AI Voice Director MCP | `32 → 40` |
| **Phase 8** | `dev/v1.2-game-forge` | `WO-023`, `WO-024` | **Game Audio & Asset Forge Suite** *(`TacPlanShooter`, `Sovereign-Nodes`, `godot4-asset-vault`)* | Procedural `SfxForgeEngine` (seeded one-shot variations), zero-click tail-wrapped seamless loop renderer, phase-locked adaptive BGM stems (`calm`/`tension`/`combat`/`boss`), VOX bark batch auto-trim + BS.1770 normalizer, Godot 4 `res://audio/` + `audio_manifest.json` packager | `40 → 48` |
| **Phase 9** | `dev/v1.3-rpg-soundboard` | `WO-025`, `WO-026` | **Pathfinder God Live Soundboard Suite** *(`Pathfinder God`, Tabletop & Cosmic Horror Campaigns)* | Real-time `RpgSoundboardEngine` (crossfading ambient beds + on-the-spot procedural spell/combat/horror one-shots), `RpgSoundboardComponent` 4×4 live pad dock + mood slider (`Exploration → Tension → Boss`), live GM Character-Voice toggle, Pathfinder God encounter MCP bridge | `48 → 56` |

## Shared Architectural Rules (All Suites)
- **Core Isolation (`tcs_core_isolation`):** All DSP plugins, procedural synthesizers, loudness/spec analyzers, and batch exporters stay strictly platform-neutral (zero desktop UI or VST3 headers) so Android/Oboe can reuse them directly.
- **Loudness Consistency:** Reuse `src/LoudnessMeter.h` (BS.1770 K-weighted gated LUFS) for all normalization and spec validation.
- **Execution Gates:** Every Work Order requires `0` compiler errors/warnings (`warning C#### = 0`), `5/5` Release `--smoke-test` PASS, isolated `RelWithDebInfo` ASAN `--smoke-test` exit `0`, passing Python `unittest`, and validated `dist\ToxicCreationzStudio-win64.zip`.
