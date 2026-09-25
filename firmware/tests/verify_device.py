#!/usr/bin/env python3
"""Black-box HTTP verification for a running esp32-matter-turntable device.

Covers what can be checked without a Matter controller or physical access:
the on-device HTTP API (/version, /, /state, /device-info, /action, /move,
/settings). The servo really moves during this run, and the ON/OFF state
and settings are restored at the end.

Not covered (needs a human / real hardware / a Matter controller):
  - the turntable physically reaching the requested angle
  - Matter-app-originated (Google Home / Alexa) commands and their reports
  - /matter (fabric removal, commissioning) and /reboot
  - an actual /update OTA flash (slow, and reboots the device)

Usage:
    python3 firmware/tests/verify_device.py --host esp32-matter-turntable.local
    python3 firmware/tests/verify_device.py --host 192.168.0.60 -v
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from typing import Callable
from urllib.parse import urlencode


class Failure(Exception):
    pass


class Device:
    def __init__(self, base_url: str, timeout: float, verbose: bool):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.verbose = verbose

    def _log(self, msg: str) -> None:
        if self.verbose:
            print(f"    . {msg}")

    def _send(self, req: urllib.request.Request, label: str) -> tuple[int, bytes]:
        start = time.monotonic()
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                status, body = resp.status, resp.read()
        except urllib.error.HTTPError as e:
            status, body = e.code, e.read()
        self._log(f"{label} -> {status} ({time.monotonic() - start:.2f}s, {len(body)}B)")
        return status, body

    def get(self, path: str) -> tuple[int, bytes]:
        return self._send(urllib.request.Request(f"{self.base_url}{path}"), f"GET {path}")

    def post(self, path: str, fields: dict[str, str]) -> tuple[int, dict | None]:
        req = urllib.request.Request(f"{self.base_url}{path}",
                                     data=urlencode(fields).encode("utf-8"), method="POST")
        req.add_header("Content-Type", "application/x-www-form-urlencoded")
        # Without this the server answers with a 303 redirect (for non-JS
        # clients), which urllib would follow into a full page fetch.
        req.add_header("Accept", "application/json")
        status, body = self._send(req, f"POST {path} {fields}")
        return status, json.loads(body) if status == 200 else None

    def json(self, path: str) -> dict:
        status, body = self.get(path)
        if status != 200:
            raise Failure(f"GET {path} returned HTTP {status}")
        return json.loads(body.decode("utf-8"))

    def state(self) -> dict:
        return self.json("/state")

    def mutate(self, path: str, fields: dict[str, str]) -> dict:
        status, state = self.post(path, fields)
        if status != 200 or state is None:
            raise Failure(f"POST {path} {fields} -> HTTP {status}")
        return state

    def wait_idle(self, limit_s: float = 10.0) -> dict:
        deadline = time.monotonic() + limit_s
        while True:
            state = self.state()
            if not state["moving"]:
                return state
            if time.monotonic() > deadline:
                raise Failure(f"servo still moving after {limit_s}s: {state}")
            time.sleep(0.3)


Test = Callable[[Device], None]
RESULTS: list[tuple[str, bool, str]] = []


def run(name: str, device: Device, fn: Test) -> None:
    print(f"[ ] {name}")
    try:
        fn(device)
    except Failure as e:
        print(f"[FAIL] {name}: {e}")
        RESULTS.append((name, False, str(e)))
        return
    except Exception as e:  # noqa: BLE001 - report, don't crash the run
        print(f"[FAIL] {name}: unexpected {type(e).__name__}: {e}")
        RESULTS.append((name, False, f"{type(e).__name__}: {e}"))
        return
    print(f"[PASS] {name}")
    RESULTS.append((name, True, ""))


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


# --- individual tests ---------------------------------------------------


def test_version(dev: Device) -> None:
    info = dev.json("/version")
    for key in ("project_name", "version", "idf_ver", "date", "time"):
        expect(key in info, f"/version missing '{key}' field: {info}")
    expect(info["project_name"] == "esp32-matter-turntable",
           f"unexpected project_name: {info['project_name']}")


def test_root_page_and_info(dev: Device) -> None:
    status, body = dev.get("/")
    expect(status == 200 and b"/action" in body, "static page is missing the switch form")
    info = dev.json("/device-info")
    for key in ("ipv4", "ipv6", "fabrics", "commissioning_open", "free_heap", "manual_code"):
        expect(key in info, f"/device-info missing '{key}': {info}")
    expect(info["free_heap"] > 20000, f"free heap is low: {info['free_heap']}")


def test_switch_round_trip(dev: Device) -> None:
    state = dev.wait_idle()
    original = state["switch"]
    try:
        for requested in (not original, original, not original):
            state = dev.mutate("/action", {"state": "on" if requested else "off"})
            expect(state["switch"] == requested, f"requested {requested}, got {state['switch']}")
            expected_angle = state["on_angle"] if requested else state["off_angle"]
            expect(state["target_angle"] == expected_angle,
                   f"target {state['target_angle']} != {expected_angle}")
            state = dev.wait_idle()
            expect(state["angle"] == expected_angle, f"angle {state['angle']} != {expected_angle}")
            time.sleep(1.0)
            expect(dev.state()["switch"] == requested, "switch drifted without any request")
    finally:
        dev.mutate("/action", {"state": "on" if original else "off"})
        dev.wait_idle()


def test_move_keeps_switch(dev: Device) -> None:
    state = dev.wait_idle()
    try:
        state_after = dev.mutate("/move", {"angle": "90"})
        expect(state_after["switch"] == state["switch"], "/move changed the switch state")
        expect(dev.wait_idle()["angle"] == 90, "servo did not reach 90 degrees")
    finally:
        dev.mutate("/action", {"state": "on" if state["switch"] else "off"})
        dev.wait_idle()


def test_invalid_requests_are_rejected(dev: Device) -> None:
    before = dev.wait_idle()
    for path, fields in (("/action", {}), ("/action", {"state": "banana"}),
                         ("/move", {"angle": "181"}), ("/move", {"angle": "x"}),
                         ("/settings", {"device_name": "x", "hostname": "bad host",
                                        "on_angle": "1", "off_angle": "2", "max_speed": "3"})):
        state = dev.mutate(path, fields)
        expect(state["error"], f"{path} {fields} was not rejected")
    after = dev.state()
    for key in ("switch", "hostname", "on_angle", "off_angle", "max_speed"):
        expect(before[key] == after[key], f"invalid request changed {key}")


def test_burst_requests_end_in_consistent_state(dev: Device) -> None:
    original = dev.wait_idle()["switch"]
    current = original
    try:
        for _ in range(6):
            current = not current
            status, _ = dev.post("/action", {"state": "on" if current else "off"})
            expect(status in (200, 503), f"burst request -> HTTP {status}")
            time.sleep(0.2)
        time.sleep(1.0)
        expect(dev.state()["switch"] == current, "final state does not match the last request")
    finally:
        dev.mutate("/action", {"state": "on" if original else "off"})
        dev.wait_idle()


def test_settings_round_trip(dev: Device) -> None:
    original = dev.state()
    fields = {key: str(original[key]) for key in
              ("device_name", "hostname", "on_angle", "off_angle", "max_speed")}
    probe = "181" if fields["max_speed"] != "181" else "182"
    try:
        state = dev.mutate("/settings", {**fields, "max_speed": probe})
        expect(not state["error"] and state["max_speed"] == int(probe), "settings save failed")
    finally:
        state = dev.mutate("/settings", fields)
        expect(state["max_speed"] == original["max_speed"], "settings restore failed")


TESTS: list[tuple[str, Test]] = [
    ("GET /version returns expected fields", test_version),
    ("GET / and /device-info respond", test_root_page_and_info),
    ("switch ON/OFF round-trips and the servo follows", test_switch_round_trip),
    ("/move moves the servo without changing the switch", test_move_keeps_switch),
    ("invalid requests are rejected without side effects", test_invalid_requests_are_rejected),
    ("burst of requests ends in a consistent state", test_burst_requests_end_in_consistent_state),
    ("/settings round-trips and is restored", test_settings_round_trip),
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="esp32-matter-turntable.local",
                        help="device hostname or IP (default: esp32-matter-turntable.local)")
    parser.add_argument("--timeout", type=float, default=20.0,
                        help="per-request timeout in seconds (default: 20.0)")
    parser.add_argument("-v", "--verbose", action="store_true", help="log every HTTP request")
    args = parser.parse_args()

    base_url = args.host if args.host.startswith("http") else f"http://{args.host}"
    dev = Device(base_url, timeout=args.timeout, verbose=args.verbose)

    print(f"Verifying device at {base_url}\n")
    for name, fn in TESTS:
        run(name, dev, fn)
        print()

    passed = sum(1 for _, ok, _ in RESULTS if ok)
    total = len(RESULTS)
    print("=" * 60)
    print(f"{passed}/{total} passed")
    if passed != total:
        print("\nFailures:")
        for name, ok, msg in RESULTS:
            if not ok:
                print(f"  - {name}: {msg}")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
