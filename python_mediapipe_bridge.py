import json
import math
import shutil
import sys
import tempfile
import time
from pathlib import Path

try:
    import cv2
except ImportError:
    print("opencv-python is required", file=sys.stderr)
    sys.exit(2)

try:
    import mediapipe as mp
except ImportError:
    print("mediapipe is required", file=sys.stderr)
    sys.exit(3)


ROOT = Path(__file__).resolve().parent
OUT_PATH = ROOT / "camera_bridge_latest.json"
TMP_PATH = ROOT / "camera_bridge_latest.tmp"
CONTROL_PATH = ROOT / "camera_bridge_control.json"
TASK_PATH = ROOT / "face_landmarker.task"
REOPEN_DELAY_SECONDS = 1.0
CAMERA_STARTUP_TIMEOUT_SECONDS = 3.0

MOUTH_LEFT = 61
MOUTH_RIGHT = 291
MOUTH_TOP = 13
MOUTH_BOTTOM = 14


def clamp(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))


def write_packet(packet: dict) -> None:
    TMP_PATH.write_text(json.dumps(packet), encoding="utf-8")
    TMP_PATH.replace(OUT_PATH)

def check_control(min_mtime: float = 0.0):
    try:
        if not CONTROL_PATH.exists():
            return None
        stat = CONTROL_PATH.stat()
        if stat.st_mtime < min_mtime:
            return None
        data = json.loads(CONTROL_PATH.read_text())
        return data.get("command")
    except:
        return None

def resolve_model_path(original_path: Path) -> Path:
    if not original_path.exists():
        return original_path

    ascii_safe = all(ord(ch) < 128 for ch in str(original_path))
    if ascii_safe:
        return original_path

    temp_target = Path(tempfile.gettempdir()) / "dandelionos_face_landmarker.task"
    if (not temp_target.exists() or
            original_path.stat().st_mtime > temp_target.stat().st_mtime):
        shutil.copyfile(original_path, temp_target)
    return temp_target


def build_empty_packet(backend: str) -> dict:
    return {
        "timestamp_ms": int(time.time() * 1000),
        "backend": backend,
        "bridge_connected": False,
        "sample_ready": False,
        "device_unavailable": False,
        "face_detected": False,
        "mouth_open_state": False,
        "mouth_open_ratio": 0.0,
        "mouth_center_x": 0.5,
        "mouth_center_y": 0.5,
        "confidence": 0.0,
        "jaw_open_score": 0.0,
        "pitch_deg": 0.0,
        "yaw_deg": 0.0,
        "roll_deg": 0.0,
        "looking_forward": False,
        "status_text": "camera bridge idle",
    }


def camera_candidates() -> list[tuple[int, int | None, str]]:
    candidates: list[tuple[int, int | None, str]] = []
    backend_options = [
        ("dshow", getattr(cv2, "CAP_DSHOW", None)),
        ("msmf", getattr(cv2, "CAP_MSMF", None)),
        ("default", None),
    ]
    for index in range(3):
        for backend_name, backend_value in backend_options:
            candidates.append((index, backend_value, backend_name))
    return candidates


def open_best_camera() -> tuple[cv2.VideoCapture | None, str]:
    last_label = "camera-open-failed"
    for index, backend, backend_name in camera_candidates():
        label = f"camera{index}-{backend_name}"
        last_label = label
        cap = cv2.VideoCapture(index) if backend is None else cv2.VideoCapture(index, backend)
        if not cap.isOpened():
            cap.release()
            continue

        cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
        return cap, label

    return None, last_label


class FaceMeshFallbackTracker:
    def __init__(self) -> None:
        self.face_mesh = mp.solutions.face_mesh.FaceMesh(
            static_image_mode=False,
            max_num_faces=1,
            refine_landmarks=True,
            min_detection_confidence=0.5,
            min_tracking_confidence=0.5,
        )
        self.backend_name = "python-mediapipe-face-mesh-fallback"

    def close(self) -> None:
        self.face_mesh.close()

    def process(self, frame_bgr) -> dict:
        packet = build_empty_packet(self.backend_name)
        packet["bridge_connected"] = True
        packet["sample_ready"] = True
        packet["status_text"] = "camera sample ready, no face"
        frame_rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
        result = self.face_mesh.process(frame_rgb)

        if not result.multi_face_landmarks:
            return packet

        landmarks = result.multi_face_landmarks[0].landmark
        left = landmarks[MOUTH_LEFT]
        right = landmarks[MOUTH_RIGHT]
        top = landmarks[MOUTH_TOP]
        bottom = landmarks[MOUTH_BOTTOM]

        mouth_width = math.hypot(right.x - left.x, right.y - left.y)
        mouth_height = math.hypot(bottom.x - top.x, bottom.y - top.y)
        ratio = 0.0 if mouth_width < 1e-6 else mouth_height / mouth_width

        packet.update(
            {
                "bridge_connected": True,
                "sample_ready": True,
                "face_detected": True,
                "mouth_open_ratio": ratio,
                "mouth_center_x": clamp((left.x + right.x + top.x + bottom.x) * 0.25, 0.0, 1.0),
                "mouth_center_y": clamp((left.y + right.y + top.y + bottom.y) * 0.25, 0.0, 1.0),
                "confidence": 0.7,
                "jaw_open_score": ratio,
                "looking_forward": True,
                "status_text": "face mesh fallback sample ready",
            }
        )
        return packet


