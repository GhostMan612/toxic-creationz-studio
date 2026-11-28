"""Out-of-process hybrid AI audio stem generator & separator for Toxic Creationz Studio.

Tries optional local/cloud integrations (Arioso, Suno, ElevenLabs, Demucs) when
installed/configured and seamlessly falls back to deterministic zero-dependency
stereo 44.1 kHz 16-bit PCM synthesis and spectral stem separation so offline
workflows and headless CI stay reliable.
"""

from __future__ import annotations

import argparse
from array import array
import hashlib
import importlib
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import wave
from typing import Any

_NOTE_OFFSETS: dict[str, int] = {
    "c": 0,
    "c#": 1,
    "db": 1,
    "d": 2,
    "d#": 3,
    "eb": 3,
    "e": 4,
    "f": 5,
    "f#": 6,
    "gb": 6,
    "g": 7,
    "g#": 8,
    "ab": 8,
    "a": 9,
    "a#": 10,
    "bb": 10,
    "b": 11,
}


def parse_root_midi(key: str) -> tuple[int, bool]:
    """Parse a musical key string (e.g. 'Am', 'F#m', 'Bb') into (midi_root, is_minor)."""
    cleaned = key.strip().lower()
    if not cleaned:
        return (45, True)

    match = re.match(r"^([a-g][#b]?)(.*)$", cleaned)
    if match is None:
        return (45, True)

    note_token, suffix = match.group(1), match.group(2).strip()
    pitch_class = _NOTE_OFFSETS.get(note_token, 9)
    is_minor = "maj" not in suffix and ("m" in suffix or "min" in suffix)
    return (36 + pitch_class, is_minor)


def _midi_to_hz(midi_note: int) -> float:
    return 440.0 * (2.0 ** ((midi_note - 69) / 12.0))


def _try_arioso_generate(
    prompt: str, duration_sec: float, output_path: Path
) -> tuple[dict[str, Any] | None, str]:
    """Best-effort optional Arioso integration; returns (result_or_None, reason)."""
    try:
        import importlib

        arioso = importlib.import_module("arioso")
    except Exception:
        return (None, "arioso package not installed")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    clamped_dur = max(0.5, min(60.0, float(duration_sec)))

    for fn_name in ("generate_wav", "generate_audio", "generate"):
        fn = getattr(arioso, fn_name, None)
        if callable(fn):
            try:
                fn(prompt=prompt, duration=clamped_dur, output_path=str(output_path))
                if output_path.is_file() and output_path.stat().st_size > 44:
                    return (
                        {
                            "backend": "arioso",
                            "backend_used": "arioso",
                            "fallback_reason": "",
                            "output_path": str(output_path.resolve()),
                            "sample_rate": 44100,
                            "channels": 2,
                            "duration_sec": clamped_dur,
                            "prompt": prompt,
                        },
                        "",
                    )
            except Exception as exc:
                return (None, f"arioso invocation failed: {exc}")

    return (None, "arioso entry point unavailable")


def _try_suno_generate(
    prompt: str, duration_sec: float, output_path: Path
) -> tuple[dict[str, Any] | None, str]:
    """Optional lazy Suno adapter requiring SUNO_API_KEY + suno package."""
    if not os.environ.get("SUNO_API_KEY", "").strip():
        return (None, "SUNO_API_KEY not set")
    try:
        import importlib

        suno_mod = importlib.import_module("suno")
    except Exception:
        return (None, "suno package not installed")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    clamped_dur = max(0.5, min(60.0, float(duration_sec)))
    for fn_name in ("generate_wav", "generate"):
        fn = getattr(suno_mod, fn_name, None)
        if callable(fn):
            try:
                fn(prompt=prompt, duration=clamped_dur, output_path=str(output_path))
                if output_path.is_file() and output_path.stat().st_size > 44:
                    return (
                        {
                            "backend": "suno",
                            "backend_used": "suno",
                            "fallback_reason": "",
                            "output_path": str(output_path.resolve()),
                            "sample_rate": 44100,
                            "channels": 2,
                            "duration_sec": clamped_dur,
                            "prompt": prompt,
                        },
                        "",
                    )
            except Exception as exc:
                return (None, f"suno invocation failed: {exc}")
    return (None, "suno entry point unavailable")


