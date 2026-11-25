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

from ai_audio_worker import generate_stem, parse_root_midi


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


if __name__ == "__main__":
    unittest.main()
