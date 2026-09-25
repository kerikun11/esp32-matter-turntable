#!/usr/bin/env python3
"""Device-less preview of the on-device web UI (web/index.html)."""

import argparse
from dataclasses import dataclass, field, replace
import hashlib
import json
import sys
import time
from functools import lru_cache
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import re
from threading import Lock
from urllib.parse import parse_qs, urlsplit


FIRMWARE_ROOT = Path(__file__).resolve().parents[2]
TEMPLATE = FIRMWARE_ROOT / "web/index.html"
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(FIRMWARE_ROOT / "components/device_common/tools/web"))
from build_web import build, source_files
STATE_LOCK = Lock()
HOSTNAME_RE = re.compile(r"^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?$")
DEFAULT_PORT = 8000


def port_number(value: str) -> int:
    try:
        port = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("port must be an integer") from error
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError("port must be between 1 and 65535")
    return port


def parse_args():
    parser = argparse.ArgumentParser(description="Preview the turntable Web UI")
    parser.add_argument(
        "-p", "--port", type=port_number, default=DEFAULT_PORT,
        help=f"TCP port to listen on (default: {DEFAULT_PORT})",
    )
    return parser.parse_args()


@dataclass
class PreviewState:
    device_name: str = "Matter Turntable"
    hostname: str = "esp32-matter-turntable"
    on_angle: int = 180
    off_angle: int = 0
    max_speed: int = 180
    switch_on: bool = True
    # simulated servo motion: angle moves linearly from start to target
    move_start: float = 0
    move_from: float = 180
    move_to: float = 180
    status_message: str = ""
    status_is_error: bool = False
    commissioning_until: float = 0
    fabrics: list = field(default_factory=lambda: [
        {"index": 1, "label": "サンプルFabric",
         "fabric_id": "0xFEDCBA9876543210",
         "node_id": "0x1234567890ABCDEF", "vendor_id": "0xFFF1"}
    ])

    def servo(self):
        """Returns (angle, moving) at the current time."""
        distance = abs(self.move_to - self.move_from)
        duration = distance * 1.5 / max(self.max_speed, 1)
        elapsed = time.monotonic() - self.move_start
        if elapsed >= duration:
            return self.move_to, False
        t = elapsed / duration
        eased = t * t * (3 - 2 * t)
        return self.move_from + (self.move_to - self.move_from) * eased, True

    def move(self, angle: int):
        current, _ = self.servo()
        self.move_from = current
        self.move_to = angle
        self.move_start = time.monotonic()


STATE = PreviewState()


@lru_cache(maxsize=1)
def assets(mtime):
    return build(TEMPLATE)


