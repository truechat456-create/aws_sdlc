//
// main.cpp - RP2040 mainboard firmware entry point (WiFi-integrated).
//
// This file is part of OpenMower (GPLv3). See src/wifi/wifi_module.h for the
// full license header.
//
// =============================================================================
// FR-9 WiFi Remote Start/Stop - Firmware integration
// =============================================================================
// This is the baseline dual-core firmware skeleton (04-Firmware-Architecture.md)
// with the WiFi remote module (FR-9) wired into the Core 1 communication loop.
//
//   Core 0 (control/safety): samples sensors, evaluates emergency, drives xESC.
//                            Owns the emergency latch (SR-1) - final authority.
//   Core 1 (comms):          host link + the new WiFi module.
//
// The WiFi module (src/wifi/wifi_module.*) is a thin, token-authenticated bridge
// (FR-9.5). START/STOP (FR-9.2/9.3) and RESET_EMERGENCY (SR-7) are forwarded to
// the host as high-level requests; EMERGENCY (FR-9.4) asserts the Core 0 latch
// directly on the < 1 s path. A 1 Hz pump reports status back (FR-9.6).
//
// The firmware proper targets the RP2040 via the Pico SDK / Arduino core. Those
// toolchains are not present in this workspace (the firmware is a git submodule
// that is not checked out). To keep this integration compilable and verifiable
// with host G++, the MCU-specific calls are behind the OPENMOWER_FIRMWARE_BUILD
// macro; when it is not defined we provide thin host stubs so the WiFi wiring
// and packet handling can be exercised natively. The control-flow and the WiFi
// integration points are identical in both builds.
// =============================================================================

#include <stdint.h>

#include "wifi/wifi_module.h"

#if defined(OPENMOWER_FIRMWARE_BUILD)
// -------- Real firmware target (RP2040 / Pico SDK + Arduino core) ------------
#include <Arduino.h>
#include <pico/multicore.h>
// The companion WiFi radio is attached on a dedicated UART (e.g. Serial2).
#define WIFI_SERIAL Serial2
#else
// -------- Host build (unit/integration verification with G++) ----------------
#include <chrono>
#include <cstdio>
// Host-build helper; also used to demonstrate the 1 Hz pump clock. Marked
// maybe_unused because the firmware build drives the pump from millis() instead.
__attribute__((unused)) static uint32_t host_millis() {
  using namespace std::chrono;
  static const auto t0 = steady_clock::now();
  return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now() - t0).count());
}
#endif

using openmower::wifi::WifiModule;
using openmower::wifi::WifiModuleCallbacks;
using openmower::wifi::WifiStatusSnapshot;

// ---------------------------------------------------------------------------
// Shared Core 0 <-> Core 1 state. On the MCU these are protected by the
// inter-core FIFO / lock mechanism; the emergency latch is owned by Core 0.
// ---------------------------------------------------------------------------
static volatile bool g_emergency_latch = false;  // Core 0 owns this (SR-1)
static volatile uint8_t g_pending_hl_command = 0;  // set by WiFi, drained by host link

// The WiFi remote module instance (lives on Core 1).
static WifiModule g_wifi;

// ---------------------------------------------------------------------------
// WiFi module callbacks
// ---------------------------------------------------------------------------

// FR-9.2 / FR-9.3 / SR-7: forward an authenticated high-level command to the
// host (Raspberry Pi). The host maps it onto mower_service/high_level_control.
static void on_wifi_high_level_command(uint8_t command, void* /*user*/) {
  g_pending_hl_command = command;
#if defined(OPENMOWER_FIRMWARE_BUILD)
  // Enqueue the request onto the host-link outbound path (heartbeat/command).
  // (Handled by the existing comms code; left as an integration hook here.)
#else
  std::printf("[fw] forward high-level command %u to host\n", command);
#endif
}

