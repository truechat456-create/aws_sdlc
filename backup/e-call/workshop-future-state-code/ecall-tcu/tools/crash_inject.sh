#!/usr/bin/env bash
#
# crash_inject.sh - inject a synthetic crash trigger for the eCall TCU.
#
# Two modes:
#   auto    - send CAN frame 0A0#01... on vcan0 (airbag deployed)
#   manual  - send UDP "SOS" to 127.0.0.1:10111
#
# All data is synthetic. Requires can-utils (cansend) for auto mode and a
# running vcan0 interface:
#
#   sudo modprobe vcan
#   sudo ip link add dev vcan0 type vcan
#   sudo ip link set up vcan0
#
set -euo pipefail

MODE="${1:-auto}"
CAN_IF="${CAN_IF:-vcan0}"
CAN_ID="${CAN_ID:-0A0}"
SOS_HOST="${SOS_HOST:-127.0.0.1}"
SOS_PORT="${SOS_PORT:-10111}"

usage() {
    echo "usage: $0 [auto|manual]"
    echo "  auto   - CAN crash frame ${CAN_ID}#01.. on ${CAN_IF}"
    echo "  manual - UDP 'SOS' to ${SOS_HOST}:${SOS_PORT}"
}

case "${MODE}" in
    auto)
        if ! command -v cansend >/dev/null 2>&1; then
            echo "error: cansend not found (install can-utils)" >&2
            exit 1
        fi
        # byte0 = 0x01 (airbag deployed), remaining bytes 0
        echo "injecting AUTOMATIC crash: ${CAN_IF} ${CAN_ID}#0100000000000000"
        cansend "${CAN_IF}" "${CAN_ID}#0100000000000000"
        ;;
    manual)
        echo "injecting MANUAL SOS: udp ${SOS_HOST}:${SOS_PORT}"
        # -u UDP, -w1 wait 1s then close
        printf 'SOS' | nc -u -w1 "${SOS_HOST}" "${SOS_PORT}"
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage
        exit 1
        ;;
esac
