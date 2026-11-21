"""Out-of-process hybrid AI audio stem generator for Toxic Creationz Studio.

Tries an optional local `arioso` installation when installed/requested and
seamlessly falls back to a deterministic zero-dependency stereo 44.1 kHz
16-bit PCM synthesizer so offline workflows and headless CI stay reliable.
"""

from __future__ import annotations

import argparse
from array import array
import hashlib
import math
from pathlib import Path
import re
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
) -> dict[str, Any] | None:
    """Best-effort optional Arioso integration; returns None if unavailable."""
    try:
        import importlib

        arioso = importlib.import_module("arioso")
    except Exception:
        return None

    output_path.parent.mkdir(parents=True, exist_ok=True)
    clamped_dur = max(0.5, min(60.0, float(duration_sec)))

    for fn_name in ("generate_wav", "generate_audio", "generate"):
        fn = getattr(arioso, fn_name, None)
        if callable(fn):
            try:
                fn(prompt=prompt, duration=clamped_dur, output_path=str(output_path))
                if output_path.is_file() and output_path.stat().st_size > 44:
                    return {
                        "backend": "arioso",
                        "output_path": str(output_path.resolve()),
                        "sample_rate": 44100,
                        "channels": 2,
                        "duration_sec": clamped_dur,
                        "prompt": prompt,
                    }
            except Exception:
                return None

    return None


def _synthesize_fallback_wav(
    prompt: str,
    duration_sec: float,
    output_path: Path,
    key: str = "Am",
    bpm: float = 124.0,
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
) -> dict[str, Any]:
    """Generate a stereo AI stem WAV file via Arioso or the deterministic synth fallback."""
    out_path = Path(output_path)
    norm_backend = backend.lower().strip()

    if norm_backend in ("auto", "arioso"):
        arioso_result = _try_arioso_generate(prompt, duration_sec, out_path)
        if arioso_result is not None:
            return arioso_result
        if norm_backend == "arioso":
            raise RuntimeError("Optional 'arioso' package is not installed or unavailable.")
    elif norm_backend != "fallback":
        raise ValueError(f"Unsupported backend: {backend!r}")

    return _synthesize_fallback_wav(
        prompt=prompt,
        duration_sec=duration_sec,
        output_path=out_path,
        key=key,
        bpm=bpm,
    )


def _main() -> int:
    parser = argparse.ArgumentParser(description="Toxic Creationz Studio AI audio stem worker")
    parser.add_argument("--prompt", required=True, help="Text prompt describing the musical stem")
    parser.add_argument("--duration", type=float, default=4.0, help="Duration in seconds (0.5-60)")
    parser.add_argument("--output", required=True, help="Destination WAV file path")
    parser.add_argument("--backend", default="auto", choices=["auto", "arioso", "fallback"])
    parser.add_argument("--key", default="Am", help="Musical key (e.g. Am, Cm, F#m)")
    parser.add_argument("--bpm", type=float, default=124.0, help="Tempo in BPM (40-240)")

    args = parser.parse_args()
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