def _try_elevenlabs_generate(
    prompt: str, duration_sec: float, output_path: Path
) -> tuple[dict[str, Any] | None, str]:
    """Optional lazy ElevenLabs adapter requiring ELEVENLABS_API_KEY + elevenlabs package."""
    if not os.environ.get("ELEVENLABS_API_KEY", "").strip():
        return (None, "ELEVENLABS_API_KEY not set")
    try:
        import importlib

        el_mod = importlib.import_module("elevenlabs")
    except Exception:
        return (None, "elevenlabs package not installed")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    clamped_dur = max(0.5, min(60.0, float(duration_sec)))
    for fn_name in ("generate_sound_effect", "generate"):
        fn = getattr(el_mod, fn_name, None)
        if callable(fn):
            try:
                audio_bytes = fn(text=prompt, duration_seconds=clamped_dur)
                if isinstance(audio_bytes, (bytes, bytearray)) and len(audio_bytes) > 44:
                    output_path.write_bytes(bytes(audio_bytes))
                    return (
                        {
                            "backend": "elevenlabs",
                            "backend_used": "elevenlabs",
                            "fallback_reason": "",
                            "output_path": str(output_path.resolve()),
                            "sample_rate": 44100,
                            "channels": 2,
                            "duration_sec": clamped_dur,
                            "prompt": prompt,
                        },
                        "",
                    )
            except Exception as exc:
                return (None, f"elevenlabs invocation failed: {exc}")
    return (None, "elevenlabs entry point unavailable")


def _synthesize_fallback_wav(
    prompt: str,
    duration_sec: float,
    output_path: Path,
    key: str = "Am",
    bpm: float = 124.0,
    fallback_reason: str = "",
) -> dict[str, Any]:
    """Synthesize a stereo 44.1 kHz 16-bit PCM chord + tempo-synced arp WAV."""
    sample_rate = 44100
    clamped_dur = max(0.5, min(60.0, float(duration_sec)))
    clamped_bpm = max(40.0, min(240.0, float(bpm)))
    total_frames = int(round(clamped_dur * sample_rate))

    root_midi, is_minor = parse_root_midi(key)
    third_offset = 3 if is_minor else 4
    scale_notes = (
        root_midi,
        root_midi + third_offset,
        root_midi + 7,
        root_midi + 12,
        root_midi + 12 + third_offset,
        root_midi + 19,
    )

    f_bass = _midi_to_hz(root_midi)
    f_third = _midi_to_hz(root_midi + third_offset)
    f_fifth = _midi_to_hz(root_midi + 7)
    arp_freqs = [_midi_to_hz(n + 12) for n in scale_notes]

    digest = hashlib.sha256(prompt.encode("utf-8")).digest()
    detune_ratio = 1.0 + ((digest[0] % 9) - 4) * 0.0008
    arp_step_offset = digest[1] % len(arp_freqs)

    samples_per_eighth = max(1.0, (60.0 / clamped_bpm) * 0.5 * sample_rate)
    fade_samples = min(total_frames // 2, int(0.020 * sample_rate))

    two_pi_over_sr = (2.0 * math.pi) / sample_rate
    w_bass = f_bass * two_pi_over_sr
    w_third_l = f_third * two_pi_over_sr
    w_third_r = (f_third * detune_ratio) * two_pi_over_sr
    w_fifth = f_fifth * two_pi_over_sr

    pcm = array("h", [0]) * (total_frames * 2)
    arp_phase = 0.0

    for i in range(total_frames):
        step_pos = i / samples_per_eighth
        step_idx = int(step_pos)
        step_frac = step_pos - step_idx
        arp_freq = arp_freqs[(step_idx + arp_step_offset) % len(arp_freqs)]
        arp_phase += arp_freq * two_pi_over_sr
        arp_env = math.exp(-5.0 * step_frac)

        amp_env = 1.0
        if fade_samples > 0:
            if i < fade_samples:
                amp_env = i / fade_samples
            elif i >= total_frames - fade_samples:
                amp_env = (total_frames - 1 - i) / fade_samples

        bass_val = 0.30 * math.sin(w_bass * i) + 0.08 * math.sin(2.0 * w_bass * i)
        pad_l = 0.18 * math.sin(w_third_l * i) + 0.14 * math.sin(w_fifth * i)
        pad_r = 0.18 * math.sin(w_third_r * i) + 0.14 * math.sin(w_fifth * i)

        pan = 0.5 + 0.35 * math.sin(0.8 * step_idx)
        arp_val = 0.22 * arp_env * math.sin(arp_phase)

        left = max(-0.85, min(0.85, amp_env * (bass_val + pad_l + arp_val * (1.0 - pan))))
        right = max(-0.85, min(0.85, amp_env * (bass_val + pad_r + arp_val * pan)))

        idx2 = i * 2
        pcm[idx2] = int(left * 32767.0)
        pcm[idx2 + 1] = int(right * 32767.0)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(output_path), "wb") as wf:
        wf.setnchannels(2)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm.tobytes())

    return {
        "backend": "fallback_synth",
        "backend_used": "fallback_synth",
        "fallback_reason": fallback_reason,
        "output_path": str(output_path.resolve()),
        "sample_rate": sample_rate,
        "channels": 2,
        "duration_sec": clamped_dur,
        "key": key,
        "bpm": clamped_bpm,
        "prompt": prompt,
    }


