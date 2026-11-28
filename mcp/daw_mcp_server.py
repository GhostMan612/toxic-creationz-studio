"""Toxic Creationz Studio AI co-producer bridge.

Out-of-process FastMCP server that speaks newline-delimited JSON-RPC 2.0 to the
DAW's native DawIpcServer on 127.0.0.1:9450. Start the DAW first, then run:

    pip install -r mcp/requirements.txt
    python mcp/daw_mcp_server.py
"""

import asyncio
import json
from pathlib import Path
import re
import sys
import time
from typing import Any

from mcp.server.fastmcp import FastMCP
from pydantic import BaseModel, Field

_MCP_DIR = str(Path(__file__).resolve().parent)
if _MCP_DIR not in sys.path:
    sys.path.insert(0, _MCP_DIR)

from ai_audio_worker import (
    generate_stem,
    generate_vo_practice_script as generate_vo_practice_script_worker,
    separate_stems,
)

mcp = FastMCP("Toxic Creationz Studio Co-Producer")

DAW_HOST = "127.0.0.1"
DAW_PORT = 9450

_rpc_counter = 0


def get_generated_stems_dir() -> Path:
    """Directory where out-of-process generated AI stems are stored."""
    return Path.home() / "Documents" / "Toxic Creationz Studio" / "GeneratedStems"


def get_separated_stems_dir() -> Path:
    """Directory where out-of-process separated stems are stored."""
    return Path.home() / "Documents" / "Toxic Creationz Studio" / "SeparatedStems"


class MidiNote(BaseModel):
    pitch: int = Field(..., ge=0, le=127, description="MIDI note number (60 = C4)")
    start_beat: float = Field(..., ge=0.0, description="Start position in beats from bar 1")
    length_beats: float = Field(..., gt=0.0, description="Duration in beats (1.0 = quarter note)")
    velocity: int = Field(100, ge=1, le=127, description="Note velocity (1-127)")


class AutomationPoint(BaseModel):
    beat: float = Field(..., ge=0.0, description="Timeline position in beats")
    value: float = Field(..., description="Parameter domain value (dB for volume/EQ, -1..1 for pan)")
    curve: float = Field(0.0, ge=-1.0, le=1.0, description="Tracktion curve curvature (-1..1, 0 = linear)")


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
    """Create a new track with an instrument: '4osc', 'tr808', 'sampler', 'tape', or 'none'."""
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
    armed: bool | None = None,
    input_device: str | None = None,
) -> dict[str, Any]:
    """Adjust mixer volume (-60 to +6 dB), pan (-1 to 1), mute, solo, record arm, or input device on a track."""
    params: dict[str, Any] = {"track_index": track_index}

    if volume_db is not None:
        params["volume_db"] = max(-60.0, min(6.0, volume_db))

    if pan is not None:
        params["pan"] = max(-1.0, min(1.0, pan))

    if mute is not None:
        params["mute"] = mute

    if solo is not None:
        params["solo"] = solo

    if input_device is not None:
        params["input_device"] = input_device

    if armed is not None:
        params["armed"] = armed

    return await send_daw_rpc("mixer.set_track", params)


@mcp.tool()
async def control_transport(
    action: str = "",
    bpm: float | None = None,
    beat: float | None = None,
    click: bool | None = None,
    loop: bool | None = None,
    loop_start_beat: float | None = None,
    loop_end_beat: float | None = None,
    count_in_bars: int | None = None,
) -> dict[str, Any]:
    """Control DAW transport: play/stop/record, BPM, playhead seek, metronome click, count-in, and loop range."""
    params: dict[str, Any] = {"action": action}

    if bpm is not None:
        params["bpm"] = max(40.0, min(240.0, bpm))

    if beat is not None:
        params["beat"] = max(0.0, beat)

    if click is not None:
        params["click"] = click

    if loop is not None:
        params["loop"] = loop

    if loop_start_beat is not None:
        params["loop_start_beat"] = max(0.0, loop_start_beat)

    if loop_end_beat is not None:
        params["loop_end_beat"] = max(1.0, loop_end_beat)

    if count_in_bars is not None:
        params["count_in_bars"] = 1 if count_in_bars > 0 else 0

    return await send_daw_rpc("transport.control", params)