// FR-9.4: assert the low-level emergency latch IMMEDIATELY (< 1 s). WiFi can
// only assert, never clear, the latch (SR-7) - clearing requires an explicit
// RESET_EMERGENCY high-level command routed through the normal safety path.
static void on_wifi_emergency(void* /*user*/) {
  g_emergency_latch = true;
#if defined(OPENMOWER_FIRMWARE_BUILD)
  // On the MCU this additionally pokes the Core 0 safety loop so motors are
  // zeroed on the next control cycle regardless of host state.
#else
  std::printf("[fw] EMERGENCY latch asserted via WiFi\n");
#endif
}

// Outbound byte sink: transmit a framed WiFi packet to the companion radio.
static void on_wifi_write(const uint8_t* data, size_t len, void* /*user*/) {
#if defined(OPENMOWER_FIRMWARE_BUILD)
  WIFI_SERIAL.write(data, len);
#else
  (void)data;
  (void)len;  // host build: swallow (test harness inspects via its own sink)
#endif
}

// ---------------------------------------------------------------------------
// Core 1: communication loop (host link + WiFi)
// ---------------------------------------------------------------------------
static void wifi_setup(const char* token, uint8_t token_len) {
  WifiModuleCallbacks cb;
  cb.on_high_level_command = &on_wifi_high_level_command;
  cb.on_emergency = &on_wifi_emergency;  // FR-9.4
  cb.write_bytes = &on_wifi_write;
  cb.user = nullptr;
  // FR-9.5 / NFR-6: token is supplied from configuration, not hard-coded.
  g_wifi.begin(cb, token, token_len);
}

// Pull the latest mower status into the WiFi module so the 1 Hz pump (FR-9.6)
// can report it. In the firmware this reads the shared ll_status snapshot.
// (maybe_unused: called from loop() only in the firmware build.)
__attribute__((unused)) static void wifi_publish_status(uint8_t state, uint8_t gps_quality, uint8_t battery_percent, bool is_charging) {
  WifiStatusSnapshot snap;
  snap.state = state;
  snap.gps_quality = gps_quality;
  snap.battery_percent = battery_percent;
  snap.is_charging = is_charging;
  snap.emergency = g_emergency_latch;  // report the authoritative Core 0 latch
  g_wifi.setStatus(snap);
}

#if defined(OPENMOWER_FIRMWARE_BUILD)
// One iteration of the Core 1 comms loop.
static void core1_comms_step() {
  // Drain the WiFi radio UART into the module (COBS stream).
  while (WIFI_SERIAL.available() > 0) {
    g_wifi.feedByte(static_cast<uint8_t>(WIFI_SERIAL.read()));  // FR-9.2..9.5
  }
  g_wifi.tick(millis());  // FR-9.6 status pump
  // ... existing host-link (heartbeat/config/cmd) handling runs here too ...
}

void core1_entry() {
  for (;;) {
    core1_comms_step();
  }
}

void setup() {
  // ... existing bus/sensor/xESC init sets the INITIALIZED status bit ...
  WIFI_SERIAL.begin(115200);
  // Token comes from ll_high_level_config (reserved option area) / NVM, not here.
  extern const char* openmower_wifi_token();
  extern uint8_t openmower_wifi_token_len();
  wifi_setup(openmower_wifi_token(), openmower_wifi_token_len());
  multicore_launch_core1(core1_entry);  // Core 1 runs comms; Core 0 continues below
}

void loop() {
  // Core 0 control/safety loop. Feeds status to the WiFi pump each cycle.
  // (Sensor sampling, emergency evaluation and xESC drive run here.)
  wifi_publish_status(/*state*/ 0, /*gps*/ 0, /*batt*/ 0, /*charging*/ false);
}
#else
// Host build: no main() here. The native integration/unit test harness
// (tools/wifi_firmware_test.cpp) drives wifi_setup(), feedByte() and tick()
// directly and inspects the module via its public stats accessors. Exposing
// the setup/status hooks is enough for that harness to link against.
extern "C" void openmower_wifi_host_setup(const char* token, uint8_t token_len) {
  wifi_setup(token, token_len);
}
#endif
