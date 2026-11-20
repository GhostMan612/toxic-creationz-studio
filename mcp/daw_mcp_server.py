"""Toxic Creationz Studio AI co-producer bridge.

Out-of-process FastMCP server that speaks newline-delimited JSON-RPC 2.0 to the
DAW's native DawIpcServer on 127.0.0.1:9450. Start the DAW first, then run:

    pip install -r mcp/requirements.txt
    python mcp/daw_mcp_server.py
"""

import asyncio
import json
from typing import Any

from mcp.server.fastmcp import FastMCP
from pydantic import BaseModel, Field

mcp = FastMCP("Toxic Creationz Studio Co-Producer")

DAW_HOST = "127.0.0.1"
DAW_PORT = 9450

_rpc_counter = 0


class MidiNote(BaseModel):
    pitch: int = Field(..., ge=0, le=127, description="MIDI note number (60 = C4)")
    start_beat: float = Field(..., ge=0.0, description="Start position in beats from bar 1")
    length_beats: float = Field(..., gt=0.0, description="Duration in beats (1.0 = quarter note)")
    velocity: int = Field(100, ge=1, le=127, description="Note velocity (1-127)")


async def send_daw_rpc(
    method: str, params: dict[str, Any] | None = None, timeout: float = 5.0
) -> dict[str, Any]:
    global _rpc_counter

    _rpc_counter += 1
    req_id = _rpc_counter

    payload = {
        "jsonrpc": "2.0",
        "id": req_id,
        "method": method,
        "params": params or {},
    }

    reader, writer = await asyncio.open_connection(DAW_HOST, DAW_PORT)

    try:
        writer.write((json.dumps(payload) + "\n").encode("utf-8"))
        await writer.drain()

        raw_line = await asyncio.wait_for(reader.readline(), timeout=timeout)

        if not raw_line:
            raise RuntimeError("DAW closed IPC connection unexpectedly.")

        response = json.loads(raw_line.decode("utf-8"))

        if "error" in response:
            raise RuntimeError(
                f"DAW RPC Error {response['error']['code']}: {response['error']['message']}"
            )

        return response["result"]

    finally:
        writer.close()
        await writer.wait_closed()


@mcp.tool()
async def get_session_state() -> dict[str, Any]:
    """Inspect current DAW tempo (BPM), playback state, rack count, and all audio/MIDI tracks."""
    return await send_daw_rpc("daw.get_state")


@mcp.tool()
async def create_instrument_track(name: str, instrument: str = "4osc") -> dict[str, Any]:
    """Create a new track in the DAW loaded with an instrument ('4osc' or 'none')."""
    return await send_daw_rpc("track.create", {"name": name, "plugin_type": instrument})


@mcp.tool()
async def write_piano_roll_notes(
    track_index: int, notes: list[MidiNote], clip_name: str = "AI Pattern"
) -> dict[str, Any]:
    """Write MIDI notes into the piano roll of the specified track index."""
    serialized = [n.model_dump() for n in notes]

    return await send_daw_rpc(
        "midi.add_notes",
        {"track_index": track_index, "clip_name": clip_name, "notes": serialized},
    )


@mcp.tool()
async def get_piano_roll_notes(
    track_index: int, start_beat: float = 0.0, end_beat: float = 64.0
) -> dict[str, Any]:
    """Read existing MIDI notes from a track within [start_beat, end_beat]."""
    return await send_daw_rpc(
        "midi.get_notes",
        {"track_index": track_index, "start_beat": start_beat, "end_beat": end_beat},
    )


@mcp.tool()
async def set_mixer_channel(
    track_index: int,
    volume_db: float | None = None,
    pan: float | None = None,
    mute: bool | None = None,
    solo: bool | None = None,
) -> dict[str, Any]:
    """Adjust mixer volume (in dB, -60 to +6), stereo pan (-1.0 to 1.0), mute, or solo on a track."""
    params: dict[str, Any] = {"track_index": track_index}

    if volume_db is not None:
        params["volume_db"] = max(-60.0, min(6.0, volume_db))

    if pan is not None:
        params["pan"] = max(-1.0, min(1.0, pan))

    if mute is not None:
        params["mute"] = mute

    if solo is not None:
        params["solo"] = solo

    return await send_daw_rpc("mixer.set_track", params)