@mcp.tool()
async def list_recording_inputs() -> dict[str, Any]:
    """List available Windows audio inputs and hardware MIDI input devices discovered by the DAW."""
    return await send_daw_rpc("recording.list_inputs")


@mcp.tool()
async def generate_bgm_arrangement(
    genre: str = "Cyberpunk",
    key: str = "Am",
    energy: int = 4,
    bars: int = 8,
    scale: str | None = None,
    seed: int | None = None,
    complexity: str | None = None,
    time_signature: str | None = None,
    bpm: float | None = None,
) -> dict[str, Any]:
    """Generate a multi-stem BGM arrangement with optional theory-engine controls."""
    params: dict[str, Any] = {"genre": genre, "key": key, "energy": energy, "bars": bars}
    if scale is not None:
        params["scale"] = scale.strip().lower()
    if seed is not None:
        params["seed"] = max(0, int(seed))
    if complexity is not None:
        params["complexity"] = complexity.strip().lower()
    if time_signature is not None:
        params["time_signature"] = time_signature.strip()
    if bpm is not None:
        params["bpm"] = max(30.0, min(300.0, float(bpm)))
    return await send_daw_rpc("bgm.generate", params)


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


@mcp.tool()
async def set_track_eq(
    track_index: int,
    low_db: float | None = None,
    mid_db: float | None = None,
    high_db: float | None = None,
) -> dict[str, Any]:
    """Insert or update the 3-band EQ (Low/Mid/High in dB, -18.0 to +18.0) on a DAW track."""
    params: dict[str, Any] = {"track_index": track_index}
    if low_db is not None:
        params["low_db"] = max(-18.0, min(18.0, low_db))
    if mid_db is not None:
        params["mid_db"] = max(-18.0, min(18.0, mid_db))
    if high_db is not None:
        params["high_db"] = max(-18.0, min(18.0, high_db))
    return await send_daw_rpc("mixer.set_eq", params)


@mcp.tool()
async def generate_ai_audio_stem(
    prompt: str,
    track_index: int = 0,
    start_beat: float = 0.0,
    duration_sec: float = 4.0,
    key: str = "Am",
    bpm: float = 124.0,
    backend: str = "auto",
    clip_name: str = "",
    auto_tempo: bool = True,
) -> dict[str, Any]:
    """Generate an AI WAV stem out-of-process and import it onto a DAW timeline track."""
    stems_dir = get_generated_stems_dir()
    stems_dir.mkdir(parents=True, exist_ok=True)

    slug = re.sub(r"[^a-zA-Z0-9]+", "_", prompt.strip()).strip("_")[:32] or "ai_stem"
    out_wav = stems_dir / f"{slug}_{int(time.time() * 1000)}.wav"

    gen_meta = await asyncio.to_thread(
        generate_stem,
        prompt,
        duration_sec,
        out_wav,
        backend,
        key,
        bpm,
    )

    import_params: dict[str, Any] = {
        "track_index": track_index,
        "file_path": gen_meta["output_path"],
        "start_beat": max(0.0, start_beat),
        "auto_tempo": auto_tempo,
        "clip_name": clip_name or slug,
    }
    import_res = await send_daw_rpc("audio.import_file", import_params, timeout=15.0)
    return {"generation": gen_meta, "import": import_res}


@mcp.tool()
async def write_track_automation(
    track_index: int,
    parameter: str,
    points: list[AutomationPoint],
    clear_existing: bool = True,
) -> dict[str, Any]:
    """Write automation curve points ('volume', 'pan', 'eq_low', 'eq_mid', 'eq_high') on a track."""
    return await send_daw_rpc(
        "automation.set_points",
        {
            "track_index": track_index,
            "parameter": parameter,
            "points": [p.model_dump() for p in points],
            "clear_existing": clear_existing,
        },
    )


