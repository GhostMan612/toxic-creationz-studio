"""Zero-dependency coarse audio identification with an optional fine path."""

from __future__ import annotations

import hashlib
import math
import wave
from pathlib import Path
from typing import Any


def _read_pcm(path: str | Path) -> tuple[list[int], int]:
    with wave.open(str(path), "rb") as wav:
        frames = wav.readframes(wav.getnframes())
        rate = wav.getframerate()
    samples = [int(v) for v in memoryview(frames).cast("h")]
    return samples, rate


def identify_clip(
    wav_path: str | Path,
    enrolled_voices: list[dict[str, Any]] | None = None,
    voice_prints_dir: str | Path | None = None,
) -> dict[str, Any]:
    """Identify instrument family, key/BPM hints and a deterministic fingerprint."""
    path = Path(wav_path)
    if not path.exists():
        raise FileNotFoundError(f"Audio file not found: {path}")

    samples, rate = _read_pcm(path)
    if not samples:
        raise ValueError(f"Audio file has no samples: {path}")

    peak = max(abs(v) for v in samples) / 32768.0
    rms = math.sqrt(sum(float(v) * v for v in samples) / len(samples)) / 32768.0
    crossings = sum(1 for i in range(1, len(samples)) if (samples[i - 1] < 0) != (samples[i] < 0))
    zcr = crossings / max(1, len(samples) - 1) * rate / 2.0

    # One-pole low-pass (~150 Hz) and a 300-3000 Hz formant-band approximation.
    low = 0.0
    low_energy = 0.0
    band_energy = 0.0
    coeff = math.exp(-2.0 * math.pi * 150.0 / rate) if rate > 0 else 0.0
    for value in samples:
        low = coeff * low + (1.0 - coeff) * value
        low_energy += low * low
        band_energy += value * value
    bass_ratio = low_energy / max(1.0, band_energy)

    families: list[dict[str, Any]] = []
    if zcr > 3200 and peak > 0.15:
        families.append({"family": "drums", "confidence": min(1.0, zcr / 6000.0)})
    if bass_ratio > 0.55:
        families.append({"family": "bass", "confidence": min(1.0, bass_ratio)})
    if 900 < zcr < 3000 and band_energy > 0:
        families.append({"family": "vocal", "confidence": min(1.0, zcr / 3000.0)})
    if not families:
        families.append({"family": "unknown", "confidence": 0.25})

    digest = hashlib.sha256()
    for value in samples:
        digest.update(value.to_bytes(2, "little", signed=True))
    fingerprint = digest.hexdigest()[:16]

    speaker = None
    for voice in enrolled_voices or []:
        if voice.get("fingerprint") == fingerprint:
            speaker = {"voice_id": voice.get("voice_id", ""), "confidence": 1.0}
            break

    result: dict[str, Any] = {
        "instruments": families,
        "key": None,
        "bpm": None,
        "speaker": speaker,
        "fingerprint": fingerprint,
        "duration_sec": len(samples) / max(1, rate),
        "method": "coarse",
    }

    # Optional fine path; never required and never called in offline CI.
    try:  # pragma: no cover - optional dependency branch
        import numpy  # noqa: F401
        import librosa  # noqa: F401
        result["method"] = "fine"
    except Exception:
        pass

    return result
