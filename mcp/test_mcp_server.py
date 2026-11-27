"""Zero-dependency unit tests for the out-of-process MCP co-producer worker."""

from __future__ import annotations

import asyncio
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest.mock import AsyncMock, patch
import wave

_MCP_DIR = Path(__file__).resolve().parent
if str(_MCP_DIR) not in sys.path:
    sys.path.insert(0, str(_MCP_DIR))

from ai_audio_worker import (
    generate_stem,
    generate_vo_practice_script,
    parse_root_midi,
    separate_stems,
    transcribe_polyphonic_basic_pitch,
)


def _load_daw_mcp_server():
    """Load daw_mcp_server.py without requiring external pip packages in CI."""
    mcp_pkg = types.ModuleType("mcp")
    mcp_server_pkg = types.ModuleType("mcp.server")
    mcp_fastmcp_pkg = types.ModuleType("mcp.server.fastmcp")

    class _StubFastMCP:
        def __init__(self, name: str):
            self.name = name

        def tool(self):
            def _decorator(fn):
                return fn

            return _decorator

        def run(self):
            return None

    mcp_fastmcp_pkg.FastMCP = _StubFastMCP
    mcp_server_pkg.fastmcp = mcp_fastmcp_pkg
    mcp_pkg.server = mcp_server_pkg

    stubs: dict[str, types.ModuleType] = {
        "mcp": mcp_pkg,
        "mcp.server": mcp_server_pkg,
        "mcp.server.fastmcp": mcp_fastmcp_pkg,
    }

    if importlib.util.find_spec("pydantic") is None:
        pydantic_pkg = types.ModuleType("pydantic")

        class _StubBaseModel:
            def __init__(self, **kwargs):
                for k, v in kwargs.items():
                    setattr(self, k, v)

            def model_dump(self):
                return dict(self.__dict__)

        def _stub_field(default=..., **_kwargs):
            return None if default is ... else default

        pydantic_pkg.BaseModel = _StubBaseModel
        pydantic_pkg.Field = _stub_field
        stubs["pydantic"] = pydantic_pkg

    server_path = _MCP_DIR / "daw_mcp_server.py"
    with patch.dict(sys.modules, stubs):
        spec = importlib.util.spec_from_file_location("daw_mcp_server_under_test", server_path)
        assert spec is not None and spec.loader is not None
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod


