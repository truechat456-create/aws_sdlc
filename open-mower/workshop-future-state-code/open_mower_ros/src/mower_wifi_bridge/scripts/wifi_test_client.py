#!/usr/bin/env python3
"""Test client for the FR-9 WiFi remote start/stop bridge.

A dependency-free WebSocket client (stdlib sockets only) that connects to the
mower_wifi_bridge node, authenticates (FR-9.5), sends a command, and prints
status frames (FR-9.6). Intended for manual verification against a running node
or the bundled fake server.

Examples:
    # send START to a bridge on the mower
    ./wifi_test_client.py --host 10.0.0.5 --port 8765 --token SECRET --command START

    # just watch status for 5 seconds
    ./wifi_test_client.py --host 127.0.0.1 --token SECRET --listen 5

    # trigger a remote emergency (FR-9.4)
    ./wifi_test_client.py --host 10.0.0.5 --token SECRET --command EMERGENCY
"""

import argparse
import base64
import json
import os
import socket
import struct
import sys
import time

_WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def _handshake(sock, host, port):
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    req = (
        "GET / HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n" % (host, port, key)
    )
    sock.sendall(req.encode("ascii"))
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = sock.recv(1024)
        if not chunk:
            raise RuntimeError("connection closed during handshake")
        resp += chunk
    if b"101" not in resp.split(b"\r\n", 1)[0]:
        raise RuntimeError("handshake failed: %r" % resp.split(b"\r\n", 1)[0])


def _send_text(sock, text):
    data = text.encode("utf-8")
    header = bytearray([0x81])  # FIN + text
    mask = os.urandom(4)
    length = len(data)
    if length < 126:
        header.append(0x80 | length)
    elif length < 65536:
        header.append(0x80 | 126)
        header += struct.pack(">H", length)
    else:
        header.append(0x80 | 127)
        header += struct.pack(">Q", length)
    header += mask
    masked = bytes(data[i] ^ mask[i % 4] for i in range(length))
    sock.sendall(bytes(header) + masked)


def _recv_frame(sock, timeout=None):
    sock.settimeout(timeout)
    try:
        header = _recv_exact(sock, 2)
    except socket.timeout:
        return None
    if header is None:
        return None
    length = header[1] & 0x7F
    if length == 126:
        length = struct.unpack(">H", _recv_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", _recv_exact(sock, 8))[0]
    payload = _recv_exact(sock, length) if length else b""
    if payload is None:
        return None
    return payload.decode("utf-8", "replace")


def _recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def main(argv=None):
    parser = argparse.ArgumentParser(description="FR-9 WiFi remote test client")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)  # FR-9.1 default port
    parser.add_argument("--token", default=os.environ.get("OM_WIFI_TOKEN", ""))
    parser.add_argument(
        "--command",
        choices=["START", "STOP", "EMERGENCY", "RESET_EMERGENCY"],
        help="command to send (FR-9.2/9.3/9.4/SR-7)",
    )
    parser.add_argument("--bad-token", action="store_true",
                        help="send a deliberately wrong token to verify FR-9.5 rejection")
    parser.add_argument("--listen", type=float, default=2.0,
                        help="seconds to listen for status frames after sending (FR-9.6)")
    args = parser.parse_args(argv)

    sock = socket.create_connection((args.host, args.port), timeout=10)
    _handshake(sock, args.host, args.port)
    print("connected to ws://%s:%d" % (args.host, args.port))

    token = "definitely-wrong-token" if args.bad_token else args.token

    if args.command:
        msg = {"token": token, "command": args.command}
        _send_text(sock, json.dumps(msg))
        print("-> sent %s" % args.command)

    deadline = time.time() + args.listen
    status_seen = 0
    while time.time() < deadline:
        frame = _recv_frame(sock, timeout=max(0.1, deadline - time.time()))
        if frame is None:
            continue
        try:
            obj = json.loads(frame)
        except ValueError:
            print("<- (raw) %s" % frame)
            continue
        if "error" in obj:
            print("<- ERROR: %s" % obj["error"])
        elif "ok" in obj:
            print("<- ACK: %s" % obj)
        elif "state" in obj:
            status_seen += 1
            print("<- STATUS: state=%s(%s) batt=%.0f%% gps=%.0f%% charging=%s emergency=%s"
                  % (obj.get("state"), obj.get("state_name"), obj.get("battery_percent", 0.0),
                     obj.get("gps_quality_percent", 0.0), obj.get("is_charging"), obj.get("emergency")))
        else:
            print("<- %s" % obj)

    try:
        sock.close()
    except OSError:
        pass
    print("done (%d status frame(s) received)" % status_seen)
    return 0


if __name__ == "__main__":
    sys.exit(main())
