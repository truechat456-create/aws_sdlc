#!/usr/bin/env python3
"""End-to-end smoke test for FR-9: fake bridge server + the shipped test client.

Starts a fake mower_wifi_bridge server (reusing the real ws_server + protocol
modules, with ROS service calls stubbed) and then runs the actual bundled
scripts/wifi_test_client.py against it to prove the shipped client works and the
full path behaves: FR-9.1 connectivity, FR-9.5 auth (accept + reject), FR-9.2
START relay, FR-9.4 EMERGENCY relay, FR-9.6 1 Hz status.

No ROS required. Exit 0 on success.
"""

import json
import os
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join(ROOT, "open_mower_ros", "src", "mower_wifi_bridge")
sys.path.insert(0, os.path.join(PKG, "src"))

from mower_wifi_bridge.protocol import CommandResult, authorize, map_command, status_to_dict  # noqa: E402
from mower_wifi_bridge.ws_server import WebSocketServer  # noqa: E402

TOKEN = "e2e-smoke-token"


class FakeStatus(object):
    state = 2
    state_name = "AUTONOMOUS"
    sub_state_name = "MOWING"
    battery_percent = 76.0
    gps_quality_percent = 95.0
    is_charging = False
    emergency = False


def start_fake_bridge(port):
    received = {"high_level": [], "emergency": 0}

    def on_message(client, text):
        try:
            msg = json.loads(text)
        except ValueError:
            client.send_text(json.dumps({"error": "bad_json"}))
            return
        if not authorize(msg, TOKEN):  # FR-9.5
            client.send_text(json.dumps({"error": "unauthorized"}))
            return
        result = map_command(msg)
        if result.kind == CommandResult.ERROR:
            client.send_text(json.dumps({"error": result.error}))
            return
        if result.kind == CommandResult.EMERGENCY:  # FR-9.4
            received["emergency"] += 1
            client.send_text(json.dumps({"ok": True, "command": msg.get("command"), "detail": "emergency"}))
        else:  # FR-9.2 / FR-9.3 / SR-7
            received["high_level"].append(result.command)
            client.send_text(json.dumps({"ok": True, "command": msg.get("command"), "detail": result.command}))

    server = WebSocketServer(host="127.0.0.1", port=port, on_message=on_message)
    server.start()

    stop = threading.Event()

    def pump():  # FR-9.6 1 Hz status
        while not stop.is_set():
            server.broadcast(json.dumps(status_to_dict(FakeStatus(), stamp=time.time())))
            time.sleep(1.0)

    t = threading.Thread(target=pump)
    t.daemon = True
    t.start()
    return server, received, stop


def free_port():
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def run_client(port, extra_args):
    client = os.path.join(PKG, "scripts", "wifi_test_client.py")
    cmd = [sys.executable, client, "--host", "127.0.0.1", "--port", str(port), "--token", TOKEN] + extra_args
    out = subprocess.check_output(cmd, stderr=subprocess.STDOUT, timeout=30).decode()
    return out


def main():
    failures = []

    def check(cond, msg):
        print(("  ok:   " if cond else "  FAIL: ") + msg)
        if not cond:
            failures.append(msg)

    port = free_port()
    server, received, stop = start_fake_bridge(port)
    try:
        time.sleep(0.3)

        print("e2e: START relay + status (FR-9.1/9.2/9.6)")
        out = run_client(port, ["--command", "START", "--listen", "2.5"])
        check("connected to ws://127.0.0.1:%d" % port in out, "FR-9.1 client connected")
        check('"ok": true' in out.lower() or "ACK" in out, "FR-9.2 START acknowledged")
        check(received["high_level"] and received["high_level"][0] == 1, "FR-9.2 server relayed COMMAND_START")
        check("STATUS:" in out, "FR-9.6 client received status frames")

        print("e2e: EMERGENCY relay (FR-9.4)")
        out = run_client(port, ["--command", "EMERGENCY", "--listen", "0.5"])
        check(received["emergency"] == 1, "FR-9.4 server relayed EMERGENCY")

        print("e2e: unauthorized rejection (FR-9.5)")
        out = run_client(port, ["--command", "START", "--bad-token", "--listen", "0.5"])
        check("unauthorized" in out.lower(), "FR-9.5 bad token rejected by server")
        check(received["high_level"] == [1], "FR-9.5 rejected command caused no new state change")
    finally:
        stop.set()
        server.stop()

    print()
    if failures:
        print("e2e FAILURES: %d" % len(failures))
        return 1
    print("e2e ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
