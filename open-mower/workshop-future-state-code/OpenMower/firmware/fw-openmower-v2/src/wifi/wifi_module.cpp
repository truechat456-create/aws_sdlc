//
// wifi_module.cpp - WiFi remote start/stop firmware module (implementation).
//
// This file is part of OpenMower (GPLv3). See wifi_module.h for the full header.
//
// =============================================================================
// FR-9 WiFi Remote Start/Stop - Firmware Module implementation
// =============================================================================
// Implements the inbound command pipeline and the 1 Hz status pump described in
// wifi_module.h. The packet pipeline mirrors the established v1 flow:
//
//   inbound : COBS decode -> verify CRC -> check type -> authenticate (FR-9.5)
//             -> dispatch (FR-9.2/9.3/9.4/SR-7)
//   outbound: fill struct -> append CRC -> COBS encode -> write (FR-9.6 + ACK)
// =============================================================================

#include "wifi_module.h"

#include <string.h>

#include "COBS.h"
#include "wifi_crc.h"

namespace openmower {
namespace wifi {

namespace {
// Constant-time-ish token comparison. Avoids early-out on first mismatch so the
// reject path does not leak token length/content via timing (defensive; FR-9.5).
bool constant_time_equal(const uint8_t* a, const uint8_t* b, uint8_t len) {
  uint8_t diff = 0;
  for (uint8_t i = 0; i < len; i++) {
    diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  }
  return diff == 0;
}
}  // namespace

WifiModule::WifiModule() = default;

void WifiModule::begin(const WifiModuleCallbacks& callbacks, const char* token, uint8_t token_len) {
  cb_ = callbacks;
  // FR-9.5 / NFR-6: token is injected from configuration, never hard-coded here.
  if (token_len > WIFI_TOKEN_MAX_LEN) {
    token_len = WIFI_TOKEN_MAX_LEN;
  }
  memset(token_, 0, sizeof(token_));
  if (token != nullptr && token_len > 0) {
    memcpy(token_, token, token_len);
  }
  token_len_ = token_len;
  rx_len_ = 0;
  first_tick_ = true;
}

bool WifiModule::feedByte(uint8_t byte) {
  // COBS stream: 0x00 is the frame delimiter.
  if (byte == 0x00) {
    bool dispatched = false;
    if (rx_len_ > 0) {
      uint8_t decoded[WIFI_MAX_FRAME_LEN];
      size_t decoded_len = COBS::decode(rx_buf_, rx_len_, decoded);
      if (decoded_len > 0) {
        handleDecodedPacket(decoded, decoded_len);
        dispatched = true;
      }
    }
    rx_len_ = 0;
    return dispatched;
  }

  if (rx_len_ < sizeof(rx_buf_)) {
    rx_buf_[rx_len_++] = byte;
  } else {
    // Overflow - drop the partial frame and resync on the next delimiter.
    rx_len_ = 0;
  }
  return false;
}

void WifiModule::handleDecodedPacket(const uint8_t* buf, size_t len) {
  if (len < 1) {
    return;
  }
  if (buf[0] != PACKET_ID_WIFI_COMMAND) {
    return;  // only command packets are inbound
  }
  if (len != sizeof(wifi_command_packet)) {
    return;  // malformed length
  }

  wifi_command_packet pkt;
  memcpy(&pkt, buf, sizeof(pkt));

  // Verify CRC over everything preceding the trailing uint16 crc field.
  uint16_t expected = crc_ccitt(buf, sizeof(wifi_command_packet) - sizeof(uint16_t));
  if (expected != pkt.crc) {
    bad_crc_++;
    emitAck(pkt.command, WIFI_ACK_BAD_CRC);
    return;
  }

  dispatchCommand(pkt);
}

bool WifiModule::tokenMatches(const uint8_t* token, uint8_t len) const {
  if (len != token_len_) {
    return false;
  }
  return constant_time_equal(token, token_, token_len_);
}

void WifiModule::dispatchCommand(const wifi_command_packet& pkt) {
  // FR-9.5 / SR-8: authenticate BEFORE acting on any command.
  uint8_t token_len = pkt.token_len;
  if (token_len > WIFI_TOKEN_MAX_LEN) {
    token_len = WIFI_TOKEN_MAX_LEN;
  }
  if (!tokenMatches(pkt.token, token_len)) {
    rejected_auth_++;
    emitAck(pkt.command, WIFI_ACK_UNAUTHORIZED);  // reject + report, no state change
    return;
  }

  switch (pkt.command) {
    case WIFI_CMD_START:  // FR-9.2
    case WIFI_CMD_STOP:   // FR-9.3
    case WIFI_CMD_RESET_EMERGENCY:  // SR-7 (asserts via host COMMAND_RESET_EMERGENCY)
      if (cb_.on_high_level_command != nullptr) {
        cb_.on_high_level_command(pkt.command, cb_.user);
      }
      accepted_commands_++;
      emitAck(pkt.command, WIFI_ACK_OK);
      break;

    case WIFI_CMD_EMERGENCY:  // FR-9.4 (< 1 s): assert latch directly, no host round-trip
      if (cb_.on_emergency != nullptr) {
        cb_.on_emergency(cb_.user);
      }
      accepted_commands_++;
      emitAck(pkt.command, WIFI_ACK_OK);
      break;

    default:
      emitAck(pkt.command, WIFI_ACK_UNKNOWN_CMD);
      break;
  }
}

void WifiModule::setStatus(const WifiStatusSnapshot& status) {
  status_ = status;
}

void WifiModule::tick(uint32_t now_millis) {
  // FR-9.6: emit a status packet once per second.
  if (first_tick_) {
    first_tick_ = false;
    last_status_millis_ = now_millis;
    emitStatusPacket();
    return;
  }
  // Unsigned subtraction handles the 32-bit millis wrap correctly.
  if (static_cast<uint32_t>(now_millis - last_status_millis_) >= 1000u) {
    last_status_millis_ = now_millis;
    emitStatusPacket();
  }
}

void WifiModule::emitStatusPacket() {
  wifi_status_packet pkt;
  pkt.type = PACKET_ID_WIFI_STATUS;
  pkt.state = status_.state;
  pkt.gps_quality = status_.gps_quality;
  pkt.battery_percent = status_.battery_percent;
  pkt.is_charging = status_.is_charging ? 1 : 0;
  pkt.emergency = status_.emergency ? 1 : 0;
  pkt.crc = crc_ccitt(reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt) - sizeof(uint16_t));
  sendFramed(reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
}

void WifiModule::emitAck(uint8_t command, uint8_t result) {
  wifi_ack_packet pkt;
  pkt.type = PACKET_ID_WIFI_ACK;
  pkt.command = command;
  pkt.result = result;
  pkt.crc = crc_ccitt(reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt) - sizeof(uint16_t));
  sendFramed(reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
}

void WifiModule::sendFramed(const uint8_t* payload, size_t len) {
  if (cb_.write_bytes == nullptr) {
    return;
  }
  uint8_t encoded[WIFI_MAX_FRAME_LEN];
  size_t encoded_len = COBS::encode(payload, len, encoded);
  encoded[encoded_len] = 0x00;  // COBS delimiter
  cb_.write_bytes(encoded, encoded_len + 1, cb_.user);
}

}  // namespace wifi
}  // namespace openmower