def generate_stem(
    prompt: str,
    duration_sec: float,
    output_path: str | Path,
    backend: str = "auto",
    key: str = "Am",
    bpm: float = 124.0,
    reference_wav: str | Path | None = None,
) -> dict[str, Any]:
    """Generate a stereo AI stem WAV file via optional cloud/local backends or deterministic synth fallback."""
    out_path = Path(output_path)
    norm_backend = backend.lower().strip()
    reasons: list[str] = []

    if reference_wav is not None:
        from audio_identify_worker import identify_clip

        info = identify_clip(reference_wav)
        if info.get("bpm"):
            bpm = float(info["bpm"])
        if info.get("key"):
            key = str(info["key"])
        family = (info.get("instruments") or [{}])[0].get("family", "unknown")
        prompt = f"{prompt} (conditioned by {family})"
        reasons.append(f"reference_wav={family}")

    if norm_backend in ("auto", "arioso"):
        res, reason = _try_arioso_generate(prompt, duration_sec, out_path)
        if res is not None:
            return res
        if reason:
            reasons.append(reason)
        if norm_backend == "auto":
            for adapter in (_try_suno_generate, _try_elevenlabs_generate):
                res, reason = adapter(prompt, duration_sec, out_path)
                if res is not None:
                    return res
                if reason:
                    reasons.append(reason)
    elif norm_backend == "suno":
        res, reason = _try_suno_generate(prompt, duration_sec, out_path)
        if res is not None:
            return res
        reasons.append(reason)
    elif norm_backend == "elevenlabs":
        res, reason = _try_elevenlabs_generate(prompt, duration_sec, out_path)
        if res is not None:
            return res
        reasons.append(reason)
    elif norm_backend in ("fallback", "offline"):
        reasons.append(f"explicit {norm_backend} backend selected")
    else:
        raise ValueError(f"Unsupported backend: {backend!r}")

    return _synthesize_fallback_wav(
        prompt=prompt,
        duration_sec=duration_sec,
        output_path=out_path,
        key=key,
        bpm=bpm,
        fallback_reason="; ".join(reasons),
    )


def generate_arrangement(
    prompt: str,
    output_dir: str | Path,
    key: str = "Am",
    bpm: float = 124.0,
    style: str = "trap",
    stems: tuple[str, ...] = ("drums", "bass", "lead", "pad"),
    reference_wav: str | Path | None = None,
) -> dict[str, Any]:
    """Generate one deterministic offline WAV per requested stem."""
    root = Path(output_dir)
    root.mkdir(parents=True, exist_ok=True)
    generated: list[dict[str, Any]] = []
    for stem in stems:
        out = root / f"{stem}.wav"
        result = generate_stem(
            prompt=f"{style} {stem}: {prompt}",
            duration_sec=4.0,
            output_path=out,
            backend="fallback",
            key=key,
            bpm=bpm,
            reference_wav=reference_wav,
        )
        generated.append({"stem": stem, "file_path": str(out), "result": result})
    return {"output_dir": str(root), "stems": generated}