@mcp.tool()
async def get_track_automation(
    track_index: int,
    parameter: str = "volume",
) -> dict[str, Any]:
    """Read automation curve points for a track parameter ('volume', 'pan', 'eq_low', 'eq_mid', 'eq_high')."""
    return await send_daw_rpc(
        "automation.get_points",
        {"track_index": track_index, "parameter": parameter},
    )


@mcp.tool()
async def configure_fx_send(
    track_index: int,
    send_db: float = -6.0,
    bus_index: int = 0,
    effect: str = "reverb",
    wet_level: float = 0.35,
) -> dict[str, Any]:
    """Route a track via Aux Send to an FX Return bus hosting built-in Reverb or Delay."""
    return await send_daw_rpc(
        "mixer.set_fx_send",
        {
            "track_index": track_index,
            "bus_index": max(0, min(7, bus_index)),
            "send_db": max(-60.0, min(6.0, send_db)),
            "effect": effect.lower().strip(),
            "wet_level": max(0.0, min(1.0, wet_level)),
        },
    )


@mcp.tool()
async def transcribe_audio_to_midi(
    track_index: int,
    clip_index: int = 0,
    target_track_index: int = -1,
    quantize_beats: float = 0.25,
    min_note_beats: float = 0.125,
    polyphonic: bool = False,
    max_voices: int = 4,
) -> dict[str, Any]:
    """Transcribe an audio clip into an editable MIDI clip (YIN mono or polyphonic peak tracker)."""
    params: dict[str, Any] = {
        "track_index": track_index,
        "clip_index": max(0, clip_index),
        "target_track_index": target_track_index,
        "quantize_beats": max(0.0, min(2.0, quantize_beats)),
        "min_note_beats": max(0.03125, min(2.0, min_note_beats)),
        "polyphonic": bool(polyphonic),
        "max_voices": max(1, min(8, int(max_voices))),
    }
    return await send_daw_rpc("audio.transcribe_to_midi", params, timeout=15.0)


@mcp.tool()
async def comp_track_takes(
    track_index: int,
    action: str = "list",
    take_index: int = -1,
    crossfade_ms: float = 10.0,
    start_beat: float = 0.0,
    end_beat: float = 4.0,
    lane_index: int = 0,
) -> dict[str, Any]:
    """List, select or flatten multi-take loop comps on a track."""
    params: dict[str, Any] = {
        "track_index": track_index,
        "action": action.lower().strip(),
        "take_index": take_index,
        "crossfade_ms": max(0.0, min(2000.0, crossfade_ms)),
        "start_beat": max(0.0, start_beat),
        "end_beat": max(0.0, end_beat),
        "lane_index": lane_index,
    }
    return await send_daw_rpc("recording.comp_takes", params, timeout=15.0)


@mcp.tool()
async def set_plugin_preset(
    track_index: int = 0,
    preset_name: str = "",
    plugin_index: int = 0,
) -> dict[str, Any]:
    """List or apply a factory sound preset (e.g. 'Reese Neuro Bass', 'Supersaw Anthem Lead') on a track plugin."""
    return await send_daw_rpc(
        "plugin.set_preset",
        {
            "track_index": max(0, track_index),
            "plugin_index": max(0, plugin_index),
            "preset_name": preset_name.strip(),
        },
    )


@mcp.tool()
async def add_rack_machine(
    machine_type: str = "4osc",
    rack_index: int = 0,
) -> dict[str, Any]:
    """Insert a synth, sampler, effect, or CV utility ('4osc', 'sampler', 'compressor', 'chorus', 'phaser', 'lowpass', 'pitchshift', 'reverb', 'delay', 'eq', 'cv_lfo', 'cv_wobble', 'cv_splitter', 'cv_envelope', 'cv_step', 'vocal_strip') into a modular rack."""
    return await send_daw_rpc(
        "rack.add_machine",
        {
            "rack_index": max(0, rack_index),
            "machine_type": machine_type.strip().lower(),
        },
    )


