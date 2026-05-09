import json
import math
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parent
OUT_PATH = ROOT / "microphone_bridge_latest.json"
TMP_PATH = ROOT / "microphone_bridge_latest.tmp"


def clamp(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))


def write_packet(packet: dict) -> None:
    TMP_PATH.write_text(json.dumps(packet), encoding="utf-8")
    TMP_PATH.replace(OUT_PATH)


def normalized_sample_name(name: str) -> str:
    return Path(name).name.strip().lower()


def sample_profile(name: str) -> dict:
    sample_name = normalized_sample_name(name)
    if sample_name == "huu.m4a":
        return {
            "backend": "offline-microphone-bridge-huu",
            "voice_detected": False,
            "fallback_requested": True,
            "base_power": 4.8,
            "power_wave": 1.0,
            "direction_x": 0.0,
            "direction_y": -1.0,
            "confidence": 0.78,
            "cycle_seconds": 1.8,
        }
    if sample_name == "ahh.m4a":
        return {
            "backend": "offline-microphone-bridge-ahh",
            "voice_detected": True,
            "fallback_requested": False,
            "base_power": 0.38,
            "power_wave": 0.08,
            "direction_x": 0.0,
            "direction_y": -1.0,
            "confidence": 0.86,
            "cycle_seconds": 2.0,
        }
    if sample_name == "no voice.m4a":
        return {
            "backend": "offline-microphone-bridge-silence",
            "voice_detected": False,
            "fallback_requested": False,
            "base_power": 0.1,
            "power_wave": 0.0,
            "direction_x": 0.0,
            "direction_y": -1.0,
            "confidence": 0.05,
            "cycle_seconds": 1.5,
        }
    raise ValueError(
        f"unsupported sample '{name}'. expected one of: huu.m4a, ahh.m4a, no voice.m4a"
    )


def build_packet(profile: dict, elapsed_seconds: float, sample_name: str) -> dict:
    cycle_seconds = float(profile["cycle_seconds"])
    phase = 0.0 if cycle_seconds <= 0.0 else (elapsed_seconds % cycle_seconds) / cycle_seconds
    wave = math.sin(phase * math.tau)
    suggested_power = clamp(
        float(profile["base_power"]) + float(profile["power_wave"]) * max(0.0, wave),
        0.1,
        10.0,
    )

    return {
        "timestamp_ms": int(time.time() * 1000),
        "backend": profile["backend"],
        "sample_name": sample_name,
        "voice_detected": bool(profile["voice_detected"]),
        "fallback_requested": bool(profile["fallback_requested"]),
        "suggested_power": suggested_power,
        "direction_x": float(profile["direction_x"]),
        "direction_y": float(profile["direction_y"]),
        "confidence": float(profile["confidence"]),
    }


def run(sample_name: str) -> int:
    sample_path = ROOT / sample_name
    if not sample_path.exists():
        print(f"sample file not found: {sample_path.name}", file=sys.stderr)
        return 2

    profile = sample_profile(sample_path.name)
    start = time.time()

    print(f"offline microphone bridge writer started for {sample_path.name}")
    try:
        while True:
            elapsed = time.time() - start
            packet = build_packet(profile, elapsed, sample_path.name)
            write_packet(packet)
            time.sleep(0.05)
    except KeyboardInterrupt:
        print("offline microphone bridge writer stopped")
        return 0


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: python python_microphone_bridge_offline.py <sample-name>", file=sys.stderr)
        return 1

    return run(sys.argv[1])


if __name__ == "__main__":
    raise SystemExit(main())
