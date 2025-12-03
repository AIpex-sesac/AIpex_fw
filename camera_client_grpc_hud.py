#!/usr/bin/env python3
"""
gRPC 기반 듀얼 카메라 HUD 클라이언트
- FRONT 카메라: 전체 화면 HUD + detection 박스
- REAR 카메라: 좌측 상단 PIP + detection 박스
- gRPC로 inference 서버와 통신
"""

import cv2
import numpy as np
import threading
import time
import json  # ← nav_json 문자열 처리용
from picamera2 import Picamera2
from board_transmission import BoardTransmission
import smbus2
import queue
from concurrent import futures as _futures
import hud_stream_pb2 as hud_pb2
import hud_stream_pb2_grpc as hud_pb2_grpc
import grpc

# === 전송용 카메라 래퍼: capture_array 결과를 회전시켜서 반환 ===
class RotatedCamera:
    def __init__(self, base_cam, rotate_code=None, flip_code=None):
        """
        base_cam : Picamera2 인스턴스
        rotate_code : cv2.ROTATE_180 등
        flip_code   : cv2.flip용 (0, 1, -1) / 필요없으면 None
        """
        self._base = base_cam
        self._rotate_code = rotate_code
        self._flip_code = flip_code

    def __getattr__(self, name):
        # start(), stop(), configure() 등은 원본 카메라에 위임
        return getattr(self._base, name)

    def capture_array(self, *args, **kwargs):
        """
        Picamera2.capture_array("main") / ("lores") 호출을 감싸서
        main 스트림에만 회전/플립 적용
        """
        stream_name = None
        if args:
            stream_name = args[0]
        elif "name" in kwargs:
            stream_name = kwargs["name"]

        frame = self._base.capture_array(*args, **kwargs)

        # ★ main 스트림만 회전/플립
        if stream_name == "main":
            if self._rotate_code is not None:
                frame = cv2.rotate(frame, self._rotate_code)
            if self._flip_code is not None:
                frame = cv2.flip(frame, self._flip_code)

        return frame


# ====== HUD / PIP 설정 ======
SCREEN_W = 800
SCREEN_H = 480

REAR_FULL_H = SCREEN_H
REAR_FULL_W = int(REAR_FULL_H * 4 / 3)
if REAR_FULL_W > SCREEN_W:
    REAR_FULL_W = SCREEN_W
    REAR_FULL_H = int(REAR_FULL_W * 3 / 4)

PIP_SCALE = 0.33
PIP_W = int(REAR_FULL_W * PIP_SCALE)
PIP_H = int(REAR_FULL_H * PIP_SCALE)
PIP_X = 10
PIP_Y = 10
REAR_PIP_HOLD_SEC = 3.0  # 마지막 디텍션 후 PIP 유지 시간(초)

# rear 화살표 유지/점멸 설정
ARROW_ALERT_HOLD_SEC = 3.0   # 디텍팅 후 화살표 유지 시간(초)
ARROW_BLINK_PERIOD = 0.6     # 점멸 주기(초) - 0.6초마다 on/off

# ====== 서버 주소 ======
SERVER_ADDRESS = "AipexFW.local:50051"

# ====== 공유 변수 ======
rear_frame: np.ndarray | None = None
rear_frame_lock = threading.Lock()

heading_deg: float = 0.0
heading_lock = threading.Lock()

# ====== X1200 배터리 ======
I2C_BUS_ID = 1
FG_ADDR = 0x36


def _read_word_swapped(bus, reg):
    raw = bus.read_word_data(FG_ADDR, reg)
    return ((raw & 0xFF) << 8) | (raw >> 8)


## I2C 배터리 잔량 수신부
def get_battery_percentage() -> int | None:
    try:
        bus = smbus2.SMBus(I2C_BUS_ID)
        raw_soc = _read_word_swapped(bus, 0x04)
        bus.close()
        percent = raw_soc / 256.0
        percent = max(0.0, min(100.0, percent))
        return int(round(percent))
    except Exception as e:
        print(f"[BAT] Failed to read battery: {e}")
        return None


