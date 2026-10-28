#!/usr/bin/env bash
#
# build.sh - Build all FR-9 WiFi remote start/stop artifacts.
#
# Two independent build targets:
#   1. Firmware WiFi module (host G++ verification build of the RP2040 module)
#   2. ROS package mower_wifi_bridge (catkin, only when a ROS env is present)
#
# The firmware module is built natively with G++ so it can be verified on this
# machine without the Pico SDK (see OpenMower/firmware/fw-openmower-v2/main.cpp
# "Host build"). The ROS package is pure Python; its catkin build is attempted
# only if catkin_make / catkin is available, otherwise a syntax check is run.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FW_DIR="$ROOT/OpenMower/firmware/fw-openmower-v2"
ROS_WS="$ROOT/open_mower_ros"
BUILD_OUT="$ROOT/build-out"
mkdir -p "$BUILD_OUT"

echo "==> FR-9 build starting (root: $ROOT)"

# --- 0. Ensure a C++ compiler is available (install G++ if missing) ----------
if ! command -v g++ >/dev/null 2>&1; then
  echo "==> g++ not found; attempting to install..."
  if command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update -qq && sudo apt-get install -y g++
  else
    echo "!! g++ missing and no apt-get available; install a C++ compiler first." >&2
    exit 1
  fi
fi
echo "==> using $(g++ --version | head -1)"

# --- 1. Firmware WiFi module (host verification build) -----------------------
echo "==> [1/2] building firmware WiFi module (host G++)"
g++ -std=c++17 -Wall -Wextra -O2 \
    -o "$BUILD_OUT/wifi_fw_test" \
    "$FW_DIR/tools/wifi_firmware_test.cpp" \
    "$FW_DIR/src/wifi/wifi_module.cpp"
echo "    -> $BUILD_OUT/wifi_fw_test"

# also confirm the integrated firmware main.cpp compiles in host mode
g++ -std=c++17 -Wall -Wextra -O2 -c \
    -o "$BUILD_OUT/main.o" \
    "$FW_DIR/src/main.cpp"
echo "    -> firmware main.cpp compiles (host mode)"

# --- 2. ROS package: catkin build if available, else syntax check ------------
echo "==> [2/2] building ROS package mower_wifi_bridge"
if command -v catkin_make >/dev/null 2>&1; then
  ( cd "$ROS_WS" && catkin_make --pkg mower_wifi_bridge )
  echo "    -> catkin_make build complete"
elif command -v catkin >/dev/null 2>&1; then
  ( cd "$ROS_WS" && catkin build mower_wifi_bridge )
  echo "    -> catkin build complete"
else
  echo "    (no catkin toolchain found; running Python syntax check instead)"
  PKG="$ROS_WS/src/mower_wifi_bridge"
  python3 -m py_compile \
    "$PKG/src/mower_wifi_bridge/__init__.py" \
    "$PKG/src/mower_wifi_bridge/protocol.py" \
    "$PKG/src/mower_wifi_bridge/ws_server.py" \
    "$PKG/src/mower_wifi_bridge/bridge_node.py" \
    "$PKG/nodes/mower_wifi_bridge_node" \
    "$PKG/scripts/wifi_test_client.py"
  echo "    -> Python sources compile cleanly"
fi

echo "==> FR-9 build OK"
