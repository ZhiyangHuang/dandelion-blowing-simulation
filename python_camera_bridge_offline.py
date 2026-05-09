import json
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parent
OUT_PATH = ROOT / "camera_bridge_latest.json"
TMP_PATH = ROOT / "camera_bridge_latest.tmp"


def clamp(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))


def write_packet(packet: dict) -> None:
    TMP_PATH.write_text(json.dumps(packet), encoding="utf-8")
    TMP_PATH.replace(OUT_PATH)


def normalized_sample_name(name: str) -> str:
    return Path(name).name.strip().lower()


def sample_profile(name: str) -> dict:
    sample_name = normalized_sample_name(name)
    if sample_name == "close_0.jpg":
        return {
            "backend": "offline-camera-bridge-close-0",
            "face_detected": True,
            "mouth_open_state": False,
            "mouth_open_ratio": 0.025,
            "mouth_center_x": 0.47,
            "mouth_center_y": 0.55,
            "confidence": 0.88,
            "jaw_open_score": 0.06,
            "pitch_deg": 0.0,
            "yaw_deg": -2.0,
            "roll_deg": 1.5,
            "looking_forward": True,
        }
    if sample_name == "close_1.jpg":
        return {
            "backend": "offline-camera-bridge-close-1",
            "face_detected": True,
            "mouth_open_state": False,
            "mouth_open_ratio": 0.03,
            "mouth_center_x": 0.53,
            "mouth_center_y": 0.51,
            "confidence": 0.9,
            "jaw_open_score": 0.07,
            "pitch_deg": 1.0,
            "yaw_deg": 1.5,
            "roll_deg": -1.0,
            "looking_forward": True,
        }
    if sample_name == "open_0.jpg":
        return {
            "backend": "offline-camera-bridge-open-0",
            "face_detected": True,
            "mouth_open_state": True,
            "mouth_open_ratio": 0.12,
            "mouth_center_x": 0.48,
            "mouth_center_y": 0.56,
            "confidence": 0.93,
            "jaw_open_score": 0.42,
            "pitch_deg": -1.0,
            "yaw_deg": -1.0,
            "roll_deg": 2.0,
            "looking_forward": True,
        }
    if sample_name == "open_1.jpg":
        return {
            "backend": "offline-camera-bridge-open-1",
            "face_detected": True,
            "mouth_open_state": True,
            "mouth_open_ratio": 0.135,
            "mouth_center_x": 0.54,
            "mouth_center_y": 0.5,
            "confidence": 0.95,
            "jaw_open_score": 0.48,
            "pitch_deg": 0.5,
            "yaw_deg": 2.0,
            "roll_deg": -1.5,
            "looking_forward": True,
        }
    raise ValueError(
        "unsupported sample "
        f"'{name}'. expected one of: close_0.jpg, close_1.jpg, open_0.jpg, open_1.jpg"
    )


def build_packet(profile: dict, sample_name: str) -> dict:
    return {
        "timestamp_ms": int(time.time() * 1000),
        "backend": profile["backend"],
        "sample_name": sample_name,
        "face_detected": bool(profile["face_detected"]),
        "mouth_open_state": bool(profile["mouth_open_state"]),
        "mouth_open_ratio": float(profile["mouth_open_ratio"]),
        "mouth_center_x": clamp(float(profile["mouth_center_x"]), 0.0, 1.0),
        "mouth_center_y": clamp(float(profile["mouth_center_y"]), 0.0, 1.0),
        "confidence": clamp(float(profile["confidence"]), 0.0, 1.0),
        "jaw_open_score": clamp(float(profile["jaw_open_score"]), 0.0, 1.0),
        "pitch_deg": float(profile["pitch_deg"]),
        "yaw_deg": float(profile["yaw_deg"]),
        "roll_deg": float(profile["roll_deg"]),
        "looking_forward": bool(profile["looking_forward"]),
    }


def run(sample_name: str) -> int:
    sample_path = ROOT / sample_name
    if not sample_path.exists():
        print(f"sample file not found: {sample_path.name}", file=sys.stderr)
        return 2

    profile = sample_profile(sample_path.name)

    print(f"offline camera bridge writer started for {sample_path.name}")
    try:
        while True:
            packet = build_packet(profile, sample_path.name)
            write_packet(packet)
            time.sleep(0.05)
    except KeyboardInterrupt:
        print("offline camera bridge writer stopped")
        return 0


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: python python_camera_bridge_offline.py <sample-name>", file=sys.stderr)
        return 1

    return run(sys.argv[1])


if __name__ == "__main__":
    raise SystemExit(main())