def draw_battery_overlay(frame: np.ndarray, level: int | None) -> np.ndarray:
    h, w, _ = frame.shape
    if level is None:
        percent_text = "--%"
        level_val = 0
    else:
        percent_text = f"{int(level)}%"
        level_val = max(0, min(100, int(level)))

    bw, bh = 70, 18
    margin = 8
    x2 = w - margin
    x1 = x2 - bw
    y1 = margin
    y2 = y1 + bh
    green = (0, 255, 0)

    cv2.rectangle(frame, (x1, y1), (x2, y2), green, 2)
    head_w = 5
    cv2.rectangle(frame, (x2, y1 + bh // 4), (x2 + head_w, y2 - bh // 4), green, -1)

    inner_margin = 3
    ix1, ix2 = x1 + inner_margin, x2 - inner_margin
    iy1, iy2 = y1 + inner_margin, y2 - inner_margin
    cv2.rectangle(frame, (ix1, iy1), (ix2, iy2), (255, 255, 255), -1)

    inner_width = ix2 - ix1
    fill_w = int(inner_width * (level_val / 100.0))
    if fill_w > 0:
        cv2.rectangle(frame, (ix1, iy1), (ix1 + fill_w, iy2), green, -1)

    cells = 4
    for i in range(1, cells):
        x = ix1 + int(inner_width * i / cells)
        cv2.line(frame, (x, iy1), (x, iy2), (0, 0, 0), 1, cv2.LINE_AA)

    font = cv2.FONT_HERSHEY_SIMPLEX
    font_scale = 0.6
    thickness = 2
    (pt_w, pt_h), _ = cv2.getTextSize(percent_text, font, font_scale, thickness)
    text_x = x1 - pt_w - 8
    text_y = y1 + pt_h + 2

    cv2.putText(frame, percent_text, (text_x, text_y), font, font_scale, (0, 0, 0), thickness + 2, cv2.LINE_AA)
    cv2.putText(frame, percent_text, (text_x, text_y), font, font_scale, green, thickness, cv2.LINE_AA)
    return frame


def draw_heading_scale(frame: np.ndarray, heading_deg: float) -> np.ndarray:
    h, w, _ = frame.shape
    green = (0, 255, 0)
    band_h = 16
    px_per_deg = 4

    side_margin = int(w * 0.30)
    left_bound = side_margin
    right_bound = w - side_margin
    center_x = w // 2
    margin_bottom = 20
    y0 = h - band_h - margin_bottom

    font = cv2.FONT_HERSHEY_SIMPLEX
    usable_w = right_bound - left_bound
    max_offset = int((usable_w / 2) / px_per_deg) + 2

    for offset in range(-max_offset, max_offset + 1):
        deg = (heading_deg + offset) % 360.0
        x = int(center_x + offset * px_per_deg)
        if x < left_bound or x >= right_bound:
            continue

        d_int = int(round(deg))
        draw_tick = False
        length = band_h - 16

        if d_int % 5 == 0:
            length = band_h - 14
            draw_tick = True
        if d_int % 10 == 0:
            length = band_h - 10
            draw_tick = True

        if draw_tick:
            y1, y2 = y0, y0 + length
            cv2.line(frame, (x, y1), (x, y2), green, 1, cv2.LINE_AA)

            if d_int % 10 == 0:
                label = str(d_int % 360)
                font_scale = 0.45
                t_thick = 1
                (tw, th), _ = cv2.getTextSize(label, font, font_scale, t_thick)
                tx = x - tw // 2
                ty = y2 + th + 2
                cv2.putText(frame, label, (tx, ty), font, font_scale, green, t_thick, cv2.LINE_AA)

    tri_height = 10
    offset_above_scale = 8
    base_y = max(0, y0 - offset_above_scale - tri_height)
    tip_y = base_y + tri_height

    pts = np.array([[center_x - 6, base_y], [center_x + 6, base_y], [center_x, tip_y]], dtype=np.int32)
    cv2.fillConvexPoly(frame, pts, green)

    cur_label = f"{int(round(heading_deg)) % 360}"
    font_scale = 0.6
    t_thick = 2
    (tw, th), _ = cv2.getTextSize(cur_label, font, font_scale, t_thick)
    tx = center_x - tw // 2
    ty = max(th + 2, base_y - 6)

    cv2.putText(frame, cur_label, (tx, ty), font, font_scale, (0, 0, 0), t_thick + 2, cv2.LINE_AA)
    cv2.putText(frame, cur_label, (tx, ty), font, font_scale, green, t_thick, cv2.LINE_AA)
    return frame


# ====== 네비 JSON용 포맷 + UI ======
def format_distance(dist: float | int | None) -> str:
    """
    remaining_distance를 사람이 보기 좋게 포맷 (단위: m 기준 가정)
    """
    if dist is None or dist < 0:
        return "--"
    try:
        dist = float(dist)
    except (ValueError, TypeError):
        return "--"

    if dist >= 1000:
        km = dist / 1000.0
        return f"{km:.1f} km"
    else:
        return f"{int(dist)} m"


def format_eta(eta: float | int | None) -> str:
    """
    eta를 '남은 시간(초)'로 가정하고 mm:ss 또는 h:mm 형식으로 포맷
    """
    if eta is None or eta < 0:
        return "--"
    try:
        eta = int(eta)
    except (ValueError, TypeError):
        return "--"

    hours = eta // 3600
    minutes = (eta % 3600) // 60
    seconds = eta % 60

    if hours > 0:
        return f"{hours}h {minutes:02d}m"
    else:
        return f"{minutes:02d}:{seconds:02d}"


def draw_text_stroke(img, text, org, font, scale, color, thickness):
    cv2.putText(img, text, org, font, scale, (0, 0, 0), thickness + 2, cv2.LINE_AA)  # 외곽선
    cv2.putText(img, text, org, font, scale, color, thickness, cv2.LINE_AA)        # 본문


def draw_nav_info(
    frame: np.ndarray,
    instruction: str | None,
    remaining_distance: float | int | None,
    speed: float | int | None,
    eta: float | int | None,
) -> np.ndarray:
    """
    좌측 하단: remaining_distance, eta
    우측 하단: instruction, speed
    heading은 별도 draw_heading_scale에서 표시
    """
    h, w, _ = frame.shape
    font = cv2.FONT_HERSHEY_SIMPLEX
    color = (0, 255, 0)
    thickness = 3
    line_h = 40
    margin_side = 12
    margin_bottom = 60  # heading 스케일과 겹치지 않도록

    # ---- 좌측 하단: 거리 + ETA ----
    dist_str = format_distance(remaining_distance)
    eta_str = format_eta(eta)

    left_line1 = f"DIST: {dist_str}"
    left_line2 = f"ETA : {eta_str}"

    y2 = h - margin_bottom
    y1 = y2 - line_h

    draw_text_stroke(frame, left_line1, (margin_side, y1), font, 1.0, color, thickness)
    draw_text_stroke(frame, left_line2, (margin_side, y2), font, 1.0, color, thickness)

    # ---- 우측 하단: instruction + 속도 ----
    if instruction is None:
        instruction = ""
    if len(instruction) > 40:
        instruction = instruction[:37] + "..."

    speed_val = "--"
    if speed is not None:
        try:
            speed_f = float(speed)
            speed_val = f"{speed_f:.1f} km/h"
        except (ValueError, TypeError):
            pass

    right_line1 = instruction
    right_line2 = f"SPD : {speed_val}"

    (w1, _), _ = cv2.getTextSize(right_line1, font, 1.0, thickness)
    (w2, _), _ = cv2.getTextSize(right_line2, font, 1.0, thickness)

    x1 = w - margin_side - w1
    x2 = w - margin_side - w2

    draw_text_stroke(frame, right_line1, (x1, y1), font, 1.0, color, thickness)
    draw_text_stroke(frame, right_line2, (x2, y2), font, 1.0, color, thickness)

    return frame


def render_detections_on_black(result: dict) -> np.ndarray:
    w = result.get("width", 640)
    h = result.get("height", 640)
    detections = result.get("detections", [])
    black = np.zeros((h, w, 3), dtype=np.uint8)

    for det in detections:
        bbox = det.get("bbox", {})
        x_min_norm = float(bbox.get("x_min", 0))
        y_min_norm = float(bbox.get("y_min", 0))
        x_max_norm = float(bbox.get("x_max", 0))
        y_max_norm = float(bbox.get("y_max", 0))

        x1 = int(x_min_norm * w)
        y1 = int(y_min_norm * h)
        x2 = int(x_max_norm * w)
        y2 = int(y_max_norm * h)

        cls_name = det.get("class", "obj")
        conf = float(det.get("score", 0.0))

        if x2 <= x1 or y2 <= y1 or x1 < 0 or y1 < 0 or x2 > w or y2 > h:
            continue

        # 박스
        cv2.rectangle(black, (x1, y1), (x2, y2), (0, 255, 0), 4)

        font = cv2.FONT_HERSHEY_SIMPLEX
        font_scale = 0.6
        thickness = 2

        cx = (x1 + x2) // 2

        # ===== 클래스네임: 박스 위 중앙 =====
        class_label = cls_name
        (cw, ch), _ = cv2.getTextSize(class_label, font, font_scale, thickness)
        class_x = cx - cw // 2
        class_y = y1 - 5
        class_x = max(0, min(class_x, w - cw))
        class_y = max(ch, min(class_y, h - 1))

        cv2.putText(
            black,
            class_label,
            (class_x, class_y),
            font,
            font_scale,
            (0, 255, 0),
            thickness,
            cv2.LINE_AA,
        )

        # ===== 점수: 박스 아래 중앙 =====
        score_label = f"{conf:.2f}"
        (sw, sh), _ = cv2.getTextSize(score_label, font, font_scale, thickness)
        score_x = cx - sw // 2
        score_y = y2 + sh + 5
        score_x = max(0, min(score_x, w - sw))
        score_y = max(sh, min(score_y, h - 1))

        cv2.putText(black, score_label, (score_x, score_y), font, font_scale, (0, 255, 0), thickness, cv2.LINE_AA,)

    return black

## AppComm JSON을 항상 dict로 변환 (문자열 → json.loads)
def get_nav_json(bt: BoardTransmission) -> dict:
    raw = bt.get_last_app_json()
    if raw is None:
        return {}
    if isinstance(raw, dict):
        return raw
    if isinstance(raw, str):
        try:
            return json.loads(raw)
        except Exception as e:
            print(f"[NAV] Failed to parse nav JSON string: {e}")
            return {}
    # 그 외 타입은 무시
    return {}


## 리어 카메라 수신부
def camera_thread_grpc(cam_name: str, cam_index: int, transmission: BoardTransmission):
    global rear_frame, heading_deg

    print(f"[{cam_name}] Camera thread started")
    try:
        while True:
            if cam_name.upper() == "REAR":
                frame_bgr = transmission.camera.capture_array("lores")
                # 방향 보정: 180도 회전 + 좌우반전 (HUD PIP용)
                frame_bgr = cv2.rotate(frame_bgr, cv2.ROTATE_180)
                frame_bgr = cv2.flip(frame_bgr, 1)

                with rear_frame_lock:
                    rear_frame = frame_bgr.copy()
                time.sleep(0.033)  # ~30fps

    except Exception as e:
        print(f"[{cam_name}] Thread error: {e}")


# 전역 구독자 관리
_hud_subscribers: list[queue.Queue] = []
_hud_subscribers_lock = threading.Lock()


class HudServicer(hud_pb2_grpc.HudServiceServicer):
    def StreamHud(self, request, context):
        """클라이언트별로 큐 생성 -> main 루프가 JPEG을 큐에 넣음 -> 여기서 yield"""
        q: "queue.Queue[tuple[bytes,int]]" = queue.Queue(maxsize=5)
        with _hud_subscribers_lock:
            _hud_subscribers.append(q)
        print(f"[HUD-SRV] Client connected, subscribers={len(_hud_subscribers)}")
        try:
            target_fps = max(1, int(request.target_fps)) if request and request.target_fps else None
            last_ts = 0
            while True:
                # 클라이언트가 끊으면 context.is_active() False
                if not context.is_active():
                    break
                try:
                    jpeg_bytes, ts = q.get(timeout=1.0)
                    # fps 제어(요청이 있으면)
                    if target_fps:
                        min_dt = 1.0 / float(target_fps)
                        if (ts - last_ts) / 1000.0 < min_dt:
                            continue
                    last_ts = ts
                    yield hud_pb2.HudFrame(jpeg=jpeg_bytes, ts=ts)
                except queue.Empty:
                    continue
        finally:
            with _hud_subscribers_lock:
                try:
                    _hud_subscribers.remove(q)
                except ValueError:
                    pass
            print(f"[HUD-SRV] Client disconnected, subscribers={len(_hud_subscribers)}")


def start_hud_server(bind_addr="0.0.0.0", bind_port=50055):
    server = grpc.server(_futures.ThreadPoolExecutor(max_workers=4))
    hud_pb2_grpc.add_HudServiceServicer_to_server(HudServicer(), server)
    bind_target = f"{bind_addr}:{bind_port}"
    bound = server.add_insecure_port(bind_target)
    if bound == 0:
        print(f"[HUD-SRV] Failed to bind {bind_target}")
        return None
    server.start()
    print(f"[HUD-SRV] Started on {bind_target} (bound={bound})")
    return server


def main():
    global rear_frame, heading_deg

    # FRONT 카메라 설정
    print("[Main] Initializing FRONT camera...")
    front_cam = Picamera2(0)
    front_config = front_cam.create_video_configuration(
        main={"size": (1640, 1232)},                      # 디텍션용 원본 (A 파이로 전송)
        lores={"size": (SCREEN_W, SCREEN_H), "format": "RGB888"}  # ★ HUD 베이스용 해상도 변경
    )
    front_cam.configure(front_config)
    front_cam.start()
    time.sleep(0.5)

    # ★ 전송용으로만 180도 회전된 카메라 래퍼 사용 (main 스트림만)
    front_cam_tx = RotatedCamera(front_cam, rotate_code=cv2.ROTATE_180, flip_code=None)

    # BoardTransmission 설정 (FRONT용) - 회전 적용된 카메라 사용
    front_transmission = BoardTransmission(camera=front_cam_tx,
                                           server_address=SERVER_ADDRESS,
                                           camera_id=0)
    front_transmission.connect()
    time.sleep(1.0)
    front_transmission.start_streaming(start_app_server=True)

    # REAR 카메라 설정
    print("[Main] Initializing REAR camera...")
    try:
        rear_cam = Picamera2(1)
        rear_config = rear_cam.create_video_configuration(
            main={"size": (1640, 1232)},                      # 서버로 가는 스트림
            lores={"size": (640, 480), "format": "RGB888"}    # HUD PIP용
        )
        rear_cam.configure(rear_config)
        rear_cam.start()
        time.sleep(0.5)
        rear_camera_available = True
    except Exception as e:
        print(f"[Main] REAR camera initialization failed: {e}")
        print("[Main] Running with FRONT camera only")
        rear_cam = None
        rear_camera_available = False

    # REAR 카메라용 BoardTransmission
    rear_transmission = None
    if rear_camera_available:
        # ★ REAR도 전송(main)만 180도 회전
        rear_cam_tx = RotatedCamera(rear_cam, rotate_code=cv2.ROTATE_180, flip_code=1)

        rear_transmission = BoardTransmission(camera=rear_cam_tx,
                                              server_address=SERVER_ADDRESS,
                                              camera_id=1)
        rear_transmission.connect()
        time.sleep(1.0)
        rear_transmission.start_streaming(start_app_server=False)  # app server 시작 안함

        rear_thread = threading.Thread(
            target=camera_thread_grpc,
            args=("REAR", 1, rear_transmission),
            daemon=True
        )
        rear_thread.start()

    cv2.namedWindow("Front HUD", cv2.WINDOW_NORMAL)
    cv2.setWindowProperty("Front HUD", cv2.WND_PROP_FULLSCREEN, cv2.WINDOW_FULLSCREEN)

    last_batt_read_time = 0.0
    batt_percent_cached: int | None = None

    debug_count = 0
    frame_count = 0

    # HUD gRPC 서버 시작 (원하면 다른 포트로)
    hud_server = start_hud_server(bind_addr="0.0.0.0", bind_port=50055)

    # REAR PIP 마지막 디텍션 시각
    last_rear_detection_time = 0.0

    # rear 화살표 마지막 활성 시각
    last_rear_left_alert_time = 0.0
    last_rear_right_alert_time = 0.0

    try:
        while True:
            now = time.time()
            frame_count += 1

            # ★ FRONT 카메라 실제 프레임 캡처 (lores 스트림 사용)
            front_raw_frame = front_cam.capture_array("lores")
            
            # ★ HUD 표시용 방향 보정 (lores는 회전 안되어 있으므로)
            front_display_base = cv2.rotate(front_raw_frame, cv2.ROTATE_180)
            front_display_base = cv2.cvtColor(front_display_base, cv2.COLOR_RGBB2BGR)
            
            # 배터리
            if now - last_batt_read_time > 1.0:
                batt_percent_cached = get_battery_percentage()
                last_batt_read_time = now

            # 1) FRONT detection 결과
            front_result = front_transmission.get_detection_result() or {}

            # 2) AppComm로 들어온 내비 JSON
            nav_json = get_nav_json(front_transmission)

            # heading 우선순위: App → detection_result
            heading_src = (
                nav_json.get("heading")
                or nav_json.get("heading_deg")
                or front_result.get("heading_deg")
                or front_result.get("heading")
            )
            if heading_src is not None:
                try:
                    with heading_lock:
                        heading_deg = float(heading_src) % 360.0
                except (ValueError, TypeError):
                    pass

            with heading_lock:
                cur_heading = heading_deg

            # 네비 정보 추출 (App JSON 우선, 없으면 detection_result fallback)
            nav_instruction = nav_json.get("instruction", front_result.get("instruction"))
            nav_remaining_distance = nav_json.get("remaining_distance", front_result.get("remaining_distance"))
            nav_speed = nav_json.get("speed", front_result.get("speed"))
            nav_eta = nav_json.get("eta", front_result.get("eta"))

            # 디버깅: 처음 3번만 detection 로그
            if debug_count < 3 and len(front_result.get("detections", [])) > 0:
                det_count = len(front_result.get("detections", []))
                print(f"\n[MAIN Frame {frame_count}] FRONT detections: {det_count}")
                print(f"[MAIN] First detection: {front_result['detections'][0]}")
                debug_count += 1

            # ★ HUD 디스플레이용: 검은 배경에 detection 박스만 (기존 동작 유지)
            front_canvas_fs = render_detections_on_black(front_result)
            front_canvas_fs = cv2.resize(front_canvas_fs, (SCREEN_W, SCREEN_H), interpolation=cv2.INTER_LINEAR)

            # ★ 50055 스트리밍용: 실제 카메라 배경 + UI 합성 (별도 생성)
            stream_canvas = front_display_base.copy()

            # Detection 박스를 실제 카메라 프레임 위에 그리기
            detections = front_result.get("detections", [])
            for det in detections:
                bbox = det.get("bbox", {})
                x_min_norm = float(bbox.get("x_min", 0))
                y_min_norm = float(bbox.get("y_min", 0))
                x_max_norm = float(bbox.get("x_max", 0))
                y_max_norm = float(bbox.get("y_max", 0))

                x1 = int(x_min_norm * SCREEN_W)
                y1 = int(y_min_norm * SCREEN_H)
                x2 = int(x_max_norm * SCREEN_W)
                y2 = int(y_max_norm * SCREEN_H)

                cls_name = det.get("class", "obj")
                conf = float(det.get("score", 0.0))

                if x2 <= x1 or y2 <= y1:
                    continue

                # 반투명 박스 배경
                overlay = stream_canvas.copy()
                cv2.rectangle(overlay, (x1, y1), (x2, y2), (0, 255, 0), -1)
                cv2.addWeighted(overlay, 0.2, stream_canvas, 0.8, 0, stream_canvas)
                
                # 박스 테두리
                cv2.rectangle(stream_canvas, (x1, y1), (x2, y2), (0, 255, 0), 4)

                font = cv2.FONT_HERSHEY_SIMPLEX
                font_scale = 0.8
                thickness = 2

                cx = (x1 + x2) // 2

                # 클래스네임: 박스 위 중앙
                class_label = cls_name
                (cw, ch), _ = cv2.getTextSize(class_label, font, font_scale, thickness)
                class_x = cx - cw // 2
                class_y = y1 - 10
                class_x = max(0, min(class_x, SCREEN_W - cw))
                class_y = max(ch + 5, class_y)

                # 텍스트 배경
                cv2.rectangle(stream_canvas, 
                            (class_x - 5, class_y - ch - 5),
                            (class_x + cw + 5, class_y + 5),
                            (0, 0, 0), -1)
                cv2.putText(stream_canvas, class_label, (class_x, class_y),
                          font, font_scale, (0, 255, 0), thickness, cv2.LINE_AA)

                # 점수: 박스 아래 중앙
                score_label = f"{conf:.2f}"
                (sw, sh), _ = cv2.getTextSize(score_label, font, font_scale, thickness)
                score_x = cx - sw // 2
                score_y = y2 + sh + 10
                score_x = max(0, min(score_x, SCREEN_W - sw))

                cv2.rectangle(stream_canvas,
                            (score_x - 5, score_y - sh - 5),
                            (score_x + sw + 5, score_y + 5),
                            (0, 0, 0), -1)
                cv2.putText(stream_canvas, score_label, (score_x, score_y),
                          font, font_scale, (0, 255, 0), thickness, cv2.LINE_AA)

            # ★ HUD 디스플레이용 UI 오버레이 (검은 배경)
            front_canvas_fs = draw_battery_overlay(front_canvas_fs, batt_percent_cached)
            front_canvas_fs = draw_heading_scale(front_canvas_fs, cur_heading)
            front_canvas_fs = draw_nav_info(
                front_canvas_fs,
                instruction=nav_instruction,
                remaining_distance=nav_remaining_distance,
                speed=nav_speed,
                eta=nav_eta,
            )

            # ★ 스트리밍용 UI 오버레이 (실제 카메라 배경)
            stream_canvas = draw_battery_overlay(stream_canvas, batt_percent_cached)
            stream_canvas = draw_heading_scale(stream_canvas, cur_heading)
            stream_canvas = draw_nav_info(
                stream_canvas,
                instruction=nav_instruction,
                remaining_distance=nav_remaining_distance,
                speed=nav_speed,
                eta=nav_eta,
            )

            # REAR PIP
            if rear_camera_available and rear_transmission:
                with rear_frame_lock:
                    if rear_frame is None:
                        flipped = cv2.flip(front_canvas_fs, 1)
                        flipped = cv2.rotate(flipped, cv2.ROTATE_180)
                        cv2.imshow("Front HUD", flipped)
                        if cv2.waitKey(1) & 0xFF == 27:
                            break
                        continue
                    rf = rear_frame.copy()

                rear_result = rear_transmission.get_detection_result() or {}
                detections = rear_result.get("detections", [])

                # 항상 현재 프레임으로 PIP 기본 영상 생성 (실시간 스트리밍 유지)
                rear_resized = cv2.resize(rf, (PIP_W, PIP_H), interpolation=cv2.INTER_LINEAR)

                pip_img = None

                if detections:
                    # 디텍션 있을 때: 박스 + 라벨 그리기, 마지막 디텍션 시각 갱신
                    cv2.rectangle(rear_resized, (0, 0), (PIP_W - 1, PIP_H - 1), (0, 0, 255), 2)

                    for det in detections:
                        bbox = det.get("bbox", {})
                        x_min_norm = float(bbox.get("x_min", 0))
                        y_min_norm = float(bbox.get("y_min", 0))
                        x_max_norm = float(bbox.get("x_max", 0))
                        y_max_norm = float(bbox.get("y_max", 0))

                        x1 = int(x_min_norm * PIP_W)
                        y1 = int(y_min_norm * PIP_H)
                        x2 = int(x_max_norm * PIP_W)
                        y2 = int(y_max_norm * PIP_H)

                        cls_name = det.get("class", "obj")
                        conf = float(det.get("score", 0.0))

                        if x2 <= x1 or y2 <= y1:
                            continue

                        cv2.rectangle(rear_resized, (x1, y1), (x2, y2), (0, 255, 0), 2)

                        font = cv2.FONT_HERSHEY_SIMPLEX
                        font_scale = 0.6
                        thickness = 2

                        h_pip, w_pip, _ = rear_resized.shape
                        cx = (x1 + x2) // 2

                        # 클래스네임: 박스 위 중앙
                        class_label = cls_name
                        (cw, ch), _ = cv2.getTextSize(class_label, font, font_scale, thickness)
                        class_x = cx - cw // 2
                        class_y = y1 - 5
                        class_x = max(0, min(class_x, w_pip - cw))
                        class_y = max(ch, min(class_y, h_pip - 1))

                        cv2.putText(
                            rear_resized,
                            class_label,
                            (class_x, class_y),
                            font,
                            font_scale,
                            (0, 255, 0),
                            thickness,
                            cv2.LINE_AA,
                        )

                        # 점수: 박스 아래 중앙
                        score_label = f"{conf:.2f}"
                        (sw, sh), _ = cv2.getTextSize(score_label, font, font_scale, thickness)
                        score_x = cx - sw // 2
                        score_y = y2 + sh + 5
                        score_x = max(0, min(score_x, w_pip - sw))
                        score_y = max(sh, min(score_y, h_pip - 1))

                        cv2.putText(
                            rear_resized,
                            score_label,
                            (score_x, score_y),
                            font,
                            font_scale,
                            (0, 255, 0),
                            thickness,
                            cv2.LINE_AA,
                        )
                        
                        # rear 방향 판정 (normalized 기준)
                        center_norm = 0.5 * (x_min_norm + x_max_norm)
                        deadband = 0.05  # 중앙 ±5% 구간은 화살표 없음

                        if center_norm < 0.5 - deadband:
                            last_rear_left_alert_time = now
                        elif center_norm > 0.5 + deadband:
                            last_rear_right_alert_time = now

                    pip_img = rear_resized
                    last_rear_detection_time = now

                else:
                    # 디텍션이 없어도 최근 N초 안이면 PIP는 계속 보이게 (박스 없이 실시간 영상)
                    if (now - last_rear_detection_time) < REAR_PIP_HOLD_SEC:
                        pip_img = rear_resized

                # PIP를 HUD 디스플레이와 스트리밍 양쪽에 합성
                if pip_img is not None:
                    y_end = min(PIP_Y + PIP_H, SCREEN_H)
                    x_end = min(PIP_X + PIP_W, SCREEN_W)
                    pip_h_eff = y_end - PIP_Y
                    pip_w_eff = x_end - PIP_X

                    if pip_h_eff > 0 and pip_w_eff > 0:
                        front_canvas_fs[PIP_Y:y_end, PIP_X:x_end] = pip_img[:pip_h_eff, :pip_w_eff]
                        stream_canvas[PIP_Y:y_end, PIP_X:x_end] = pip_img[:pip_h_eff, :pip_w_eff]
                    
                    # rear 방향 화살표: 양쪽 캔버스 모두에 그리기
                    h_f, w_f, _ = front_canvas_fs.shape
                    mid_y = h_f // 2

                    arrow_len = int(min(w_f, h_f) * 0.18)
                    half_w = arrow_len // 2
                    half_h = arrow_len // 3
                    arrow_thick = 4
                    arrow_color = (0, 0, 255)
                    margin = 20

                    left_age = now - last_rear_left_alert_time
                    right_age = now - last_rear_right_alert_time

                    left_active = left_age < ARROW_ALERT_HOLD_SEC
                    right_active = right_age < ARROW_ALERT_HOLD_SEC

                    if left_active:
                        phase = left_age % ARROW_BLINK_PERIOD
                        left_draw_on = phase < (ARROW_BLINK_PERIOD / 2.0)
                    else:
                        left_draw_on = False

                    if right_active:
                        phase = right_age % ARROW_BLINK_PERIOD
                        right_draw_on = phase < (ARROW_BLINK_PERIOD / 2.0)
                    else:
                        right_draw_on = False

                    # ← 왼쪽 화살표
                    if left_draw_on:
                        cx = margin + half_w
                        cy = mid_y
                        pts_left = np.array([
                            [cx + half_w, cy - half_h],
                            [cx + half_w, cy + half_h],
                            [cx - half_w, cy],
                        ], dtype=np.int32)

                        for canvas in [front_canvas_fs, stream_canvas]:
                            cv2.polylines(
                                canvas,
                                [pts_left],
                                isClosed=True,
                                color=arrow_color,
                                thickness=arrow_thick,
                                lineType=cv2.LINE_AA,
                            )

                    # → 오른쪽 화살표
                    if right_draw_on:
                        cx = w_f - margin - half_w
                        cy = mid_y
                        pts_right = np.array([
                            [cx - half_w, cy - half_h],
                            [cx - half_w, cy + half_h],
                            [cx + half_w, cy],
                        ], dtype=np.int32)

                        for canvas in [front_canvas_fs, stream_canvas]:
                            cv2.polylines(
                                canvas,
                                [pts_right],
                                isClosed=True,
                                color=arrow_color,
                                thickness=arrow_thick,
                                lineType=cv2.LINE_AA,
                            )
                        
            # 좌우 반전 + 180도 회전 (HUD용)
            # HUD 디스플레이용: 좌우 반전 + 180도 회전
            hud_flipped = cv2.flip(front_canvas_fs, 1)
            hud_flipped = cv2.rotate(hud_flipped, cv2.ROTATE_180)

            cv2.imshow("Front HUD", hud_flipped)

            # ★ 50055 스트리밍용: 실제 카메라 배경 위 UI를 JPEG로 인코딩
            stream_flipped = cv2.flip(stream_canvas, 1)
            stream_flipped = cv2.rotate(stream_flipped, cv2.ROTATE_180)
             
            try:
                ok, enc = cv2.imencode('.jpg', stream_flipped, [int(cv2.IMWRITE_JPEG_QUALITY), 85])
                if ok:
                    jpeg_bytes = enc.tobytes()
                    ts_ms = int(time.time() * 1000)
                    with _hud_subscribers_lock:
                        for q in list(_hud_subscribers):
                            try:
                                q.put_nowait((jpeg_bytes, ts_ms))
                            except queue.Full:
                                pass
            except Exception as e:
                print(f"[HUD] Failed to encode/broadcast HUD frame: {e}")

            if cv2.waitKey(1) & 0xFF == 27:
                break
    finally:
        # 정리
        if hud_server:
            hud_server.stop(0)
            print("[HUD-SRV] stopped")
        front_transmission.stop_streaming()
        if rear_camera_available and rear_transmission:
            rear_transmission.stop_streaming()

        front_stats = front_transmission.get_statistics()
        print("\n" + "=" * 60)
        print("FRONT Camera Statistics:")
        print(f"  Duration: {front_stats['duration_sec']:.2f}s")
        print(f"  Avg FPS: {front_stats['avg_fps_received']:.2f}")

        if rear_camera_available and rear_transmission:
            rear_stats = rear_transmission.get_statistics()
            print("\nREAR Camera Statistics:")
            print(f"  Duration: {rear_stats['duration_sec']:.2f}s")
            print(f"  Avg FPS: {rear_stats['avg_fps_received']:.2f}")
        print("=" * 60 + "\n")

        front_transmission.disconnect()
        if rear_camera_available and rear_transmission:
            rear_transmission.disconnect()
        front_cam.close()
        if rear_camera_available and rear_cam:
            rear_cam.close()
        cv2.destroyAllWindows()
        print("[Main] Shutdown complete")


if __name__ == "__main__":
    main()