@mcp.tool()
async def add_rack_module(
    module_type: str = "cv_lfo",
    rack_index: int = 0,
    x: float = 0.25,
    y: float = 0.5,
) -> dict[str, Any]:
    """Insert a CV or hardware module into a modular rack.

    module_type: 'cv_lfo', 'cv_filter', 'cv_splitter', 'cv_envelope', 'cv_step',
    'vocal_strip', 'tr808', 'sampler', 'tape', or '4osc'. x/y are normalised 0..1 rack positions.
    """
    return await send_daw_rpc(
        "rack.add_module",
        {
            "rack_index": max(0, rack_index),
            "module_type": module_type.strip().lower(),
            "x": x,
            "y": y,
        },
    )


@mcp.tool()
async def generate_chord_progression(
    track_index: int = 0,
    key: str = "C",
    scale: str = "minor",
    progression: list[str] | None = None,
    start_beat: float = 0.0,
    beats_per_chord: float = 4.0,
    octave: int = 4,
    velocity: int = 96,
    voice_lead: bool = True,
    seed: int | None = None,
    bars: int | None = None,
    complexity: str | None = None,
    time_signature: str | None = None,
) -> dict[str, Any]:
    """Generate a Roman-numeral voice-led chord progression on a MIDI/instrument track."""
    params: dict[str, Any] = {
        "track_index": max(0, track_index),
        "key": key.strip() or "C",
        "scale": scale.strip().lower() or "minor",
        "progression": progression or ["i", "VI", "III", "VII"],
        "start_beat": max(0.0, start_beat),
        "beats_per_chord": max(0.25, min(32.0, beats_per_chord)),
        "octave": max(1, min(7, octave)),
        "velocity": max(1, min(127, velocity)),
        "voice_lead": voice_lead,
    }
    if seed is not None:
        params["seed"] = max(0, int(seed))
    if bars is not None:
        params["bars"] = max(8, min(64, int(bars)))
    if complexity is not None:
        params["complexity"] = complexity.strip().lower()
    if time_signature is not None:
        params["time_signature"] = time_signature.strip()
    return await send_daw_rpc("midi.generate_progression", params)


@mcp.tool()
async def quantize_track_to_scale(
    track_index: int = 0,
    key: str = "C",
    scale: str = "minor",
    grid_beats: float = 0.25,
    snap_pitch: bool = True,
    snap_timing: bool = True,
) -> dict[str, Any]:
    """Snap MIDI notes on a track into a musical scale and/or beat grid."""
    return await send_daw_rpc(
        "midi.quantize_scale",
        {
            "track_index": max(0, track_index),
            "key": key.strip() or "C",
            "scale": scale.strip().lower() or "minor",
            "grid_beats": max(0.03125, min(4.0, grid_beats)),
            "snap_pitch": snap_pitch,
            "snap_timing": snap_timing,
        },
    )


@mcp.tool()
async def auto_balance_mix_lufs(
    target_lufs: float = -14.0,
    max_gain_db: float = 6.0,
    min_gain_db: float = -18.0,
    headroom_db: float = -1.0,
) -> dict[str, Any]:
    """Gain-balance non-muted mixer tracks toward a target LUFS loudness with headroom clamping."""
    clamped_min = max(-60.0, min(6.0, min_gain_db))
    clamped_max = max(clamped_min, min(6.0, max_gain_db))
    return await send_daw_rpc(
        "mixer.auto_balance_lufs",
        {
            "target_lufs": max(-40.0, min(-3.0, target_lufs)),
            "max_gain_db": clamped_max,
            "min_gain_db": clamped_min,
            "headroom_db": max(-12.0, min(0.0, headroom_db)),
        },
        timeout=10.0,
    )


@mcp.tool()
async def duplicate_arrangement_section(
    start_beat: float,
    end_beat: float,
    dest_beat: float,
    track_indices: list[int] | None = None,
    mute_drums: bool = False,
) -> dict[str, Any]:
    """Duplicate MIDI and audio clip sections in [start_beat, end_beat) to dest_beat."""
    params: dict[str, Any] = {
        "start_beat": max(0.0, start_beat),
        "end_beat": max(0.25, end_beat),
        "dest_beat": max(0.0, dest_beat),
        "mute_drums": mute_drums,
    }
    if track_indices is not None:
        params["track_indices"] = [max(0, i) for i in track_indices]
    return await send_daw_rpc("arrangement.duplicate_section", params, timeout=10.0)