def state_json() -> bytes:
    with STATE_LOCK:
        state = replace(STATE)
        STATE.status_message = ""
        STATE.status_is_error = False
        commissioning_open = time.monotonic() < STATE.commissioning_until
    angle, moving = state.servo()
    return json.dumps({
        "switch": state.switch_on, "moving": moving, "powered": moving,
        "angle": round(angle), "target_angle": round(state.move_to),
        "commissioned": bool(state.fabrics),
        "commissioning_open": commissioning_open,
        "device_name": state.device_name, "hostname": state.hostname,
        "on_angle": state.on_angle, "off_angle": state.off_angle,
        "max_speed": state.max_speed,
        "message": state.status_message, "error": state.status_is_error,
        "reboot": False, "preview": True,
    }, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def encoding_quality(header, coding):
    qualities = {}
    for item in header.split(","):
        name, _, parameter = item.strip().partition(";")
        q = 1.0
        if parameter:
            try:
                q = float(parameter.strip().removeprefix("q="))
                if not 0 <= q <= 1: q = 0.0
            except ValueError:
                q = 0.0
        qualities[name.lower()] = q
    if coding in qualities: return qualities[coding]
    if coding == "identity": return 0 if qualities.get("*") == 0 else 1
    return qualities.get("*", 0)


def set_status(message: str, is_error: bool = False):
    STATE.status_message = message
    STATE.status_is_error = is_error


def parse_int(form, key):
    text = form.get(key, [""])[0]
    return int(text) if re.fullmatch(r"-?\d{1,6}", text) else None


class PreviewHandler(BaseHTTPRequestHandler):
    def send_html(self):
        plain, compressed = assets(tuple(p.stat().st_mtime_ns for p in source_files(TEMPLATE)))
        header = self.headers.get("Accept-Encoding", "")
        gzip = encoding_quality(header, "gzip") > 0
        if not gzip and encoding_quality(header, "identity") == 0:
            self.send_error(406)
            return
        content = compressed if gzip else plain
        etag = '"' + hashlib.sha256(content).hexdigest()[:24] + '"'
        matches = [item.strip().removeprefix("W/") for item in
                   self.headers.get("If-None-Match", "").split(",")]
        unchanged = etag in matches or "*" in matches
        self.send_response(304 if unchanged else 200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Vary", "Accept-Encoding")
        self.send_header("ETag", etag)
        if gzip: self.send_header("Content-Encoding", "gzip")
        if not unchanged: self.send_header("Content-Length", str(len(content)))
        self.end_headers()
        if not unchanged: self.wfile.write(content)

    def send_json(self, content: bytes):
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(content)))
        self.end_headers()
        self.wfile.write(content)

    def respond_mutation(self):
        if "application/json" in self.headers.get("Accept", ""):
            self.send_json(state_json())
        else:
            self.send_response(303)
            self.send_header("Location", "/")
            self.send_header("Content-Length", "0")
            self.end_headers()

    def read_form(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8")
        return parse_qs(body, keep_blank_values=True)

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/": self.send_html()
        elif path == "/state": self.send_json(state_json())
        elif path == "/device-info":
            with STATE_LOCK:
                fabrics = [dict(fabric) for fabric in STATE.fabrics]
                commissioning_open = time.monotonic() < STATE.commissioning_until
            self.send_json(json.dumps({
                "project_name": "esp32-matter-turntable", "version": "preview",
                "build_date": "Jan  1 2026 00:00:00", "idf_version": "v5.5.5",
                "uptime_seconds": 93784, "reset_reason": "電源投入",
                "free_heap": 81234, "min_free_heap": 60312,
                "largest_free_block": 45056, "connected": True,
                "ssid": "Preview Wi-Fi", "rssi": -48, "channel": 6,
                "mac": "02:00:00:00:00:01",
                "ipv4": "192.0.2.10", "ipv6": ["2001:db8::10", "fe80::10"],
                "manual_code": "34970112332",
                "qr_payload": "MT:Y.K9042C00KA0648G00",
                "fabrics": fabrics, "commissioning_open": commissioning_open,
            }, ensure_ascii=False).encode("utf-8"))
        elif path == "/version":
            self.send_json(json.dumps({
                "project_name": "esp32-matter-turntable", "version": "preview",
                "idf_ver": "v5.5.5", "date": "Jan  1 2026", "time": "00:00:00",
            }).encode("utf-8"))
        else: self.send_error(404)

    def do_POST(self):
        form = self.read_form()
        path = urlsplit(self.path).path
        handlers = {
            "/settings": self.handle_settings, "/action": self.handle_action,
            "/move": self.handle_move, "/matter": self.handle_matter,
            "/reboot": self.handle_reboot,
        }
        if path not in handlers:
            self.send_error(404)
            return
        with STATE_LOCK:
            handlers[path](form)
        self.respond_mutation()

    def handle_settings(self, form):
        device_name = form.get("device_name", [""])[0].strip()
        hostname = form.get("hostname", [""])[0].strip()
        on_angle = parse_int(form, "on_angle")
        off_angle = parse_int(form, "off_angle")
        max_speed = parse_int(form, "max_speed")
        if not device_name or len(device_name.encode("utf-8")) > 64:
            set_status("デバイス名は1〜64バイトで入力してください。設定は保存されませんでした。", True)
            return
        if not HOSTNAME_RE.match(hostname):
            set_status("ホスト名は英数字とハイフン（先頭・末尾以外）で63文字以内にしてください。設定は保存されませんでした。", True)
            return
        if (on_angle is None or off_angle is None or max_speed is None
                or not 0 <= on_angle <= 180 or not 0 <= off_angle <= 180
                or not 1 <= max_speed <= 720):
            set_status("角度・速度の入力内容を確認してください。設定は保存されませんでした。", True)
            return
        STATE.device_name = device_name
        STATE.hostname = hostname
        STATE.on_angle = on_angle
        STATE.off_angle = off_angle
        STATE.max_speed = max_speed
        set_status("設定を保存しました。角度は次回のON/OFF操作から反映されます。")

    def handle_action(self, form):
        value = form.get("state", [""])[0]
        if value not in ("on", "off"):
            set_status("操作内容が不正です。", True)
            return
        STATE.switch_on = value == "on"
        STATE.move(STATE.on_angle if STATE.switch_on else STATE.off_angle)
        set_status(f"ターンテーブルを{'ON' if STATE.switch_on else 'OFF'}にしました。")

    def handle_move(self, form):
        angle = parse_int(form, "angle")
        if angle is None or not 0 <= angle <= 180:
            set_status("角度は0〜180度で指定してください。", True)
            return
        STATE.move(angle)
        set_status(f"試し動作：{angle}度に移動します。ON/OFFの状態は変わりません。")

    def handle_matter(self, form):
        action = form.get("action", [""])[0]
        if action == "commission":
            if time.monotonic() < STATE.commissioning_until:
                set_status("ペアリング受付はすでに開始されています。")
            else:
                STATE.commissioning_until = time.monotonic() + 300
                set_status("ペアリング受付を開始しました（最大5分間）。")
        elif action == "remove":
            target = next((f for f in STATE.fabrics
                           if str(f["index"]) == form.get("index", [""])[0]
                           and all(f[key] == form.get(key, [""])[0]
                                   for key in ("fabric_id", "node_id", "vendor_id"))), None)
            if target is None:
                set_status("削除対象が見つからないか、登録情報が変わっています。一覧を確認してください。", True)
                return
            STATE.fabrics.remove(target)
            message = f"Fabric #{target['index']}を削除しました。Wi-Fi接続は維持されます。"
            if not STATE.fabrics and time.monotonic() >= STATE.commissioning_until:
                message += "再登録するにはペアリング受付を開始してください。"
            set_status(message)
        else:
            set_status("Matterの操作内容が不正です。", True)

    def handle_reboot(self, form):
        set_status("再起動しています。プレビューでは再起動を省略します。")

    def log_message(self, format, *args):
        print(format % args)


if __name__ == "__main__":
    args = parse_args()
    address = ("127.0.0.1", args.port)
    print(f"Web UI preview: http://{address[0]}:{address[1]}")
    ThreadingHTTPServer(address, PreviewHandler).serve_forever()
