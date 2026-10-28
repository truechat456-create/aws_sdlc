#!/usr/bin/env bash
#
# validate.sh - Run all FR-9 WiFi remote start/stop verification tests.
#
# Runs:
#   1. Firmware WiFi module native test  (FR-9.2..9.6, FR-9.5, CRC, 1 Hz pump)
#   2. Host-side ROS bridge logic test   (WebSocket handshake, auth, command map,
#                                         execution path, 1 Hz broadcast)
#   3. Live end-to-end smoke test        (fake server + bundled test client)
#
# Exits non-zero if any stage fails.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FW_DIR="$ROOT/OpenMower/firmware/fw-openmower-v2"
PKG="$ROOT/open_mower_ros/src/mower_wifi_bridge"
BUILD_OUT="$ROOT/build-out"
mkdir -p "$BUILD_OUT"

pass=0
fail=0
run() {
  local name="$1"; shift
  echo "---- $name ----"
  if "$@"; then
    echo "PASS: $name"; pass=$((pass+1))
  else
    echo "FAIL: $name"; fail=$((fail+1))
  fi
  echo
}

# --- 1. Firmware WiFi module native test -------------------------------------
build_and_run_fw() {
  g++ -std=c++17 -Wall -Wextra -O2 \
      -o "$BUILD_OUT/wifi_fw_test" \
      "$FW_DIR/tools/wifi_firmware_test.cpp" \
      "$FW_DIR/src/wifi/wifi_module.cpp"
  "$BUILD_OUT/wifi_fw_test"
}
run "firmware WiFi module (G++)" build_and_run_fw

# --- 2. Host-side ROS bridge logic test --------------------------------------
run "host-side bridge logic (WebSocket+auth+status)" \
    python3 "$PKG/test/test_wifi_bridge.py"

# --- 3. Live end-to-end smoke test (fake server + bundled test client) -------
run "end-to-end smoke test (test client)" \
    python3 "$ROOT/scripts/e2e_smoke.py"

echo "================================================"
echo "FR-9 validation summary: $pass passed, $fail failed"
echo "================================================"
[ "$fail" -eq 0 ]