@mcp.tool()
async def control_transport(
    action: str = "",
    bpm: float | None = None,
    beat: float | None = None,
) -> dict[str, Any]:
    """Start ('play') or stop ('stop') DAW playback, update BPM, or seek to a beat position."""
    params: dict[str, Any] = {"action": action}

    if bpm is not None:
        params["bpm"] = max(40.0, min(240.0, bpm))

    if beat is not None:
        params["beat"] = max(0.0, beat)

    return await send_daw_rpc("transport.control", params)


@mcp.tool()
async def generate_bgm_arrangement(
    genre: str = "Cyberpunk",
    key: str = "Am",
    energy: int = 4,
    bars: int = 8,
) -> dict[str, Any]:
    """Generate a multi-stem time-stretched and pitch-shifted riff BGM arrangement."""
    return await send_daw_rpc(
        "bgm.generate",
        {"genre": genre, "key": key, "energy": energy, "bars": bars},
    )


@mcp.tool()
async def process_audio_clip(
    track_index: int,
    operation: str = "silence",
    clip_index: int = 0,
    start_norm: float = 0.0,
    end_norm: float = 1.0,
) -> dict[str, Any]:
    """Apply non-destructive bounce-to-take DSP (normalize/reverse/fade_in/fade_out/silence) to a clip."""
    return await send_daw_rpc(
        "audio.process_clip",
        {
            "track_index": track_index,
            "clip_index": clip_index,
            "operation": operation,
            "start_norm": max(0.0, min(1.0, start_norm)),
            "end_norm": max(0.0, min(1.0, end_norm)),
        },
    )


@mcp.tool()
async def get_modular_racks() -> dict[str, Any]:
    """Inspect all Reason-style modular racks, their plugin nodes/pins, and active Audio/MIDI/CV patch cables."""
    return await send_daw_rpc("rack.get_state")


@mcp.tool()
async def patch_modular_rack(
    action: str,
    source_id: int,
    source_pin: int,
    dest_id: int,
    dest_pin: int,
    rack_index: int = 0,
) -> dict[str, Any]:
    """Connect ('connect') or unplug ('disconnect') a virtual Audio/MIDI/CV cable in a modular rack (use ID 0 for Rack I/O)."""
    return await send_daw_rpc(
        "rack.patch",
        {
            "rack_index": rack_index,
            "action": action,
            "source_id": source_id,
            "source_pin": source_pin,
            "dest_id": dest_id,
            "dest_pin": dest_pin,
        },
    )


@mcp.tool()
async def import_audio_stem(
    track_index: int,
    file_path: str,
    start_beat: float = 0.0,
    length_beats: float = 0.0,
    clip_name: str = "",
    auto_tempo: bool = False,
    pitch_shift: float = 0.0,
) -> dict[str, Any]:
    """Import an external WAV stem (e.g. from Arioso/Suno/Lyria) onto a timeline track without blocking playback."""
    params: dict[str, Any] = {
        "track_index": track_index,
        "file_path": file_path,
        "start_beat": max(0.0, start_beat),
        "auto_tempo": auto_tempo,
        "pitch_shift": pitch_shift,
    }
    if length_beats > 0.0:
        params["length_beats"] = length_beats
    if clip_name:
        params["clip_name"] = clip_name
    return await send_daw_rpc("audio.import_file", params)


@mcp.tool()
async def export_master_wav(
    output_path: str,
    start_beat: float | None = None,
    end_beat: float | None = None,
) -> dict[str, Any]:
    """Render a stereo master mixdown WAV of the current DAW session."""
    params: dict[str, Any] = {"output_path": output_path}
    if start_beat is not None:
        params["start_beat"] = max(0.0, start_beat)
    if end_beat is not None:
        params["end_beat"] = max(0.0, end_beat)
    return await send_daw_rpc("audio.export_wav", params, timeout=60.0)


if __name__ == "__main__":
    mcp.run()