def _read_stereo_wav(wav_path: Path) -> tuple[list[float], list[float], int]:
    """Read a PCM WAV file and return (left, right, sample_rate) in [-1, 1]."""
    with wave.open(str(wav_path), "rb") as wf:
        n_channels = wf.getnchannels()
        sampwidth = wf.getsampwidth()
        sample_rate = wf.getframerate()
        n_frames = wf.getnframes()
        raw = wf.readframes(n_frames)

    if sampwidth != 2:
        raise ValueError(f"Only 16-bit PCM WAV input is supported by stdlib splitter (got {sampwidth * 8}-bit).")

    samples = array("h")
    samples.frombytes(raw)
    inv_scale = 1.0 / 32768.0
    left: list[float] = []
    right: list[float] = []

    if n_channels == 1:
        left = [s * inv_scale for s in samples]
        right = list(left)
    else:
        for i in range(0, len(samples), n_channels):
            left.append(samples[i] * inv_scale)
            right.append(samples[i + 1] * inv_scale)

    return (left, right, sample_rate)


def _write_stereo_wav(wav_path: Path, left: list[float], right: list[float], sample_rate: int = 44100) -> None:
    wav_path.parent.mkdir(parents=True, exist_ok=True)
    pcm = array("h", [0]) * (len(left) * 2)
    for i, (lv, rv) in enumerate(zip(left, right)):
        l_clamped = max(-0.98, min(0.98, lv))
        r_clamped = max(-0.98, min(0.98, rv))
        pcm[i * 2] = int(l_clamped * 32767.0)
        pcm[i * 2 + 1] = int(r_clamped * 32767.0)

    with wave.open(str(wav_path), "wb") as wf:
        wf.setnchannels(2)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm.tobytes())


def _try_demucs_separate(input_path: Path, output_dir: Path) -> tuple[dict[str, str] | None, str]:
    """Optional lazy Demucs CLI/module separation."""
    demucs_exe = shutil.which("demucs")
    if demucs_exe is None:
        return (None, "demucs executable/package not installed")

    try:
        output_dir.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [demucs_exe, "-o", str(output_dir), str(input_path)],
            check=True,
            capture_output=True,
            timeout=180,
        )
        stem_map: dict[str, str] = {}
        for stem in ("drums", "bass", "vocals", "other"):
            found = list(output_dir.rglob(f"{stem}.wav"))
            if found:
                stem_map[stem] = str(found[0].resolve())
        if len(stem_map) == 4:
            return (stem_map, "")
        return (None, "demucs completed without all 4 stem WAVs")
    except Exception as exc:
        return (None, f"demucs separation failed: {exc}")