@mcp.tool()
async def separate_audio_stems(
    input_wav: str,
    import_to_daw: bool = True,
    start_beat: float = 0.0,
    backend: str = "auto",
) -> dict[str, Any]:
    """Separate an audio WAV into drums/bass/vocals/other stems via Demucs or offline splitter and optionally import into tracks."""
    src = Path(input_wav)
    slug = re.sub(r"[^a-zA-Z0-9]+", "_", src.stem).strip("_")[:32] or "mix"
    out_dir = get_separated_stems_dir() / f"{slug}_{int(time.time() * 1000)}"

    sep_meta = await asyncio.to_thread(separate_stems, src, out_dir, backend)
    imported: list[dict[str, Any]] = []

    if import_to_daw:
        clamped_start = max(0.0, start_beat)
        for stem_name in ("drums", "bass", "vocals", "other"):
            stem_path = sep_meta["stems"].get(stem_name)
            if not stem_path:
                continue
            track_res = await send_daw_rpc(
                "track.create",
                {"name": f"{slug} ({stem_name.capitalize()})", "plugin_type": "none"},
            )
            imp_res = await send_daw_rpc(
                "audio.import_file",
                {
                    "track_index": track_res["track_index"],
                    "file_path": stem_path,
                    "start_beat": clamped_start,
                    "clip_name": f"{slug}_{stem_name}",
                },
                timeout=15.0,
            )
            imported.append(imp_res)

    return {"separation": sep_meta, "imported": imported}


@mcp.tool()
async def analyze_vo_specs(
    file_path: str = "",
    track_index: int = 0,
    clip_index: int = 0,
    profile: str = "acx",
) -> dict[str, Any]:
    """Analyze a WAV (or the selected audio clip) against ACX or audition loudness specs."""
    selected = (profile or "acx").strip().lower()
    if selected not in ("acx", "audition"):
        selected = "acx"
    params: dict[str, Any] = {
        "file_path": file_path.strip(),
        "track_index": max(0, track_index),
        "clip_index": max(0, clip_index),
        "profile": selected,
    }
    return await send_daw_rpc("audio.analyze_vo_specs", params, timeout=15.0)


@mcp.tool()
async def export_abc_audition(
    track_index: int = 0,
    take_indices: list[int] | None = None,
    gap_seconds: float = 1.5,
    mono: bool = True,
    output_path: str = "",
) -> dict[str, Any]:
    """Concatenate A/B/C slate takes with clean gaps into a submission-ready WAV."""
    params: dict[str, Any] = {
        "track_index": max(0, track_index),
        "take_indices": take_indices or [0, 1, 2],
        "gap_seconds": min(10.0, max(0.0, float(gap_seconds))),
        "mono": bool(mono),
        "output_path": output_path.strip(),
    }
    return await send_daw_rpc("recording.export_abc_audition", params, timeout=15.0)


@mcp.tool()
async def punch_and_roll_splice(
    track_index: int = 0,
    prior_wav: str = "",
    new_take_wav: str = "",
    punch_seconds: float = 0.0,
    crossfade_ms: float = 15.0,
    output_path: str = "",
) -> dict[str, Any]:
    """Stitch the pre-punch portion of a prior WAV into the new take with an equal-power crossfade."""
    params: dict[str, Any] = {
        "track_index": max(0, track_index),
        "prior_wav": prior_wav.strip(),
        "new_take_wav": new_take_wav.strip(),
        "punch_seconds": max(0.0, float(punch_seconds)),
        "crossfade_ms": min(100.0, max(5.0, float(crossfade_ms))),
        "output_path": output_path.strip(),
    }
    return await send_daw_rpc("recording.punch_and_roll", params, timeout=15.0)