class McpWorkerTests(unittest.TestCase):
    def test_parse_root_midi(self) -> None:
        self.assertEqual(parse_root_midi("Am"), (45, True))
        self.assertEqual(parse_root_midi("Cm"), (36, True))
        self.assertEqual(parse_root_midi("F#m"), (42, True))
        self.assertEqual(parse_root_midi("Bb"), (46, False))

    def test_generate_stem_fallback_wav_integrity(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out_wav = Path(tmp) / "nested" / "test_stem.wav"
            meta = generate_stem(
                prompt="dark cyberpunk bassline",
                duration_sec=1.5,
                output_path=out_wav,
                backend="auto",
                key="Am",
                bpm=124.0,
            )

            self.assertEqual(meta["backend"], "fallback_synth")
            self.assertTrue(out_wav.is_file())

            with wave.open(str(out_wav), "rb") as wf:
                self.assertEqual(wf.getnchannels(), 2)
                self.assertEqual(wf.getsampwidth(), 2)
                self.assertEqual(wf.getframerate(), 44100)
                self.assertEqual(wf.getnframes(), int(round(44100 * 1.5)))
                frames = wf.readframes(4096)

            unpacked = struct.unpack(f"<{len(frames) // 2}h", frames)
            peak = max(abs(s) for s in unpacked)
            self.assertGreater(peak, 500)

    def test_set_track_eq_clamps_and_sends_rpc(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"track_index": 2, "low_db": 18.0, "mid_db": -3.5, "high_db": -18.0})

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.set_track_eq(2, low_db=25.0, mid_db=-3.5, high_db=-30.0))

        self.assertEqual(res["low_db"], 18.0)
        mock_rpc.assert_awaited_once_with(
            "mixer.set_eq",
            {"track_index": 2, "low_db": 18.0, "mid_db": -3.5, "high_db": -18.0},
        )

    def test_generate_ai_audio_stem_generates_and_imports(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"track_index": 1, "clip_id": 42, "clip_name": "CyberPad"})

        with tempfile.TemporaryDirectory() as tmp:
            stems_dir = Path(tmp) / "stems"
            with (
                patch.object(srv, "get_generated_stems_dir", return_value=stems_dir),
                patch.object(srv, "send_daw_rpc", mock_rpc),
            ):
                res = asyncio.run(
                    srv.generate_ai_audio_stem(
                        prompt="cyberpunk shimmer pad",
                        track_index=1,
                        start_beat=4.0,
                        duration_sec=1.0,
                        key="Cm",
                        bpm=128.0,
                        backend="fallback",
                        clip_name="CyberPad",
                    )
                )

            wav_path = Path(res["generation"]["output_path"])
            self.assertTrue(wav_path.is_file())
            self.assertGreater(wav_path.stat().st_size, 44)
            mock_rpc.assert_awaited_once()

            call_args = mock_rpc.await_args
            self.assertEqual(call_args.args[0], "audio.import_file")
            self.assertEqual(call_args.kwargs.get("timeout"), 15.0)
            payload = call_args.args[1]
            self.assertEqual(payload["track_index"], 1)
            self.assertEqual(payload["start_beat"], 4.0)
            self.assertEqual(payload["clip_name"], "CyberPad")
            self.assertEqual(payload["file_path"], str(wav_path))

    def test_list_recording_inputs_rpc(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"audio_inputs": ["Mic 1/2"], "midi_inputs": ["Keystation"]})

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.list_recording_inputs())

        self.assertEqual(res["audio_inputs"], ["Mic 1/2"])
        mock_rpc.assert_awaited_once_with("recording.list_inputs")

    def test_set_mixer_channel_armed_and_input_device(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"updated_track": 0, "armed": True, "input_device": "Virtual MIDI"})

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.set_mixer_channel(0, armed=True, input_device="Virtual MIDI"))

        self.assertTrue(res["armed"])
        mock_rpc.assert_awaited_once_with(
            "mixer.set_track",
            {"track_index": 0, "input_device": "Virtual MIDI", "armed": True},
        )

    def test_control_transport_record_and_count_in(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"is_recording": True, "count_in_bars": 1})

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.control_transport(action="record", count_in_bars=2))

        self.assertTrue(res["is_recording"])
        mock_rpc.assert_awaited_once_with(
            "transport.control",
            {"action": "record", "count_in_bars": 1},
        )

    def test_write_track_automation_rpc(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"track_index": 0, "parameter": "volume", "point_count": 2})

        pts = [srv.AutomationPoint(beat=0.0, value=-12.0, curve=0.0), srv.AutomationPoint(beat=4.0, value=0.0, curve=0.2)]
        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.write_track_automation(0, "volume", pts, clear_existing=True))

        self.assertEqual(res["point_count"], 2)
        mock_rpc.assert_awaited_once_with(
            "automation.set_points",
            {
                "track_index": 0,
                "parameter": "volume",
                "points": [
                    {"beat": 0.0, "value": -12.0, "curve": 0.0},
                    {"beat": 4.0, "value": 0.0, "curve": 0.2},
                ],
                "clear_existing": True,
            },
        )

    def test_get_track_automation_rpc(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"track_index": 0, "parameter": "pan", "points": []})

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.get_track_automation(0, parameter="pan"))

        self.assertEqual(res["parameter"], "pan")
        mock_rpc.assert_awaited_once_with(
            "automation.get_points",
            {"track_index": 0, "parameter": "pan"},
        )

    def test_configure_fx_send_clamps_and_sends_rpc(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"track_index": 1, "bus_index": 7, "send_db": -9.0, "wet_level": 1.0})

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.configure_fx_send(1, send_db=-9.0, bus_index=12, effect="REVERB", wet_level=1.5))

        self.assertEqual(res["bus_index"], 7)
        mock_rpc.assert_awaited_once_with(
            "mixer.set_fx_send",
            {
                "track_index": 1,
                "bus_index": 7,
                "send_db": -9.0,
                "effect": "reverb",
                "wet_level": 1.0,
            },
        )

    def test_transcribe_audio_to_midi_clamps_and_uses_15s_timeout(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(
            return_value={
                "target_track_index": 3,
                "clip_name": "Vocal MIDI",
                "notes_transcribed": 4,
                "notes": [],
            }
        )

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(
                srv.transcribe_audio_to_midi(
                    track_index=0,
                    clip_index=-2,
                    target_track_index=3,
                    quantize_beats=5.0,
                    min_note_beats=0.001,
                )
            )

        self.assertEqual(res["target_track_index"], 3)
        mock_rpc.assert_awaited_once_with(
            "audio.transcribe_to_midi",
            {
                "track_index": 0,
                "clip_index": 0,
                "target_track_index": 3,
                "quantize_beats": 2.0,
                "min_note_beats": 0.03125,
                "polyphonic": False,
                "max_voices": 4,
            },
            timeout=15.0,
        )

    def test_transcribe_audio_to_midi_defaults(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(
            return_value={
                "target_track_index": 2,
                "clip_name": "Bass MIDI",
                "notes_transcribed": 2,
                "notes": [],
            }
        )

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.transcribe_audio_to_midi(1))

        self.assertEqual(res["notes_transcribed"], 2)
        mock_rpc.assert_awaited_once_with(
            "audio.transcribe_to_midi",
            {
                "track_index": 1,
                "clip_index": 0,
                "target_track_index": -1,
                "quantize_beats": 0.25,
                "min_note_beats": 0.125,
                "polyphonic": False,
                "max_voices": 4,
            },
            timeout=15.0,
        )

    def test_set_plugin_preset_clamps_and_forwards(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(
            return_value={
                "track_index": 0,
                "plugin_index": 0,
                "plugin_name": "4OSC",
                "applied_preset": "Reese Neuro Bass",
                "available_presets": ["Init Sine", "Reese Neuro Bass"],
            }
        )

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(
                srv.set_plugin_preset(
                    track_index=-3,
                    preset_name="  Reese Neuro Bass  ",
                    plugin_index=-1,
                )
            )

        self.assertEqual(res["applied_preset"], "Reese Neuro Bass")
        mock_rpc.assert_awaited_once_with(
            "plugin.set_preset",
            {
                "track_index": 0,
                "plugin_index": 0,
                "preset_name": "Reese Neuro Bass",
            },
        )

    def test_add_rack_machine_clamps_and_normalizes(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(
            return_value={
                "rack_index": 0,
                "plugin_id": 91,
                "plugin_name": "Sampler",
                "machine_type": "sampler",
                "available_presets": ["Neon 808 + Synth Kit"],
            }
        )

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.add_rack_machine(machine_type="  SAMPLER ", rack_index=-2))

        self.assertEqual(res["machine_type"], "sampler")
        mock_rpc.assert_awaited_once_with(
            "rack.add_machine",
            {
                "rack_index": 0,
                "machine_type": "sampler",
            },
        )


    def test_add_rack_module_clamps_and_forwards(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(
            return_value={
                "rack_index": 0,
                "plugin_id": 108,
                "name": "TR-808 Drum Synth",
                "inputs": ["CV Accent In"],
                "outputs": ["Master L", "Master R", "Kick Out", "Snare Out", "Hat/Tom Out"],
            }
        )

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(
                srv.add_rack_module(module_type="  TR808 ", rack_index=-3, x=0.4, y=0.6)
            )

        self.assertEqual(res["name"], "TR-808 Drum Synth")
        mock_rpc.assert_awaited_once_with(
            "rack.add_module",
            {
                "rack_index": 0,
                "module_type": "tr808",
                "x": 0.4,
                "y": 0.6,
            },
        )

    def test_create_instrument_track_hardware_types(self) -> None:
        srv = _load_daw_mcp_server()

        for instrument in ("tr808", "sampler", "tape", "none"):
            mock_rpc = AsyncMock(return_value={"track_index": 0, "name": "HW Track"})
            with patch.object(srv, "send_daw_rpc", mock_rpc):
                res = asyncio.run(srv.create_instrument_track(name="HW Track", instrument=instrument))
            self.assertEqual(res["track_index"], 0)
            mock_rpc.assert_awaited_once_with(
                "track.create", {"name": "HW Track", "plugin_type": instrument}
            )

    def test_generate_stem_cloud_fallback_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out_wav = Path(tmp) / "suno_fallback.wav"
            meta = generate_stem(
                prompt="neon synthwave lead",
                duration_sec=1.0,
                output_path=out_wav,
                backend="suno",
                key="Cm",
                bpm=120.0,
            )
            self.assertEqual(meta["backend"], "fallback_synth")
            self.assertEqual(meta["backend_used"], "fallback_synth")
            self.assertIn("SUNO_API_KEY", meta["fallback_reason"])
            self.assertTrue(out_wav.is_file())

    def test_separate_stems_offline_four_wav_integrity(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src_wav = Path(tmp) / "mix.wav"
            generate_stem(
                prompt="full electronic groove",
                duration_sec=1.0,
                output_path=src_wav,
                backend="offline",
                key="Am",
                bpm=124.0,
            )
            stems_dir = Path(tmp) / "stems"
            res = separate_stems(src_wav, stems_dir, backend="offline")
            self.assertEqual(res["backend_used"], "offline_spectral_splitter")
            self.assertEqual(set(res["stems"].keys()), {"drums", "bass", "vocals", "other"})

            for stem_name, stem_path_str in res["stems"].items():
                stem_path = Path(stem_path_str)
                self.assertTrue(stem_path.is_file(), f"Missing stem {stem_name}")
                with wave.open(str(stem_path), "rb") as wf:
                    self.assertEqual(wf.getnchannels(), 2)
                    self.assertEqual(wf.getsampwidth(), 2)
                    self.assertEqual(wf.getframerate(), 44100)
                    frames = wf.readframes(wf.getnframes())
                unpacked = struct.unpack(f"<{len(frames) // 2}h", frames)
                self.assertGreater(max(abs(s) for s in unpacked), 100, f"Stem {stem_name} was silent")

    def test_generate_chord_progression_rpc(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"track_index": 0, "chords_written": 4, "notes_written": 12})

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(
                srv.generate_chord_progression(
                    track_index=-2,
                    key="Cm",
                    scale=" MINOR ",
                    progression=["i", "VI", "III", "VII"],
                    start_beat=-1.0,
                    beats_per_chord=4.0,
                    octave=9,
                    velocity=140,
                    voice_lead=True,
                )
            )

        self.assertEqual(res["chords_written"], 4)
        mock_rpc.assert_awaited_once_with(
            "midi.generate_progression",
            {
                "track_index": 0,
                "key": "Cm",
                "scale": "minor",
                "progression": ["i", "VI", "III", "VII"],
                "start_beat": 0.0,
                "beats_per_chord": 4.0,
                "octave": 7,
                "velocity": 127,
                "voice_lead": True,
            },
        )

    def test_quantize_track_to_scale_rpc(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"track_index": 1, "notes_checked": 8, "pitch_snapped": 2, "timing_snapped": 3})

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(
                srv.quantize_track_to_scale(
                    track_index=1,
                    key="C",
                    scale=" PENTATONIC_MINOR ",
                    grid_beats=0.001,
                    snap_pitch=True,
                    snap_timing=True,
                )
            )

        self.assertEqual(res["pitch_snapped"], 2)
        mock_rpc.assert_awaited_once_with(
            "midi.quantize_scale",
            {
                "track_index": 1,
                "key": "C",
                "scale": "pentatonic_minor",
                "grid_beats": 0.03125,
                "snap_pitch": True,
                "snap_timing": True,
            },
        )

    def test_auto_balance_and_duplicate_section_rpcs(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(side_effect=[
            {"target_lufs": -14.0, "tracks_adjusted": []},
            {"clips_duplicated": 3, "dest_start_beat": 8.0, "dest_end_beat": 12.0},
        ])

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            lufs_res = asyncio.run(srv.auto_balance_mix_lufs(target_lufs=-50.0, max_gain_db=9.0, min_gain_db=-80.0))
            dup_res = asyncio.run(srv.duplicate_arrangement_section(0.0, 4.0, 8.0, track_indices=[0, -1], mute_drums=True))

        self.assertEqual(lufs_res["target_lufs"], -14.0)
        self.assertEqual(dup_res["clips_duplicated"], 3)
        self.assertEqual(mock_rpc.await_args_list[0].args[0], "mixer.auto_balance_lufs")
        self.assertEqual(
            mock_rpc.await_args_list[0].args[1],
            {"target_lufs": -40.0, "max_gain_db": 6.0, "min_gain_db": -60.0, "headroom_db": -1.0},
        )
        self.assertEqual(mock_rpc.await_args_list[0].kwargs.get("timeout"), 10.0)
        self.assertEqual(mock_rpc.await_args_list[1].args[0], "arrangement.duplicate_section")
        self.assertEqual(
            mock_rpc.await_args_list[1].args[1],
            {
                "start_beat": 0.0,
                "end_beat": 4.0,
                "dest_beat": 8.0,
                "mute_drums": True,
                "track_indices": [0, 0],
            },
        )

    def test_separate_audio_stems_tool_creates_tracks_and_imports(self) -> None:
        srv = _load_daw_mcp_server()
        track_calls = iter([
            {"track_index": 4, "name": "mix (Drums)"},
            {"track_index": 5, "name": "mix (Bass)"},
            {"track_index": 6, "name": "mix (Vocals)"},
            {"track_index": 7, "name": "mix (Other)"},
        ])

        async def _fake_rpc(method: str, params: dict | None = None, **_kwargs):
            if method == "track.create":
                return next(track_calls)
            return {"track_index": params["track_index"], "clip_name": params["clip_name"]}

        mock_rpc = AsyncMock(side_effect=_fake_rpc)

        with tempfile.TemporaryDirectory() as tmp:
            src_wav = Path(tmp) / "mix.wav"
            generate_stem("synth bass", 0.75, src_wav, backend="offline")
            sep_dir = Path(tmp) / "sep_out"
            with (
                patch.object(srv, "get_separated_stems_dir", return_value=sep_dir),
                patch.object(srv, "send_daw_rpc", mock_rpc),
            ):
                res = asyncio.run(srv.separate_audio_stems(str(src_wav), import_to_daw=True, start_beat=4.0, backend="offline"))

            self.assertEqual(len(res["imported"]), 4)
            self.assertEqual(mock_rpc.await_count, 8)
            for stem_path in res["separation"]["stems"].values():
                self.assertTrue(Path(stem_path).is_file())


    def test_add_rack_module_sidechain_comp(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(
            return_value={"rack_index": 1, "name": "Sidechain Compressor", "inputs": 4, "outputs": 2}
        )

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.add_rack_module("sidechain_comp", rack_index=1))

        self.assertEqual(res["name"], "Sidechain Compressor")
        mock_rpc.assert_awaited_once_with(
            "rack.add_module",
            {"rack_index": 1, "module_type": "sidechain_comp", "x": 0.25, "y": 0.5},
        )

    def test_transcribe_audio_to_midi_polyphonic(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(
            return_value={"notes_transcribed": 3, "notes": [{"pitch": 60}, {"pitch": 63}, {"pitch": 67}]}
        )

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(
                srv.transcribe_audio_to_midi(2, clip_index=1, polyphonic=True, max_voices=99)
            )

        self.assertEqual(res["notes_transcribed"], 3)
        mock_rpc.assert_awaited_once_with(
            "audio.transcribe_to_midi",
            {
                "track_index": 2,
                "clip_index": 1,
                "target_track_index": -1,
                "quantize_beats": 0.25,
                "min_note_beats": 0.125,
                "polyphonic": True,
                "max_voices": 8,
            },
            timeout=15.0,
        )

    def test_comp_track_takes_clamps_and_forwards(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(
            return_value={"track_index": 0, "output_file": "C:/tmp/Comp1.wav", "crossfade_ms": 2000.0}
        )

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(
                srv.comp_track_takes(
                    0, action="FLATTEN", take_index=1, crossfade_ms=9000.0, start_beat=-2.0
                )
            )

        self.assertEqual(res["crossfade_ms"], 2000.0)
        mock_rpc.assert_awaited_once_with(
            "recording.comp_takes",
            {
                "track_index": 0,
                "action": "flatten",
                "take_index": 1,
                "crossfade_ms": 2000.0,
                "start_beat": 0.0,
                "end_beat": 4.0,
                "lane_index": 0,
            },
            timeout=15.0,
        )

    def test_transcribe_polyphonic_basic_pitch_offline_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src_wav = Path(tmp) / "triad.wav"
            generate_stem("C minor triad", 0.6, src_wav, backend="offline")

            with patch(
                "ai_audio_worker.importlib.import_module", side_effect=ImportError("no basic_pitch")
            ):
                res = transcribe_polyphonic_basic_pitch(src_wav, Path(tmp) / "triad.mid")

        self.assertEqual(res["backend_used"], "cpp_polyphonic_fallback")
        self.assertEqual(res["status"], "unavailable")
        self.assertIn("basic_pitch unavailable", res["fallback_reason"])
        self.assertEqual(res["note_count"], 0)
        self.assertEqual(res["notes"], [])


    def test_cmake_msvc_guards_and_platform_branches(self) -> None:
        cmake_text = (_MCP_DIR.parent / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("if(MSVC)", cmake_text)
        self.assertIn("else()", cmake_text)
        for flag in ("/MP", "/bigobj", "/W4", "/utf-8"):
            self.assertIn(flag, cmake_text)
        for flag in ("-Wall", "-Wextra", "-fPIC"):
            self.assertIn(flag, cmake_text)
        for branch in (
            "if(ANDROID)",
            "elseif(APPLE AND NOT IOS)",
            "elseif(UNIX AND NOT APPLE AND NOT ANDROID)",
        ):
            self.assertIn(branch, cmake_text)
        for option in (
            'option(TCS_ENABLE_DESKTOP_VST3',
            'option(TCS_BUILD_DESKTOP_APP',
            'option(TCS_BUILD_CORE_ISOLATION_CHECK',
        ):
            self.assertIn(option, cmake_text)
        self.assertIn("add_library(tcs_core STATIC", cmake_text)
        self.assertIn("add_library(tcs_core_isolation STATIC", cmake_text)
        isolation_block = cmake_text.split("add_library(tcs_core_isolation STATIC", 1)[1].split("endif()", 1)[0]
        for definition in ("JUCE_USE_CURL=0", "JUCE_WEB_BROWSER=0", "JUCE_PLUGINHOST_VST3=0"):
            self.assertIn(definition, isolation_block)
        self.assertIn("target_link_libraries(tcs_core PUBLIC atomic)", cmake_text)
        self.assertIn("target_link_libraries(AIDAW PRIVATE atomic)", cmake_text)
        # Android Oboe must be applied to tcs_core: TCS_BUILD_DESKTOP_APP is
        # forced OFF on Android, so an AIDAW-scoped define is unreachable.
        self.assertIn("set(TCS_ANDROID_OBOE_DEFINITIONS JUCE_USE_ANDROID_OBOE=1 JUCE_PLUGINHOST_VST3=0)", cmake_text)
        self.assertIn("${TCS_ANDROID_OBOE_DEFINITIONS}", cmake_text)
        self.assertNotIn("elseif(ANDROID)", cmake_text)

    def test_cross_platform_workflow_parses(self) -> None:
        workflow = _MCP_DIR.parent / ".github" / "workflows" / "cross-platform-build.yml"
        text = workflow.read_text(encoding="utf-8")

        self.assertNotIn("\t", text)
        for marker in ("name:", "on:", "workflow_dispatch:", "jobs:", "runs-on: macos-15",
                       "runs-on: ubuntu-22.04", "python3 -B -m unittest discover"):
            self.assertIn(marker, text)
        self.assertNotIn("runs-on: macos-14", text)
        self.assertNotIn("runs-on: windows-latest", text)
        self.assertIn("pull_request:", text)
        self.assertIn("tcs_core_isolation AIDAW", text)
        for apt_dep in ("libxi-dev", "libcurl4-openssl-dev", "libxext-dev"):
            self.assertIn(apt_dep, text)

    def test_lufs_meter_is_bs1770_gated(self) -> None:
        header = (_MCP_DIR.parent / "src" / "LoudnessMeter.h").read_text(encoding="utf-8")
        for marker in ("makeHighShelf", "makeHighPass", "1681.974f", "38.0f",
                       "absoluteGate", "relativeGate", "-0.691"):
            self.assertIn(marker, header)
        server = (_MCP_DIR.parent / "src" / "DawIpcServer.cpp").read_text(encoding="utf-8")
        self.assertIn("KWeightedLoudnessMeter", server)
        self.assertIn("bs1770_k_weighted_gated", server)
    def test_cv_blueprint_modules_are_registered(self) -> None:
        header = (_MCP_DIR.parent / "src" / "CvRackPlugins.h").read_text(encoding="utf-8")
        for marker in (
            "class CvEnvelopeFollowerPlugin",
            "class CvStepModulatorPlugin",
            'xmlTypeName = "cv_envelope_follower"',
            'xmlTypeName = "cv_step_modulator"',
            "pm.createBuiltInType<CvEnvelopeFollowerPlugin>()",
            "pm.createBuiltInType<CvStepModulatorPlugin>()",
        ):
            self.assertIn(marker, header)
        self.assertNotIn("(float)", header)  # locked zero-warning invariant

    def test_windows_workflow_validates_packaged_zip(self) -> None:
        text = (_MCP_DIR.parent / ".github" / "workflows" / "windows-build.yml").read_text(encoding="utf-8")
        self.assertIn("Validate Packaged Artifact", text)
        self.assertIn("ZipFile]::OpenRead", text)
        self.assertIn("Toxic Creationz Studio.exe", text)

    def test_package_script_rejects_non_pe_executable(self) -> None:
        text = (_MCP_DIR.parent / "scripts" / "package-windows.ps1").read_text(encoding="utf-8")
        self.assertIn("$Header[0] -ne 0x4D", text)
        self.assertIn("$Header[1] -ne 0x5A", text)
        self.assertIn("ZipFile]::OpenRead", text)

    def test_analyze_vo_specs_tool(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"passed": True, "lufs": -18.4, "measurement": "bs1770_k_weighted_gated"})

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.analyze_vo_specs("/tmp/vo.wav", profile="ACX"))

        self.assertTrue(res["passed"])
        mock_rpc.assert_awaited_once_with(
            "audio.analyze_vo_specs",
            {"file_path": "/tmp/vo.wav", "track_index": 0, "clip_index": 0, "profile": "acx"},
            timeout=15.0,
        )

    def test_export_abc_audition_tool(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"output_file": "C:/tmp/abc.wav"})

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.export_abc_audition(1, take_indices=[2, 0], gap_seconds=99.0, mono=False))

        self.assertEqual(res["output_file"], "C:/tmp/abc.wav")
        mock_rpc.assert_awaited_once_with(
            "recording.export_abc_audition",
            {"track_index": 1, "take_indices": [2, 0], "gap_seconds": 10.0, "mono": False, "output_path": ""},
            timeout=15.0,
        )

    def test_punch_and_roll_splice_tool(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"output_file": "C:/tmp/punch.wav"})

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.punch_and_roll_splice(0, "a.wav", "b.wav", punch_seconds=2.5, crossfade_ms=1.0))

        self.assertEqual(res["output_file"], "C:/tmp/punch.wav")
        mock_rpc.assert_awaited_once_with(
            "recording.punch_and_roll",
            {"track_index": 0, "prior_wav": "a.wav", "new_take_wav": "b.wav",
             "punch_seconds": 2.5, "crossfade_ms": 5.0, "output_path": ""},
            timeout=15.0,
        )

    def test_wo021_plugin_and_analyzer_sources(self) -> None:
        root = _MCP_DIR.parent
        plugin = (root / "src" / "VocalChannelPlugin.h").read_text(encoding="utf-8")
        for marker in ("tcs_vocal_channel", "Vocal Booth Strip", "currentLimiterGrDb",
                       "deEssFreqHz", "breathFloorDb", "ceilingDb"):
            self.assertIn(marker, plugin)
        analyzer = (root / "src" / "VocalSpecAnalyzer.h").read_text(encoding="utf-8")
        for marker in ("LoudnessMeter.h", "bs1770_k_weighted_gated", 'out.profile == "acx"',
                       "Audition integrated loudness", "noiseFloorDb", "headSilenceSec", "tailSilenceSec"):
            self.assertIn(marker, analyzer)
        server = (root / "src" / "DawIpcServer.cpp").read_text(encoding="utf-8")
        for method in ("audio.analyze_vo_specs", "recording.export_abc_audition", "recording.punch_and_roll"):
            self.assertIn(method, server)

    def test_load_vo_script_tool(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"title": "VO Script", "wpm": 140})

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.load_vo_script("Line one.", title="  Audition  ", wpm=999, adr_enabled=True))

        self.assertEqual(res["wpm"], 140)
        mock_rpc.assert_awaited_once_with(
            "vo.set_script",
            {"title": "Audition", "text": "Line one.", "wpm": 220,
             "adr_enabled": True, "adr_line_sec": 4.0},
            timeout=15.0,
        )

    def test_generate_vo_practice_script_all_genres_and_five_p(self) -> None:
        genres = ("anime", "cartoon", "commercial", "villain", "game_barks", "audiobook")
        seen_titles: set[str] = set()
        for genre in genres:
            with self.subTest(genre=genre):
                result = generate_vo_practice_script(genre, "determined", 20.0)
                self.assertEqual(result["genre"], genre)
                self.assertTrue(result["text"])
                self.assertTrue(result["adr_windows"])
                for cue in ("pitch", "pace", "placement", "posture", "personality"):
                    self.assertIn(cue, result["five_p"])
                seen_titles.add(result["title"])
        self.assertEqual(len(seen_titles), len(genres))
        self.assertEqual(generate_vo_practice_script("unknown")["genre"], "anime")

    def test_generate_vo_practice_script_tool_pushes_to_booth(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"title": "Dawn at the Gate"})

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.generate_vo_practice_script("anime", load_into_booth=True))

        self.assertTrue(res["loaded_into_booth"])
        self.assertEqual(res["script"]["genre"], "anime")
        call_args, call_kwargs = mock_rpc.await_args
        self.assertEqual(call_args[0], "vo.set_script")
        self.assertEqual(call_args[1]["title"], "Dawn at the Gate")
        self.assertEqual(call_kwargs, {"timeout": 15.0})

    def test_build_vo_demo_reel_layout_tool(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"voice_track_index": 1, "bed_track_index": 2})

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.build_vo_demo_reel_layout(
                voice_track_index=1, bed_track_index=2, duck_threshold_db=-99.0, duck_ratio=99.0))

        self.assertEqual(res["bed_track_index"], 2)
        mock_rpc.assert_awaited_once_with(
            "vo.build_demo_reel_layout",
            {"voice_track_index": 1, "duck_threshold_db": -60.0, "duck_ratio": 20.0,
             "bed_track_index": 2},
            timeout=15.0,
        )

    def test_forge_game_sfx_batch_tool(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"count": 4})

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.forge_game_sfx_batch(
                "ui_click", count=99, seed=7, output_path="/tmp/sfx",
                duration_sec=99.0, peak_db=99.0))

        self.assertEqual(res["count"], 4)
        mock_rpc.assert_awaited_once_with(
            "game.forge_sfx_batch",
            {"category": "ui_click", "count": 16, "seed": 7, "output_dir": "/tmp/sfx",
             "prefix": "", "duration_sec": 10.0, "peak_db": 0.0,
             "import_first_to_track": -1},
            timeout=30.0,
        )

    def test_export_seamless_game_loop_tool(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"output_file": "C:/tmp/loop.wav"})

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.export_seamless_game_loop(
                start_beat=1.0, end_beat=9.0, output_path="C:/tmp/loop.wav",
                tail_seconds=99.0, target_lufs=-99.0))

        self.assertEqual(res["output_file"], "C:/tmp/loop.wav")
        mock_rpc.assert_awaited_once_with(
            "audio.export_seamless_loop",
            {"start_beat": 1.0, "end_beat": 9.0, "output_path": "C:/tmp/loop.wav",
             "tail_seconds": 5.0, "target_lufs": -40.0},
            timeout=30.0,
        )

    def test_wo023_engine_and_exporter_sources(self) -> None:
        root = _MCP_DIR.parent
        engine = (root / "src" / "SfxForgeEngine.h").read_text(encoding="utf-8")
        for marker in ("gunshotTactical", "breachExplosion", "footstepSurface", "uiClick",
                       "meleeImpact", "doorBreach", "sciFiPulse", "ambientDrone",
                       "generateSfxVariations", "withBitsPerSample (16)"):
            self.assertIn(marker, engine)
        exporter = (root / "src" / "MixdownExporter.h").read_text(encoding="utf-8")
        self.assertIn("exportSeamlessLoopWav", exporter)
        self.assertIn("targetLufs", exporter)

    def test_wo023_rpc_methods_registered(self) -> None:
        server = (_MCP_DIR.parent / "src" / "DawIpcServer.cpp").read_text(encoding="utf-8")
        for method in ("game.forge_sfx_batch", "audio.export_seamless_loop"):
            self.assertIn(method, server)
        self.assertIn("SfxForgeEngine.h", server)
        isolation = (_MCP_DIR.parent / "src" / "CoreIsolationCheck.cpp").read_text(encoding="utf-8")
        self.assertIn("SfxForgeEngine.h", isolation)

    def test_export_adaptive_bgm_stems_tool(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"count": 2})

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.export_adaptive_bgm_stems(
                0.0, 8.0, "C:/pack", {"calm": [0], "combat": [1, 2]}, -99.0))

        self.assertEqual(res["count"], 2)
        mock_rpc.assert_awaited_once_with(
            "game.export_adaptive_stems",
            {"start_beat": 0.0, "end_beat": 8.0, "output_dir": "C:/pack",
             "layers": {"calm": [0], "combat": [1, 2]}, "target_lufs": -40.0},
            timeout=30.0,
        )

    def test_process_vox_barks_tool(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"count": 3})

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.process_vox_barks(
                [" C:/a.wav ", "C:/b.wav"], "C:/pack", " hero_attack ", 99.0))

        self.assertEqual(res["count"], 3)
        mock_rpc.assert_awaited_once_with(
            "game.process_vox_barks",
            {"input_files": ["C:/a.wav", "C:/b.wav"], "output_dir": "C:/pack",
             "prefix": "hero_attack", "target_lufs": 0.0},
            timeout=30.0,
        )

    def test_export_godot_audio_pack_tool(self) -> None:
        srv = _load_daw_mcp_server()
        mock_rpc = AsyncMock(return_value={"asset_count": 12})

        with patch.object(srv, "send_daw_rpc", mock_rpc):
            res = asyncio.run(srv.export_godot_audio_pack("C:/pack", False, 128.0))

        self.assertEqual(res["asset_count"], 12)
        mock_rpc.assert_awaited_once_with(
            "game.export_godot_pack",
            {"output_dir": "C:/pack", "include_manifest": False, "bpm": 128.0},
            timeout=30.0,
        )

    def test_wo024_cpp_sources_declare_required_symbols(self) -> None:
        root = _MCP_DIR.parent
        exporter = (root / "src" / "GameAssetExporter.h").read_text(encoding="utf-8")
        for marker in ("exportAdaptiveBgmStems", "processVoxBarkBatch",
                       "writeAudioManifestJson", "audio_manifest.json",
                       "variation_group", "lufs"):
            self.assertIn(marker, exporter)
        dock = (root / "src" / "GameForgeComponent.cpp").read_text(encoding="utf-8")
        for method in ("game.forge_sfx_batch", "audio.export_seamless_loop",
                       "game.export_adaptive_stems", "game.export_godot_pack"):
            self.assertIn(method, dock)
        server = (root / "src" / "DawIpcServer.cpp").read_text(encoding="utf-8")
        for method in ("game.export_adaptive_stems", "game.process_vox_barks",
                       "game.export_godot_pack"):
            self.assertIn(method, server)

if __name__ == "__main__":
    unittest.main()
