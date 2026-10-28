#!/usr/bin/env python3
"""Self-contained verification for the FR-9 WiFi bridge host-side logic.

Runs WITHOUT ROS (ROS runs only inside the OpenMower container). It exercises:

  - protocol.authorize           FR-9.5 token auth (accept/reject, constant time)
  - protocol.map_command         FR-9.2/9.3/9.4/SR-7 command mapping
  - protocol.status_to_dict      FR-9.6 status serialization
  - ws_server.WebSocketServer    FR-9.1 real WebSocket handshake + framing, and
                                 FR-9.6 1 Hz broadcast, driven by the test client.

The ROS service calls are stubbed by a FakeExecutor that records what the real
node would call, so the full WebSocket->auth->map->execute path is validated.

Exit code 0 == all checks passed.
"""

import json
import os
import socket
import struct
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
sys.path.insert(0, os.path.abspath(SRC))

from mower_wifi_bridge.protocol import (  # noqa: E402
    COMMAND_START,
    COMMAND_HOME,
    COMMAND_RESET_EMERGENCY,
    CommandResult,
    authorize,
    map_command,
    status_to_dict,
)
from mower_wifi_bridge.ws_server import WebSocketServer  # noqa: E402

FAILURES = []


def check(cond, msg):
    if cond:
        print("  ok:   %s" % msg)
    else:
        print("  FAIL: %s" % msg)
        FAILURES.append(msg)


# --------------------------------------------------------------------------- #
# Unit tests: pure protocol logic
# --------------------------------------------------------------------------- #
def test_protocol():
    print("protocol (FR-9.2..9.6, SR-7, FR-9.5):")
    token = "s3cret"

    check(authorize({"token": "s3cret"}, token) is True, "FR-9.5 correct token accepted")
    check(authorize({"token": "wrong"}, token) is False, "FR-9.5 wrong token rejected")
    check(authorize({}, token) is False, "FR-9.5 missing token rejected")
    check(authorize({"token": "x"}, "") is False, "FR-9.5 empty configured token rejects all")

    check(map_command({"command": "START"}) == CommandResult(CommandResult.HIGH_LEVEL, command=COMMAND_START),
          "FR-9.2 START -> COMMAND_START")
    check(map_command({"command": "STOP"}) == CommandResult(CommandResult.HIGH_LEVEL, command=COMMAND_HOME),
          "FR-9.3 STOP -> COMMAND_HOME")
    check(map_command({"command": "EMERGENCY"}) == CommandResult(CommandResult.EMERGENCY),
          "FR-9.4 EMERGENCY -> emergency service")
    check(map_command({"command": "RESET_EMERGENCY"})
          == CommandResult(CommandResult.HIGH_LEVEL, command=COMMAND_RESET_EMERGENCY),
          "SR-7 RESET_EMERGENCY -> COMMAND_RESET_EMERGENCY")
    check(map_command({"command": "NOPE"}).kind == CommandResult.ERROR, "unknown command -> error")

    class FakeStatus(object):
        state = 2
        state_name = "MOWING"
        sub_state_name = ""
        battery_percent = 83.0
        gps_quality_percent = 97.0
        is_charging = False
        emergency = False

    d = status_to_dict(FakeStatus(), stamp=1730000000.0)
    check(d["state"] == 2 and d["state_name"] == "MOWING", "FR-9.6 status fields serialized")
    check(d["battery_percent"] == 83.0 and d["gps_quality_percent"] == 97.0, "FR-9.6 battery/gps serialized")
    check(d["stamp"] == 1730000000.0, "FR-9.6 stamp serialized")


# --------------------------------------------------------------------------- #
# Minimal WebSocket client (mirror of scripts/wifi_test_client.py essentials)
# --------------------------------------------------------------------------- #
import base64  # noqa: E402

_WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def ws_connect(host, port):
    sock = socket.create_connection((host, port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    sock.sendall(
        ("GET / HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
         "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (host, port, key)).encode()
    )
    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += sock.recv(1024)
    assert b"101" in resp.split(b"\r\n", 1)[0], resp
    return sock


def ws_send(sock, text):
    data = text.encode("utf-8")
    mask = os.urandom(4)
    header = bytearray([0x81, 0x80 | len(data)])
    header += mask
    sock.sendall(bytes(header) + bytes(data[i] ^ mask[i % 4] for i in range(len(data))))


def ws_recv(sock, timeout=2.0):
    sock.settimeout(timeout)
    try:
        h = sock.recv(2)
    except socket.timeout:
        return None
    if len(h) < 2:
        return None
    length = h[1] & 0x7F
    if length == 126:
        length = struct.unpack(">H", sock.recv(2))[0]
    payload = b""
    while len(payload) < length:
        payload += sock.recv(length - len(payload))
    return payload.decode("utf-8")


# --------------------------------------------------------------------------- #
# Integration test: real WebSocket server + auth + command execution + 1 Hz pump
# --------------------------------------------------------------------------- #
class FakeExecutor(object):
    """Stands in for the ROS service calls the node would make."""

    def __init__(self):
        self.high_level_commands = []
        self.emergencies = 0
        self.lock = threading.Lock()

    def execute(self, result):
        with self.lock:
            if result.kind == CommandResult.EMERGENCY:
                self.emergencies += 1
                return True, "emergency_asserted"
            if result.kind == CommandResult.HIGH_LEVEL:
                self.high_level_commands.append(result.command)
                return True, "command=%d" % result.command
        return False, "unhandled"


def test_server():
    print("server (FR-9.1 WebSocket, auth path, FR-9.6 broadcast):")
    token = "integration-token"
    executor = FakeExecutor()

    def on_message(client, text):
        try:
            msg = json.loads(text)
        except ValueError:
            client.send_text(json.dumps({"error": "bad_json"}))
            return
        if not authorize(msg, token):  # FR-9.5
            client.send_text(json.dumps({"error": "unauthorized"}))
            return
        result = map_command(msg)
        if result.kind == CommandResult.ERROR:
            client.send_text(json.dumps({"error": result.error}))
            return
        ok, detail = executor.execute(result)
        client.send_text(json.dumps({"ok": ok, "command": msg.get("command"), "detail": detail}))

    server = WebSocketServer(host="127.0.0.1", port=0, on_message=on_message)
    # bind to an ephemeral port then read it back
    server.port = _free_port()
    server.start()
    try:
        # start the 1 Hz status pump equivalent (FR-9.6)
        stop = threading.Event()

        class FakeStatus(object):
            state = 1
            state_name = "IDLE"
            sub_state_name = ""
            battery_percent = 50.0
            gps_quality_percent = 88.0
            is_charging = True
            emergency = False

        def pump():
            while not stop.is_set():
                server.broadcast(json.dumps(status_to_dict(FakeStatus(), stamp=time.time())))
                time.sleep(1.0)

        pump_thread = threading.Thread(target=pump)
        pump_thread.daemon = True

        # --- valid START (FR-9.2) ---
        c = ws_connect("127.0.0.1", server.port)
        ws_send(c, json.dumps({"token": token, "command": "START"}))
        ack = json.loads(ws_recv(c))
        check(ack.get("ok") is True and ack.get("detail") == "command=%d" % COMMAND_START,
              "FR-9.2 START acknowledged over WebSocket")
        check(executor.high_level_commands == [COMMAND_START], "FR-9.2 executor received COMMAND_START")

        # --- EMERGENCY (FR-9.4) ---
        ws_send(c, json.dumps({"token": token, "command": "EMERGENCY"}))
        ack = json.loads(ws_recv(c))
        check(ack.get("ok") is True and executor.emergencies == 1, "FR-9.4 EMERGENCY executed")

        # --- unauthorized (FR-9.5) ---
        ws_send(c, json.dumps({"token": "nope", "command": "START"}))
        ack = json.loads(ws_recv(c))
        check(ack.get("error") == "unauthorized", "FR-9.5 wrong token -> unauthorized over WebSocket")
        check(executor.high_level_commands == [COMMAND_START], "FR-9.5 rejected command caused no state change")

        # --- status broadcast at ~1 Hz (FR-9.6) ---
        pump_thread.start()
        t0 = time.time()
        stamps = []
        while time.time() - t0 < 2.5 and len(stamps) < 3:
            frame = ws_recv(c, timeout=1.5)
            if frame is None:
                continue
            obj = json.loads(frame)
            if "state" in obj:
                stamps.append(obj["stamp"])
        stop.set()
        check(len(stamps) >= 2, "FR-9.6 received >=2 status frames")
        if len(stamps) >= 2:
            dt = stamps[1] - stamps[0]
            check(0.5 <= dt <= 1.8, "FR-9.6 status interval ~1 s (got %.2fs)" % dt)

        c.close()
    finally:
        server.stop()


def _free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main():
    print("=== FR-9 WiFi bridge host-side verification ===")
    test_protocol()
    test_server()
    print()
    if FAILURES:
        print("FAILURES: %d" % len(FAILURES))
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