@mcp.tool()
async def load_vo_script(
    text: str,
    title: str = "VO Script",
    wpm: int = 140,
    adr_enabled: bool = False,
    adr_line_sec: float = 4.0,
) -> dict[str, Any]:
    """Load original script copy into the VO booth teleprompter and Edit state."""
    params: dict[str, Any] = {
        "title": title.strip() or "VO Script",
        "text": text,
        "wpm": min(220, max(80, int(wpm))),
        "adr_enabled": bool(adr_enabled),
        "adr_line_sec": min(15.0, max(1.0, float(adr_line_sec))),
    }
    return await send_daw_rpc("vo.set_script", params, timeout=15.0)


@mcp.tool()
async def generate_vo_practice_script(
    genre: str = "anime",
    emotion: str = "determined",
    duration_sec: float = 15.0,
    load_into_booth: bool = True,
) -> dict[str, Any]:
    """Generate copyright-free audition copy with 5-P direction and ADR windows."""
    script = await asyncio.to_thread(
        generate_vo_practice_script_worker, genre, emotion, duration_sec
    )
    if not load_into_booth:
        return {"script": script, "loaded_into_booth": False}

    loaded = await send_daw_rpc(
        "vo.set_script",
        {
            "title": script["title"],
            "text": script["text"],
            "wpm": script["wpm"],
            "adr_enabled": script["adr_enabled"],
            "adr_line_sec": script["adr_line_sec"],
        },
        timeout=15.0,
    )
    return {"script": script, "loaded_into_booth": True, "booth": loaded}


@mcp.tool()
async def build_vo_demo_reel_layout(
    voice_track_index: int = 0,
    bed_track_index: int | None = None,
    duck_threshold_db: float = -20.0,
    duck_ratio: float = 6.0,
) -> dict[str, Any]:
    """Wire a vocal booth strip on the voice track and a sidechain ducked music bed."""
    params: dict[str, Any] = {
        "voice_track_index": max(0, int(voice_track_index)),
        "duck_threshold_db": min(0.0, max(-60.0, float(duck_threshold_db))),
        "duck_ratio": min(20.0, max(1.0, float(duck_ratio))),
    }
    if bed_track_index is not None:
        params["bed_track_index"] = max(0, int(bed_track_index))
    return await send_daw_rpc("vo.build_demo_reel_layout", params, timeout=15.0)


@mcp.tool()
async def forge_game_sfx_batch(
    category: str = "gunshot_tactical",
    count: int = 6,
    seed: int = 42,
    output_path: str = "",
    prefix: str = "",
    duration_sec: float = 0.5,
    peak_db: float = -1.0,
    import_first_to_track: int = -1,
) -> dict[str, Any]:
    """Forge seeded, non-repetitive procedural game SFX takes into a WAV batch."""
    params: dict[str, Any] = {
        "category": category.strip().lower(),
        "count": min(16, max(1, int(count))),
        "seed": max(0, int(seed)),
        "output_dir": output_path.strip(),
        "prefix": prefix.strip(),
        "duration_sec": min(10.0, max(0.05, float(duration_sec))),
        "peak_db": min(0.0, max(-60.0, float(peak_db))),
        "import_first_to_track": int(import_first_to_track),
    }
    return await send_daw_rpc("game.forge_sfx_batch", params, timeout=30.0)


@mcp.tool()
async def export_seamless_game_loop(
    start_beat: float = 0.0,
    end_beat: float = 8.0,
    output_path: str = "",
    tail_seconds: float = 2.0,
    target_lufs: float = -14.0,
) -> dict[str, Any]:
    """Render a beat range with tail-wrapped, click-free looping and BS.1770 normalization."""
    params: dict[str, Any] = {
        "start_beat": float(start_beat),
        "end_beat": float(end_beat),
        "output_path": output_path.strip(),
        "tail_seconds": min(5.0, max(0.1, float(tail_seconds))),
        "target_lufs": min(-3.0, max(-40.0, float(target_lufs))),
    }
    return await send_daw_rpc("audio.export_seamless_loop", params, timeout=30.0)


