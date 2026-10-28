#!/usr/bin/env python3
"""gnss_sim.py - synthetic NMEA GNSS simulator for the eCall TCU.

Streams NMEA 0183 GGA + RMC sentences over UDP (default 127.0.0.1:10110),
walking a vehicle along a simple straight-line route. All data is synthetic.

The --cut-at-impact option stops emitting sentences after a given number of
seconds, simulating GNSS loss at a crash. In baseline v1 the TCU has no
fallback, so after the cut the MSD position becomes NOT_AVAILABLE.

Usage:
    python3 gnss_sim.py --host 127.0.0.1 --port 10110 --rate 1.0
    python3 gnss_sim.py --cut-at-impact 20   # stop after 20 s
"""
import argparse
import math
import socket
import time


def nmea_checksum(body: str) -> str:
    """XOR of all chars between '$' and '*', as two hex digits."""
    cs = 0
    for ch in body:
        cs ^= ord(ch)
    return f"{cs:02X}"


def to_nmea_lat(lat_deg: float) -> tuple[str, str]:
    hemi = "N" if lat_deg >= 0 else "S"
    lat = abs(lat_deg)
    deg = int(lat)
    minutes = (lat - deg) * 60.0
    return f"{deg:02d}{minutes:07.4f}", hemi


def to_nmea_lon(lon_deg: float) -> tuple[str, str]:
    hemi = "E" if lon_deg >= 0 else "W"
    lon = abs(lon_deg)
    deg = int(lon)
    minutes = (lon - deg) * 60.0
    return f"{deg:03d}{minutes:07.4f}", hemi


def build_gga(lat: float, lon: float, sats: int, hhmmss: str) -> str:
    lat_s, lat_h = to_nmea_lat(lat)
    lon_s, lon_h = to_nmea_lon(lon)
    body = (f"GPGGA,{hhmmss},{lat_s},{lat_h},{lon_s},{lon_h},"
            f"1,{sats:02d},0.9,545.4,M,46.9,M,,")
    return f"${body}*{nmea_checksum(body)}"


def build_rmc(lat: float, lon: float, speed_kn: float,
              course: float, hhmmss: str) -> str:
    lat_s, lat_h = to_nmea_lat(lat)
    lon_s, lon_h = to_nmea_lon(lon)
    body = (f"GPRMC,{hhmmss},A,{lat_s},{lat_h},{lon_s},{lon_h},"
            f"{speed_kn:05.1f},{course:05.1f},010126,,")
    return f"${body}*{nmea_checksum(body)}"


def main() -> int:
    ap = argparse.ArgumentParser(description="Synthetic NMEA GNSS simulator")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=10110)
    ap.add_argument("--rate", type=float, default=1.0,
                    help="sentences per second")
    ap.add_argument("--lat", type=float, default=48.1173,
                    help="start latitude (deg)")
    ap.add_argument("--lon", type=float, default=11.5167,
                    help="start longitude (deg)")
    ap.add_argument("--course", type=float, default=90.0,
                    help="travel direction (deg true)")
    ap.add_argument("--speed", type=float, default=50.0,
                    help="speed in km/h")
    ap.add_argument("--cut-at-impact", type=float, default=None,
                    metavar="SECONDS",
                    help="stop emitting after N seconds (GNSS loss)")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dst = (args.host, args.port)
    period = 1.0 / max(args.rate, 0.001)

    lat, lon = args.lat, args.lon
    speed_kn = args.speed / 1.852  # km/h -> knots
    # meters per tick along the course
    step_m = (args.speed * 1000.0 / 3600.0) * period
    course_rad = math.radians(args.course)

    print(f"gnss_sim -> udp {args.host}:{args.port} "
          f"@ {args.rate} Hz (synthetic data)")
    if args.cut_at_impact is not None:
        print(f"GNSS will be cut after {args.cut_at_impact} s (impact)")

    start = time.time()
    tick = 0
    try:
        while True:
            elapsed = time.time() - start
            if args.cut_at_impact is not None and elapsed >= args.cut_at_impact:
                print("--- GNSS cut (impact). No more sentences. ---")
                break

            hhmmss = time.strftime("%H%M%S", time.gmtime())
            gga = build_gga(lat, lon, 9, hhmmss)
            rmc = build_rmc(lat, lon, speed_kn, args.course, hhmmss)
            payload = (gga + "\r\n" + rmc + "\r\n").encode("ascii")
            sock.sendto(payload, dst)
            if tick % 5 == 0:
                print(f"t={elapsed:6.1f}s  {lat:.5f},{lon:.5f}")

            # advance position (approx flat-earth step)
            dlat = (step_m * math.cos(course_rad)) / 111_320.0
            dlon = (step_m * math.sin(course_rad)) / (
                111_320.0 * math.cos(math.radians(lat)))
            lat += dlat
            lon += dlon
            tick += 1
            time.sleep(period)
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