def separate_stems(
    input_wav: str | Path,
    output_dir: str | Path,
    backend: str = "auto",
) -> dict[str, Any]:
    """Separate a stereo WAV into 4 stems (drums, bass, vocals, other).

    Uses Demucs when available and falls back cleanly to a zero-dependency
    4-band / mid-side + transient offline splitter producing stereo 44.1 kHz
    16-bit WAV stems.
    """
    src_path = Path(input_wav)
    if not src_path.is_file():
        raise FileNotFoundError(f"Input WAV not found: {src_path}")

    out_root = Path(output_dir)
    out_root.mkdir(parents=True, exist_ok=True)
    norm_backend = backend.lower().strip()
    if norm_backend not in ("auto", "demucs", "offline", "fallback"):
        raise ValueError(f"Unsupported stem separation backend: {backend!r}")

    fallback_reason = ""
    if norm_backend in ("auto", "demucs"):
        demucs_stems, reason = _try_demucs_separate(src_path, out_root)
        if demucs_stems is not None:
            return {
                "backend": "demucs",
                "backend_used": "demucs",
                "fallback_reason": "",
                "output_dir": str(out_root.resolve()),
                "sample_rate": 44100,
                "stems": demucs_stems,
            }
        fallback_reason = reason
    else:
        fallback_reason = f"explicit {norm_backend} backend selected"

    left, right, src_sr = _read_stereo_wav(src_path)
    target_sr = 44100
    if src_sr != target_sr and len(left) > 1:
        new_len = max(1, int(round(len(left) * target_sr / float(src_sr))))
        resampled_l: list[float] = []
        resampled_r: list[float] = []
        ratio = (len(left) - 1) / float(max(1, new_len - 1))
        for i in range(new_len):
            pos = i * ratio
            idx = min(len(left) - 2, int(pos))
            frac = pos - idx
            resampled_l.append(left[idx] * (1.0 - frac) + left[idx + 1] * frac)
            resampled_r.append(right[idx] * (1.0 - frac) + right[idx + 1] * frac)
        left, right = resampled_l, resampled_r

    n_frames = len(left)
    lp_bass_a = 1.0 - math.exp((-2.0 * math.pi * 240.0) / target_sr)
    lp_vox_a = 1.0 - math.exp((-2.0 * math.pi * 3200.0) / target_sr)
    lp_env_fast = 1.0 - math.exp((-2.0 * math.pi * 120.0) / target_sr)
    lp_env_slow = 1.0 - math.exp((-2.0 * math.pi * 12.0) / target_sr)

    bass_l = [0.0] * n_frames
    bass_r = [0.0] * n_frames
    vox_l = [0.0] * n_frames
    vox_r = [0.0] * n_frames
    drums_l = [0.0] * n_frames
    drums_r = [0.0] * n_frames
    other_l = [0.0] * n_frames
    other_r = [0.0] * n_frames

    s_bass_l = s_bass_r = 0.0
    s_vox_l = s_vox_r = 0.0
    env_f = env_s = 0.0

    for i in range(n_frames):
        in_l = left[i]
        in_r = right[i]
        mid = 0.5 * (in_l + in_r)
        side = 0.5 * (in_l - in_r)

        s_bass_l += lp_bass_a * (in_l - s_bass_l)
        s_bass_r += lp_bass_a * (in_r - s_bass_r)

        s_vox_l += lp_vox_a * (in_l - s_vox_l)
        s_vox_r += lp_vox_a * (in_r - s_vox_r)
        mid_band_l = s_vox_l - s_bass_l
        mid_band_r = s_vox_r - s_bass_r

        amp = abs(mid)
        env_f += lp_env_fast * (amp - env_f)
        env_s += lp_env_slow * (amp - env_s)
        transient = max(0.0, min(1.0, (env_f - env_s) * 4.0 + 0.18))

        high_l = in_l - s_vox_l
        high_r = in_r - s_vox_r

        bass_l[i] = s_bass_l * 1.15
        bass_r[i] = s_bass_r * 1.15

        vox_l[i] = 0.72 * mid_band_l + 0.15 * mid
        vox_r[i] = 0.72 * mid_band_r + 0.15 * mid

        drums_l[i] = transient * (0.55 * s_bass_l + high_l + 0.25 * mid_band_l)
        drums_r[i] = transient * (0.55 * s_bass_r + high_r + 0.25 * mid_band_r)

        other_l[i] = (1.0 - 0.5 * transient) * (high_l + side * 0.85) + 0.20 * mid_band_l
        other_r[i] = (1.0 - 0.5 * transient) * (high_r - side * 0.85) + 0.20 * mid_band_r

    stem_buffers = {
        "drums": (drums_l, drums_r),
        "bass": (bass_l, bass_r),
        "vocals": (vox_l, vox_r),
        "other": (other_l, other_r),
    }

    stem_paths: dict[str, str] = {}
    for stem_name, (sl, sr_buf) in stem_buffers.items():
        dest = out_root / f"{stem_name}.wav"
        _write_stereo_wav(dest, sl, sr_buf, target_sr)
        stem_paths[stem_name] = str(dest.resolve())

    return {
        "backend": "offline_spectral_splitter",
        "backend_used": "offline_spectral_splitter",
        "fallback_reason": fallback_reason,
        "output_dir": str(out_root.resolve()),
        "sample_rate": target_sr,
        "stems": stem_paths,
    }


