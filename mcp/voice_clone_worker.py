"""Strictly local-only voice print enrollment and synthesis.

Voice prints are stored on-device under ~/Documents/Toxic Creationz Studio/VoicePrints
(or an isolated directory supplied by tests). No cloud service is contacted.
"""

from __future__ import annotations

import datetime as _dt
import hashlib
import json
import shutil
import wave
from pathlib import Path
from typing import Any


class VoiceCloneError(ValueError):
    """Validation or local-backend error (never a silent fallback)."""

    code = -32602


def voice_prints_root(voice_prints_dir: str | Path | None = None) -> Path:
    if voice_prints_dir is not None:
        return Path(voice_prints_dir)
    return Path.home() / "Documents" / "Toxic Creationz Studio" / "VoicePrints"


def _read_duration(path: Path) -> tuple[float, int, float]:
    if not path.exists():
        raise VoiceCloneError(f"Reference WAV not found: {path}")
    try:
        with wave.open(str(path), "rb") as wav:
            frames = wav.readframes(wav.getnframes())
            rate = wav.getframerate()
            duration = wav.getnframes() / max(1, rate)
    except Exception as exc:  # pragma: no cover - corrupt fixture path
        raise VoiceCloneError(f"Invalid reference WAV: {exc}") from exc
    peak = 0.0
    if frames:
        import array
        pcm = array.array("h")
        pcm.frombytes(frames)
        peak = max((abs(v) for v in pcm), default=0) / 32768.0
    return duration, rate, peak


def enroll_voice(
    voice_id: str,
    name: str,
    reference_wav: str | Path,
    consent: bool = False,
    overwrite: bool = False,
    voice_prints_dir: str | Path | None = None,
) -> dict[str, Any]:
    if consent is not True:
        raise VoiceCloneError("Explicit consent=True is required to enroll a voice")
    voice_id = voice_id.strip()
    if not voice_id or not name.strip():
        raise VoiceCloneError("voice_id and name are required")

    duration, rate, peak = _read_duration(Path(reference_wav))
    if duration < 10.0 or duration > 60.0:
        raise VoiceCloneError("Reference WAV must be 10-60 seconds")
    if rate < 7000:
        raise VoiceCloneError("Reference WAV sample rate is too low")
    if peak <= 0.001:
        raise VoiceCloneError("Reference WAV is silent")

    root = voice_prints_root(voice_prints_dir)
    target_dir = root / voice_id
    if target_dir.exists() and not overwrite:
        raise VoiceCloneError(f"Voice {voice_id} is already enrolled")
    target_dir.mkdir(parents=True, exist_ok=True)

    reference = target_dir / "reference.wav"
    shutil.copy2(reference_wav, reference)
    digest = hashlib.sha256(reference.read_bytes()).hexdigest()
    (target_dir / "embedding.bin").write_bytes(bytes.fromhex(digest[:32]))

    metadata = {
        "voice_id": voice_id,
        "name": name.strip(),
        "enrolled": _dt.datetime.now(_dt.timezone.utc).isoformat(),
        "backend": "local-only",
        "consent_note": "Operator supplied explicit consent; print stays on this device.",
        "fingerprint": digest[:16],
        "duration_sec": duration,
        "sample_rate": rate,
    }
    (target_dir / "voice.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    return metadata


def list_voices(voice_prints_dir: str | Path | None = None) -> list[dict[str, Any]]:
    root = voice_prints_root(voice_prints_dir)
    if not root.exists():
        return []
    voices: list[dict[str, Any]] = []
    for child in sorted(root.iterdir()):
        meta = child / "voice.json"
        if child.is_dir() and meta.exists():
            voices.append(json.loads(meta.read_text(encoding="utf-8")))
    return voices


def forget_voice(
    voice_id: str | None = None,
    delete_all: bool = False,
    voice_prints_dir: str | Path | None = None,
) -> int:
    root = voice_prints_root(voice_prints_dir)
    if not root.exists():
        return 0
    if delete_all:
        removed = 0
        for child in root.iterdir():
            if child.is_dir():
                shutil.rmtree(child)
                removed += 1
        return removed
    if not voice_id:
        raise VoiceCloneError("voice_id or delete_all is required")
    target = root / voice_id.strip()
    if not target.exists():
        raise VoiceCloneError(f"Unknown voice_id: {voice_id}")
    shutil.rmtree(target)
    return 1


def generate_voice_line(
    voice_id: str,
    text: str,
    output_wav: str | Path,
    emotion: str = "neutral",
    voice_prints_dir: str | Path | None = None,
) -> dict[str, Any]:
    """Synthesize with a local zero-shot model only; never substitute a voice."""
    root = voice_prints_root(voice_prints_dir)
    target = root / voice_id.strip()
    if not (target / "voice.json").exists():
        raise VoiceCloneError(f"Unknown voice_id: {voice_id}")
    if not text.strip():
        raise VoiceCloneError("Text is required")

    try:  # pragma: no cover - optional local backend
        from TTS.api import TTS  # type: ignore

        model = TTS(model_name="xtts_v2")
        model.tts_to_file(text=text, speaker_wav=str(target / "reference.wav"),
                          language="en", file_path=str(output_wav), emotion=emotion)
    except Exception:
        return {
            "status": "unavailable",
            "voice_id": voice_id,
            "error": "Local zero-shot cloning backend is not installed; no voice was substituted.",
        }

    return {"status": "ok", "voice_id": voice_id, "output_wav": str(output_wav)}
