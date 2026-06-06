#!/usr/bin/env python3

from dataclasses import dataclass, replace
from html import escape
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Lock
from urllib.parse import parse_qs, urlsplit


FIRMWARE_ROOT = Path(__file__).resolve().parents[2]
TEMPLATE = FIRMWARE_ROOT / "main/servo_web_page.inc"
STATE_LOCK = Lock()


@dataclass
class PreviewState:
    device_name: str = "Matter Servo"
    on_angle: int = 180
    off_angle: int = 0
    max_speed: int = 180
    switch_on: bool = True
    status_message: str = ""
    status_is_error: bool = False


STATE = PreviewState()


def read_template() -> str:
    source = TEMPLATE.read_text(encoding="utf-8").strip()
    prefix = 'R"HTML('
    suffix = ')HTML"'
    if not source.startswith(prefix) or not source.endswith(suffix):
        raise RuntimeError(f"Unexpected template format: {TEMPLATE}")
    return source[len(prefix) : -len(suffix)]


def status_notice(message: str, is_error: bool) -> str:
    if not message:
        return ""
    kind = "error" if is_error else "success"
    return f'<div class="status {kind}">{escape(message)}</div>'


def consume_state() -> PreviewState:
    with STATE_LOCK:
        state = replace(STATE)
        STATE.status_message = ""
        STATE.status_is_error = False
    return state


def render() -> bytes:
    state = consume_state()
    values = {
        "{{DEVICE_NAME}}": escape(state.device_name, quote=True),
        "{{STATUS_NOTICE}}": status_notice(
            state.status_message, state.status_is_error
        ),
        "{{SWITCH_ACTION}}": "off" if state.switch_on else "on",
        "{{SWITCH_CLASS}}": "on" if state.switch_on else "off",
        "{{SWITCH_STATE}}": "ON" if state.switch_on else "OFF",
        "{{ON_ANGLE}}": str(state.on_angle),
        "{{OFF_ANGLE}}": str(state.off_angle),
        "{{MAX_SPEED}}": str(state.max_speed),
    }

    html = read_template()
    for key, value in values.items():
        html = html.replace(key, value)
    return html.encode("utf-8")


def set_status(message: str, is_error: bool = False):
    STATE.status_message = message
    STATE.status_is_error = is_error


class PreviewHandler(BaseHTTPRequestHandler):
    def send_content(
        self, status: int, content_type: str, content: bytes
    ) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(content)))
        self.end_headers()
        self.wfile.write(content)

    def send_html(self) -> None:
        try:
            self.send_content(200, "text/html; charset=utf-8", render())
        except Exception as error:
            self.send_content(
                500,
                "text/plain; charset=utf-8",
                str(error).encode("utf-8"),
            )

    def redirect_root(self) -> None:
        self.send_response(303)
        self.send_header("Location", "/")
        self.end_headers()

    def read_form(self) -> dict[str, list[str]]:
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8")
        return parse_qs(body, keep_blank_values=True)

    def do_GET(self) -> None:
        if urlsplit(self.path).path != "/":
            self.send_content(
                404, "text/plain; charset=utf-8", "Not found".encode()
            )
            return
        self.send_html()

    def do_POST(self) -> None:
        path = urlsplit(self.path).path
        if path not in ("/settings", "/action"):
            self.send_content(
                404, "text/plain; charset=utf-8", "Not found".encode()
            )
            return

        form = self.read_form()
        with STATE_LOCK:
            if path == "/settings":
                self.handle_settings(form)
            else:
                self.handle_action(form)
        self.redirect_root()

    def handle_settings(self, form: dict[str, list[str]]) -> None:
        device_name = form.get("device_name", [""])[0].strip()
        try:
            on_angle = int(form.get("on_angle", ["-1"])[0])
            off_angle = int(form.get("off_angle", ["-1"])[0])
            max_speed = int(form.get("max_speed", ["0"])[0])
        except ValueError:
            on_angle = -1
            off_angle = -1
            max_speed = 0

        if (
            not device_name
            or len(device_name.encode("utf-8")) > 64
            or not 0 <= on_angle <= 180
            or not 0 <= off_angle <= 180
            or not 1 <= max_speed <= 720
        ):
            set_status(
                "入力内容を確認してください。設定は保存されませんでした。",
                True,
            )
            return

        STATE.device_name = device_name
        STATE.on_angle = on_angle
        STATE.off_angle = off_angle
        STATE.max_speed = max_speed
        set_status("設定を保存しました。")

    def handle_action(self, form: dict[str, list[str]]) -> None:
        state = form.get("state", [""])[0]
        if state not in ("on", "off"):
            set_status("操作内容が不正です。", True)
            return

        STATE.switch_on = state == "on"
        set_status(f"サーボを{'ON' if STATE.switch_on else 'OFF'}にしました。")

    def log_message(self, format: str, *args) -> None:
        print(format % args)


if __name__ == "__main__":
    address = ("127.0.0.1", 8000)
    print(f"Web UI preview: http://{address[0]}:{address[1]}")
    ThreadingHTTPServer(address, PreviewHandler).serve_forever()