def transcribe_polyphonic_basic_pitch(
    input_wav: str | Path,
    output_midi: str | Path | None = None,
) -> dict[str, Any]:
    """Optional Spotify basic_pitch polyphonic transcription with an offline fallback.

    Zero extra dependencies are required: when basic_pitch is absent the call
    returns a structured ``cpp_polyphonic_fallback`` payload so headless CI and
    DAWs without Python ML packages stay green.
    """
    src = Path(input_wav)
    if not src.exists():
        raise FileNotFoundError(f"input_wav does not exist: {src}")

    midi_path = Path(output_midi) if output_midi else src.with_suffix(".mid")
    fallback: dict[str, Any] = {
        "status": "unavailable",
        "backend_used": "cpp_polyphonic_fallback",
        "fallback_reason": "",
        "input_wav": str(src),
        "output_midi": "",
        "notes": [],
        "note_count": 0,
    }

    try:
        basic_pitch = importlib.import_module("basic_pitch")
        inference = basic_pitch.inference
        model, model_output = inference.predict(str(src))

        note_events: list[dict[str, Any]] = []
        try:
            raw_notes = model_output[1]
            for event in raw_notes:
                note_events.append(
                    {
                        "start_beat": float(event[0]),
                        "end_beat": float(event[1]),
                        "pitch": int(event[2]),
                        "amplitude": float(event[3]),
                    }
                )
        except Exception:
            note_events = []

        written = ""
        try:
            inference.save(model_output, str(midi_path))
            if midi_path.exists():
                written = str(midi_path)
        except Exception:
            written = ""

        return {
            "status": "ok",
            "backend_used": "basic_pitch",
            "fallback_reason": "",
            "input_wav": str(src),
            "output_midi": written,
            "notes": note_events,
            "note_count": len(note_events),
        }
    except Exception as exc:
        fallback["fallback_reason"] = f"basic_pitch unavailable: {exc}"
        return fallback


def generate_vo_practice_script(
    genre: str = "anime",
    emotion: str = "determined",
    duration_sec: float = 15.0,
) -> dict[str, Any]:
    """Deterministic, copyright-free audition copy with 5-P direction cues.

    Zero external dependencies so booth CI and headless runs stay reproducible.
    The returned payload can be pushed directly into the DAW teleprompter with
    ``vo.set_script`` via the FastMCP ``generate_vo_practice_script`` tool.
    """
    selected = (genre or "anime").strip().lower()
    if selected not in {"anime", "cartoon", "commercial", "villain", "game_barks", "audiobook"}:
        selected = "anime"

    feeling = (emotion or "determined").strip().lower() or "determined"
    duration = min(120.0, max(5.0, float(duration_sec)))

    scripts: dict[str, tuple[str, str, list[str]]] = {
        "anime": (
            "Dawn at the Gate",
            "determined",
            [
                "The sky is still dark, but I can hear the city waking up.",
                "If tomorrow needs a hero, then tonight I start training.",
                "I am not running because I am afraid. I am running because I believe.",
                "When the bell rings, answer it. That is all a hero has to do.",
            ],
        ),
        "cartoon": (
            "The Last Pancake",
            "playful",
            [
                "Ladies and gentlemen, gather round for the greatest pancake in the universe.",
                "One side is golden, the other side is a mystery. That is called balance.",
                "Nobody flip it! I repeat, nobody flip it!",
                "Fine. I will flip it. But only because the syrup asked me to.",
            ],
        ),
        "commercial": (
            "Morning, Made Simple",
            "warm",
            [
                "There is a better way to start the day.",
                "One small ritual. One clear moment. Before the noise begins.",
                "Quiet coffee. Clean light. A plan you can actually keep.",
                "Morning, made simple. Starting tomorrow, starting now.",
            ],
        ),
        "villain": (
            "The Quiet Throne",
            "menacing",
            [
                "You expected a throne of fire. I built one of silence.",
                "Every door you opened, I had already closed from the inside.",
                "Do not mistake patience for mercy. I have simply been waiting.",
                "When the last light goes out, you will remember my name.",
            ],
        ),
        "game_barks": (
            "Training Ground Barks",
            "sharp",
            [
                "Move! We have no time to waste.",
                "Flank left, I will take the center.",
                "Shield up! Something is coming through the trees.",
                "Hold the line. Now!",
            ],
        ),
        "audiobook": (
            "The Lighthouse Keeper",
            "steady",
            [
                "Every evening, the keeper lit the lamp before the tide turned.",
                "He said the light was not for ships. It was for the people waiting on shore.",
                "The storm arrived without warning, and the glass began to shake.",
                "Still, he counted the seconds between each wave, and he read on.",
            ],
        ),
    }

    title, default_feeling, lines = scripts[selected]
    direction = {
        "pitch": "chest-forward, bright top notes" if selected in {"anime", "commercial"} else "low, grounded placement",
        "pace": "forward and energized" if selected in {"anime", "game_barks"} else "measured, deliberate",
        "placement": "mask resonance, close mic" if selected != "villain" else "low resonance, backed off the mic",
        "posture": "standing, ribs open, jaw loose",
        "personality": feeling,
    }

    cue_count = max(2, min(6, round(duration / 5.0)))
    span = duration / cue_count
    adr_windows = [
        {
            "index": i + 1,
            "start_sec": round(i * span, 2),
            "end_sec": round(min(duration, (i + 1) * span), 2),
            "cue": lines[i % len(lines)],
        }
        for i in range(cue_count)
    ]

    wpm = 140 if selected in {"anime", "cartoon", "game_barks"} else 125
    text = "\n".join(lines)
    return {
        "genre": selected,
        "emotion": feeling,
        "duration_sec": duration,
        "title": title,
        "text": text,
        "wpm": wpm,
        "adr_enabled": True,
        "adr_line_sec": round(span, 2),
        "direction": direction,
        "five_p": direction,
        "adr_windows": adr_windows,
        "default_emotion": default_feeling,
    }


