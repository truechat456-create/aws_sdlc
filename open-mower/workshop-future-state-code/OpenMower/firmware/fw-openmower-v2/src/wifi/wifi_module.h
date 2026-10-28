//
// wifi_module.h - WiFi remote start/stop firmware module for the RP2040 mainboard.
//
// This file is part of OpenMower.
//
// OpenMower is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, version 3 of the License.
//
// OpenMower is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
// warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with OpenMower. If not, see
// <https://www.gnu.org/licenses/>.
//
// =============================================================================
// FR-9 WiFi Remote Start/Stop - Firmware Module
// =============================================================================
// Requirements realized here (see workshop-future-state-requirements/16-WiFi-Subsystem.md
// and 04-Firmware-Architecture.md "How a New Module (WiFi) Plugs In"):
//
//   FR-9.1  WiFi connectivity for remote command reception (companion ESP WiFi
//           module feeds framed packets to this firmware module over UART).
//   FR-9.2  Remote START  -> emit WIFI_CMD_START upward to the host.
//   FR-9.3  Remote STOP   -> emit WIFI_CMD_STOP upward to the host.
//   FR-9.4  Remote EMERGENCY -> assert low-level emergency latch directly
//           (< 1 s path, does not wait on the host round-trip).
//   FR-9.5  Token-based authentication: every inbound packet carries a token that
//           must match the configured secret before the command is acted on.
//   FR-9.6  Status monitoring at 1 Hz: module emits a status packet every 1000 ms.
//
// Design constraints (04-Firmware-Architecture.md):
//   - Runs on Core 1 (communication). It MUST NOT take control-loop authority
//     from Core 0. The only exception is FR-9.4 emergency, which only *asserts*
//     (never clears) the existing emergency latch via the provided callback -
//     this preserves SR-1 (firmware is final authority) and SR-7 (WiFi asserts,
//     never replaces, the chain).
//   - No dynamic allocation; fixed-size packed structs + COBS + CRC framing,
//     mirroring the established v1 packet pipeline.
//
// The module is transport-agnostic: the companion WiFi radio delivers raw bytes
// via WifiModule::feedByte(); the module delivers outbound bytes via a write
// callback. This keeps it unit-testable on a host with G++.
// =============================================================================

#ifndef OPENMOWER_WIFI_MODULE_H
#define OPENMOWER_WIFI_MODULE_H

#include <stddef.h>
#include <stdint.h>

#include "wifi_datatypes.h"

namespace openmower {
namespace wifi {

// Callback the host-link / safety layer provides so the module can act on
// authenticated commands. All callbacks are optional (may be null) which keeps
// the module usable in tests.
struct WifiModuleCallbacks {
  // FR-9.2 / FR-9.3: a validated high-level command (START/STOP/RESET_EMERGENCY)
  // should be forwarded to the host (Raspberry Pi) as a high-level request.
  void (*on_high_level_command)(uint8_t command, void* user) = nullptr;

  // FR-9.4: a validated EMERGENCY command. Implementation must assert (set) the
  // low-level emergency latch immediately. It must never clear the latch.
  void (*on_emergency)(void* user) = nullptr;

  // Outbound byte sink: the module calls this to transmit a framed packet to the
  // companion WiFi radio. Called once per fully framed packet.
  void (*write_bytes)(const uint8_t* data, size_t len, void* user) = nullptr;

  // Opaque pointer passed back to every callback.
  void* user = nullptr;
};

// Snapshot of mower status the comms layer feeds in so the 1 Hz pump (FR-9.6)
// can report it to remote clients. Mirrors mower_msgs/HighLevelStatus fields
// that are meaningful over the wire.
struct WifiStatusSnapshot {
  uint8_t state = 0;             // HighLevelStatus.state
  uint8_t gps_quality = 0;       // 0..100
  uint8_t battery_percent = 0;   // 0..100
  bool is_charging = false;
  bool emergency = false;
};

class WifiModule {
 public:
  // token: the configured shared secret (FR-9.5 / NFR-6 - supplied from config,
  // never hard-coded). token_len must be <= WIFI_TOKEN_MAX_LEN.
  WifiModule();

  // Configure the module. Safe to call again to rotate the token.
  void begin(const WifiModuleCallbacks& callbacks, const char* token, uint8_t token_len);

  // Feed one received byte from the companion WiFi radio (COBS stream).
  // Returns true when a full packet was decoded and dispatched this call.
  bool feedByte(uint8_t byte);

  // Update the cached status used by the 1 Hz pump (FR-9.6).
  void setStatus(const WifiStatusSnapshot& status);

  // Drive the 1 Hz status pump. now_millis is a monotonic millisecond clock.
  // Emits a status packet when >= 1000 ms elapsed since the last emission.
  void tick(uint32_t now_millis);

  // Stats / introspection (useful for tests and diagnostics).
  uint32_t acceptedCommandCount() const { return accepted_commands_; }
  uint32_t rejectedAuthCount() const { return rejected_auth_; }
  uint32_t badCrcCount() const { return bad_crc_; }

 private:
  void handleDecodedPacket(const uint8_t* buf, size_t len);
  void dispatchCommand(const wifi_command_packet& pkt);
  void emitStatusPacket();
  void emitAck(uint8_t command, uint8_t result);
  void sendFramed(const uint8_t* payload, size_t len);
  bool tokenMatches(const uint8_t* token, uint8_t len) const;

  WifiModuleCallbacks cb_{};

  // Configured secret (FR-9.5). Stored as bytes; length-prefixed compare.
  uint8_t token_[WIFI_TOKEN_MAX_LEN] = {0};
  uint8_t token_len_ = 0;

  // COBS reassembly buffer for the inbound stream.
  uint8_t rx_buf_[WIFI_MAX_FRAME_LEN] = {0};
  size_t rx_len_ = 0;

  WifiStatusSnapshot status_{};
  uint32_t last_status_millis_ = 0;
  bool first_tick_ = true;

  uint32_t accepted_commands_ = 0;
  uint32_t rejected_auth_ = 0;
  uint32_t bad_crc_ = 0;
};

}  // namespace wifi
}  // namespace openmower

#endif  // OPENMOWER_WIFI_MODULE_H
