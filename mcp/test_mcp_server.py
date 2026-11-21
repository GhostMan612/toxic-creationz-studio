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


if __name__ == "__main__":
    unittest.main()