def resolve_rpg_encounter_cue(event: str, setting: str = "cosmic_horror") -> dict[str, Any]:
    """Turn a natural tabletop event into a deterministic soundboard directive."""
    text = " ".join([event, setting]).strip().lower()
    if "boss" in text or "initiative" in text or "dragon" in text:
        return {
            "event": "boss_reveal",
            "biome": "boss_altar",
            "mood_intensity": 1.0,
            "sfx_name": "dragon_roar",
            "gm_voice": "heroic_narrator",
        }
    if "critical" in text or "hit" in text or "damage" in text:
        return {
            "event": "critical_hit",
            "biome": "haunted_crypt",
            "mood_intensity": 0.85,
            "sfx_name": "critical_hit",
            "gm_voice": "normal_gm",
        }
    if "spell" in text or "fireball" in text or "cast" in text:
        return {
            "event": "spell_cast",
            "biome": "ancient_forest",
            "mood_intensity": 0.6,
            "sfx_name": "fireball_cast",
            "gm_voice": "normal_gm",
        }
    if "tavern" in text or "rest" in text or "heal" in text:
        return {
            "event": "tavern_rest",
            "biome": "rainy_tavern",
            "mood_intensity": 0.15,
            "sfx_name": "healing_chime",
            "gm_voice": "normal_gm",
        }
    if "drowned" in text or "vault" in text or "cosmic" in text or "eldritch" in text:
        return {
            "event": "cosmic_dread",
            "biome": "drowned_stars",
            "mood_intensity": 0.55,
            "sfx_name": "eldritch_whisper",
            "gm_voice": "eldritch_monster",
        }
    return {
        "event": "exploration",
        "biome": "ancient_forest",
        "mood_intensity": 0.2,
        "sfx_name": "",
        "gm_voice": "normal_gm",
    }


def _main() -> int:
    parser = argparse.ArgumentParser(description="Toxic Creationz Studio AI audio stem worker")
    parser.add_argument("--action", default="generate", choices=["generate", "separate"])
    parser.add_argument("--prompt", default="", help="Text prompt describing the musical stem")
    parser.add_argument("--duration", type=float, default=4.0, help="Duration in seconds (0.5-60)")
    parser.add_argument("--output", default="", help="Destination WAV file or output directory path")
    parser.add_argument("--input", default="", help="Source WAV file path for stem separation")
    parser.add_argument(
        "--backend",
        default="auto",
        choices=["auto", "arioso", "suno", "elevenlabs", "demucs", "offline", "fallback"],
    )
    parser.add_argument("--key", default="Am", help="Musical key (e.g. Am, Cm, F#m)")
    parser.add_argument("--bpm", type=float, default=124.0, help="Tempo in BPM (40-240)")

    args = parser.parse_args()
    if args.action == "separate":
        if not args.input or not args.output:
            raise SystemExit("--input and --output are required for --action separate")
        sep_res = separate_stems(args.input, args.output, backend=args.backend)
        print(sep_res["output_dir"])
        return 0

    if not args.prompt or not args.output:
        raise SystemExit("--prompt and --output are required for --action generate")
    result = generate_stem(
        prompt=args.prompt,
        duration_sec=args.duration,
        output_path=args.output,
        backend=args.backend,
        key=args.key,
        bpm=args.bpm,
    )
    print(result["output_path"])
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
