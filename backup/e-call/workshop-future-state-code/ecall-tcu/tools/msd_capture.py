#!/usr/bin/env python3
"""msd_capture.py - capture and decode one eCall MSD datagram.

Binds a UDP socket on the PSAP address/port, waits for a single MSD datagram
(sent by ecalld on a trigger), and decodes it per docs/msd_layout.md.

The decoded record is printed as human-readable text and as JSON. Position is
reported as NOT_AVAILABLE when the latitude/longitude sentinel (0x7FFFFFFF) is
present, matching the v1 "live GNSS only, no fallback" policy. All data is
synthetic.

Usage:
    python3 msd_capture.py --host 127.0.0.1 --port 5000 --timeout 30
"""
import argparse
import json
import socket
import struct
import sys

COORD_NOT_AVAILABLE = 0x7FFFFFFF
FLAG_TEST = 0x01
FLAG_POS_TRUSTED = 0x02

ACTIVATION = {1: "AUTOMATIC", 2: "MANUAL"}


def decode_msd(buf: bytes) -> dict:
    """Decode the 44-byte big-endian MSD. Raises ValueError if too short."""
    if len(buf) < 44:
        raise ValueError(f"MSD too short: {len(buf)} bytes (need >= 44)")

    version = buf[0]
    message_id = struct.unpack_from(">I", buf, 1)[0]
    activation = buf[5]
    flags = buf[6]
    vin = buf[7:24].decode("ascii", errors="replace").rstrip()
    timestamp = struct.unpack_from(">Q", buf, 24)[0]
    lat_mas = struct.unpack_from(">i", buf, 32)[0]
    lon_mas = struct.unpack_from(">i", buf, 36)[0]
    direction = struct.unpack_from(">H", buf, 40)[0]
    passengers = buf[42]

    pos_trusted = bool(flags & FLAG_POS_TRUSTED)
    not_available = (lat_mas == COORD_NOT_AVAILABLE
                     or lon_mas == COORD_NOT_AVAILABLE)

    if not_available:
        lat_deg = None
        lon_deg = None
        position = "NOT_AVAILABLE"
    else:
        lat_deg = lat_mas / 3_600_000.0
        lon_deg = lon_mas / 3_600_000.0
        position = f"{lat_deg:.6f}, {lon_deg:.6f}"

    return {
        "size_bytes": len(buf),
        "version": version,
        "message_id": message_id,
        "activation": ACTIVATION.get(activation, f"UNKNOWN({activation})"),
        "test_call": bool(flags & FLAG_TEST),
        "position_trusted": pos_trusted,
        "vin": vin,
        "timestamp_utc": timestamp,
        "latitude_mas": lat_mas,
        "longitude_mas": lon_mas,
        "position": position,
        "direction_deg": direction,
        "passengers": passengers,
        "raw_hex": buf[:44].hex(),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="Capture and decode an MSD")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--timeout", type=float, default=30.0)
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.host, args.port))
    sock.settimeout(args.timeout)

    print(f"listening for MSD on udp {args.host}:{args.port} "
          f"(timeout {args.timeout}s)", file=sys.stderr)

    try:
        data, src = sock.recvfrom(256)
    except socket.timeout:
        print("TIMEOUT: no MSD received", file=sys.stderr)
        return 2
    finally:
        sock.close()

    print(f"received {len(data)} bytes from {src[0]}:{src[1]}",
          file=sys.stderr)
    decoded = decode_msd(data)

    print("=== Decoded MSD ===")
    for k in ("size_bytes", "version", "message_id", "activation",
              "test_call", "position_trusted", "vin", "timestamp_utc",
              "latitude_mas", "longitude_mas", "position", "direction_deg",
              "passengers", "raw_hex"):
        print(f"{k:18}: {decoded[k]}")
    print("=== JSON ===")
    print(json.dumps(decoded, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