class FaceLandmarkerTracker:
    def __init__(self, model_path: Path) -> None:
        mp_tasks = mp.tasks
        mp_vision = mp.tasks.vision
        resolved_model_path = resolve_model_path(model_path)

        self.backend_name = "python-mediapipe-face-landmarker"
        self.base_options = mp_tasks.BaseOptions
        self.face_landmarker = mp_vision.FaceLandmarker
        self.face_landmarker_options = mp_vision.FaceLandmarkerOptions
        self.running_mode = mp_vision.RunningMode

        options = self.face_landmarker_options(
            base_options=self.base_options(model_asset_path=str(resolved_model_path)),
            running_mode=self.running_mode.IMAGE,
            num_faces=1,
            min_tracking_confidence=0.4,
            min_face_detection_confidence=0.4,
            min_face_presence_confidence=0.4,
            output_face_blendshapes=True,
            output_facial_transformation_matrixes=True,
        )
        self.detector = self.face_landmarker.create_from_options(options)

    def close(self) -> None:
        self.detector.close()

    @staticmethod
    def extract_pose(matrix_data) -> tuple[float, float, float]:
        matrix = [float(v) for row in matrix_data for v in row]
        r00, r01, r02 = matrix[0], matrix[1], matrix[2]
        r10, r11, r12 = matrix[4], matrix[5], matrix[6]
        r20, r21, r22 = matrix[8], matrix[9], matrix[10]

        pitch = math.degrees(math.atan2(-r12, r11)) + 15.0
        yaw = math.degrees(math.atan2(r02, r22))
        roll = math.degrees(math.atan2(r10, r00))
        return pitch, yaw, roll

    @staticmethod
    def is_looking_forward(pitch: float, yaw: float, roll: float) -> bool:
        return abs(yaw) < 10.0 and abs(pitch) < 10.0 and abs(roll) < 15.0

    @staticmethod
    def blendshape_map(result) -> dict[str, float]:
        if not result.face_blendshapes:
            return {}
        output = {}
        for blendshape in result.face_blendshapes[0]:
            output[blendshape.category_name] = float(blendshape.score)
        return output

    def process(self, frame_bgr) -> dict:
        packet = build_empty_packet(self.backend_name)
        packet["bridge_connected"] = True
        packet["sample_ready"] = True
        packet["status_text"] = "camera sample ready, no face"
        frame_rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=frame_rgb)
        result = self.detector.detect(mp_image)

        if not result.face_landmarks:
            return packet

        landmarks = result.face_landmarks[0]
        left = landmarks[MOUTH_LEFT]
        right = landmarks[MOUTH_RIGHT]
        top = landmarks[MOUTH_TOP]
        bottom = landmarks[MOUTH_BOTTOM]

        mouth_width = math.hypot(right.x - left.x, right.y - left.y)
        mouth_height = math.hypot(bottom.x - top.x, bottom.y - top.y)
        ratio = 0.0 if mouth_width < 1e-6 else mouth_height / mouth_width

        blendshapes = self.blendshape_map(result)
        jaw_open_score = blendshapes.get("jawOpen", 0.0)

        pitch = yaw = roll = 0.0
        looking_forward = False
        if result.facial_transformation_matrixes:
            pitch, yaw, roll = self.extract_pose(result.facial_transformation_matrixes[0])
            looking_forward = self.is_looking_forward(pitch, yaw, roll)

        packet.update(
            {
                "bridge_connected": True,
                "sample_ready": True,
                "face_detected": True,
                "mouth_open_ratio": ratio,
                "mouth_center_x": clamp((left.x + right.x + top.x + bottom.x) * 0.25, 0.0, 1.0),
                "mouth_center_y": clamp((left.y + right.y + top.y + bottom.y) * 0.25, 0.0, 1.0),
                "confidence": max(0.5, jaw_open_score),
                "jaw_open_score": jaw_open_score,
                "pitch_deg": pitch,
                "yaw_deg": yaw,
                "roll_deg": roll,
                "looking_forward": looking_forward,
                "status_text": "face tracked" if looking_forward else "face tracked, not forward",
            }
        )
        return packet