@mcp.tool()
async def export_adaptive_bgm_stems(
    start_beat: float = 0.0,
    end_beat: float = 8.0,
    output_path: str = "",
    layers: dict[str, list[int]] | None = None,
    target_lufs: float = -14.0,
) -> dict[str, Any]:
    """Export phase-locked adaptive intensity BGM stems for Godot 4 crossfades."""
    params: dict[str, Any] = {
        "start_beat": float(start_beat),
        "end_beat": float(end_beat),
        "output_dir": output_path.strip(),
        "layers": layers or {"calm": [0], "combat": [1]},
        "target_lufs": min(-3.0, max(-40.0, float(target_lufs))),
    }
    return await send_daw_rpc("game.export_adaptive_stems", params, timeout=30.0)


@mcp.tool()
async def process_vox_barks(
    input_files: list[str],
    output_path: str = "",
    prefix: str = "actor_bark",
    target_lufs: float = -16.0,
) -> dict[str, Any]:
    """Trim, fade and normalize VOX bark WAVs to a -16 LUFS game-ready batch."""
    params: dict[str, Any] = {
        "input_files": [p.strip() for p in input_files],
        "output_dir": output_path.strip(),
        "prefix": prefix.strip(),
        "target_lufs": min(0.0, max(-60.0, float(target_lufs))),
    }
    return await send_daw_rpc("game.process_vox_barks", params, timeout=30.0)


@mcp.tool()
async def export_godot_audio_pack(
    output_path: str = "",
    include_manifest: bool = True,
    bpm: float = 120.0,
) -> dict[str, Any]:
    """Build a Godot 4 / asset-vault pack with audio_manifest.json."""
    params: dict[str, Any] = {
        "output_dir": output_path.strip(),
        "include_manifest": bool(include_manifest),
        "bpm": float(bpm),
    }
    return await send_daw_rpc("game.export_godot_pack", params, timeout=30.0)


@mcp.tool()
async def get_music_theory_catalog() -> dict[str, Any]:
    """Return all 12 keys, 16 scales/modes, 14 genre profiles and time signatures."""
    return await send_daw_rpc("theory.get_catalog", {})


@mcp.tool()
async def generate_song_arrangement(
    genre: str = "ambient_cinematic",
    key: str = "C",
    scale: str = "",
    bars: int = 16,
    bpm: float = 0.0,
    seed: int = 42,
    complexity: str = "standard",
    include_counter_melody: bool = True,
) -> dict[str, Any]:
    """Generate a seeded multi-section Drums/Bass/Chords/Lead(+Counter) song arrangement."""
    params: dict[str, Any] = {
        "genre": genre.strip().lower(),
        "key": key.strip() or "C",
        "scale": scale.strip().lower(),
        "bars": max(8, min(64, int(bars))),
        "bpm": max(0.0, min(300.0, float(bpm))),
        "seed": max(0, int(seed)),
        "complexity": complexity.strip().lower(),
        "include_counter_melody": bool(include_counter_melody),
    }
    return await send_daw_rpc("midi.generate_song_arrangement", params, timeout=30.0)


@mcp.tool()
async def set_rpg_scene(
    biome: str = "drowned_stars",
    mood_intensity: float = 0.35,
    crossfade_sec: float = 2.0,
) -> dict[str, Any]:
    """Crossfade the live RPG soundboard ambience and set encounter mood."""
    params: dict[str, Any] = {
        "biome": biome.strip().lower(),
        "mood_intensity": min(1.0, max(0.0, float(mood_intensity))),
        "crossfade_sec": min(5.0, max(0.5, float(crossfade_sec))),
    }
    return await send_daw_rpc("rpg.set_scene", params)


@mcp.tool()
async def trigger_rpg_sfx(sfx_name: str = "sword_clash", velocity: float = 0.9) -> dict[str, Any]:
    """Fire a one-shot procedural tabletop SFX on the live soundboard."""
    params: dict[str, Any] = {
        "sfx_name": sfx_name.strip().lower(),
        "velocity": min(1.0, max(0.0, float(velocity))),
    }
    return await send_daw_rpc("rpg.trigger_sfx", params)


@mcp.tool()
async def get_rpg_soundboard_state() -> dict[str, Any]:
    """Read the active RPG biome, mood and available biomes/SFX."""
    return await send_daw_rpc("rpg.get_state", {})


if __name__ == "__main__":
    mcp.run()