class StableMouthBridge:
    def __init__(self) -> None:
        self.open_threshold_ratio = 0.08
        self.close_threshold_ratio = 0.045
        self.open_threshold_jaw = 0.28
        self.close_threshold_jaw = 0.12
        self.mouth_open_state = False
        self.cap = None
        self.camera_backend = "camera-not-open"
        self.last_open_attempt_ms = 0.0
        self.camera_wait_started_at = time.time()
        self.process_started_at = time.time()
        self.first_frame_started_at = 0.0

        self.detector = self._build_detector()

    def _build_detector(self):
        if TASK_PATH.exists():
            try:
                return FaceLandmarkerTracker(TASK_PATH)
            except Exception as exc:
                print(f"face_landmarker.task load failed, fallback to FaceMesh: {exc}", file=sys.stderr)
        if hasattr(mp, "solutions") and hasattr(mp.solutions, "face_mesh"):
            return FaceMeshFallbackTracker()
        raise RuntimeError(
            "mediapipe face mesh fallback unavailable and face_landmarker.task could not be loaded")

    def close(self) -> None:
        self.detector.close()
        if self.cap is not None:
            self.cap.release()
            self.cap = None

    def interrupt_allowed(self) -> bool:
        return self.first_frame_started_at > 0.0

    def ensure_camera(self) -> bool:
        now = time.time()
        if self.cap is not None and self.cap.isOpened():
            return True
        if now - self.last_open_attempt_ms < REOPEN_DELAY_SECONDS:
            return False

        self.last_open_attempt_ms = now
        cap, backend = open_best_camera()
        self.camera_backend = backend
        if cap is None:
            self.cap = None
            return False

        self.cap = cap
        self.camera_wait_started_at = now
        return True

    def camera_wait_packet(self) -> dict:
        packet = build_empty_packet(self.camera_backend)
        waited_seconds = max(0.0, time.time() - self.camera_wait_started_at)
        packet["device_unavailable"] = waited_seconds >= CAMERA_STARTUP_TIMEOUT_SECONDS
        if packet["device_unavailable"]:
            packet["status_text"] = f"camera device unavailable via {self.camera_backend}"
        else:
            packet["status_text"] = f"camera starting, waiting for device via {self.camera_backend}"
        return packet

    def update_hysteresis(self, packet: dict) -> None:
        ratio = float(packet.get("mouth_open_ratio", 0.0))
        jaw_open = float(packet.get("jaw_open_score", 0.0))
        looking_forward = bool(packet.get("looking_forward", True))

        open_signal = ratio >= self.open_threshold_ratio or jaw_open >= self.open_threshold_jaw
        close_signal = ratio <= self.close_threshold_ratio and jaw_open <= self.close_threshold_jaw

        if not looking_forward:
            open_signal = False

        if not self.mouth_open_state and open_signal:
            self.mouth_open_state = True
        elif self.mouth_open_state and close_signal:
            self.mouth_open_state = False

        packet["mouth_open_state"] = self.mouth_open_state

    def run(self) -> int:
        try:
            while True:
                if check_control(self.process_started_at) == "stop" and self.interrupt_allowed():
                    break
                if not self.ensure_camera():
                    packet = self.camera_wait_packet()
                    write_packet(packet)
                    time.sleep(0.05)
                    continue

                ok, frame = self.cap.read()
                if not ok or frame is None:
                    packet = build_empty_packet(self.camera_backend)
                    packet["bridge_connected"] = True
                    packet["device_unavailable"] = False
                    packet["status_text"] = f"camera opened on {self.camera_backend}, waiting for frame"
                    write_packet(packet)
                    time.sleep(0.03)
                    continue

                if self.first_frame_started_at <= 0.0:
                    self.first_frame_started_at = time.time()

                packet = self.detector.process(frame)
                self.update_hysteresis(packet)
                packet["timestamp_ms"] = int(time.time() * 1000)
                packet["device_unavailable"] = False
                packet["backend"] = f"{packet.get('backend', 'camera-bridge')}@{self.camera_backend}"
                if not packet.get("face_detected", False):
                    packet["status_text"] = "camera sample ready, no face"
                elif not packet.get("looking_forward", True):
                    packet["status_text"] = "face tracked, not forward"
                elif not packet.get("mouth_open_state", False):
                    packet["status_text"] = "face tracked, mouth closed"
                else:
                    packet["status_text"] = "face tracked, mouth open"
                write_packet(packet)
                time.sleep(0.01)
        finally:
            self.close()


def main() -> int:
    try:
        bridge = StableMouthBridge()
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 4

    return bridge.run()


if __name__ == "__main__":
    raise SystemExit(main())
