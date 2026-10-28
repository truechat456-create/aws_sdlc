//
// wifi_datatypes.h - Packed wire structs for the FR-9 WiFi remote protocol.
//
// This file is part of OpenMower (GPLv3). See wifi_module.h for the license header.
//
// =============================================================================
// FR-9 WiFi Remote Start/Stop - Firmware wire protocol
// =============================================================================
// These packed structs mirror the established OpenMower v1 packet style
// (one-byte type ID + payload + uint16 CRC, then COBS-framed). They define the
// contract between the companion WiFi radio and the RP2040 firmware module.
//
//   Inbound (radio -> firmware):  wifi_command_packet   (FR-9.2..9.5)
//   Outbound (firmware -> radio): wifi_status_packet    (FR-9.6)
//                                 wifi_ack_packet        (command result)
//
// Command codes intentionally match mower_msgs/HighLevelControlSrv so the host
// bridge can forward them 1:1 (see 05-Communication-Protocols.md command map).
// =============================================================================

#ifndef OPENMOWER_WIFI_DATATYPES_H
#define OPENMOWER_WIFI_DATATYPES_H

#include <stdint.h>

// Packet type IDs for the WiFi channel. Chosen outside the existing
// PACKET_ID_LL_* range (ll_datatypes.h) to avoid any collision.
#define PACKET_ID_WIFI_COMMAND 0x60  // radio -> firmware (authenticated command)
#define PACKET_ID_WIFI_STATUS 0x61   // firmware -> radio (1 Hz status, FR-9.6)
#define PACKET_ID_WIFI_ACK 0x62      // firmware -> radio (command result)

// WiFi command codes. Values match mower_msgs/HighLevelControlSrv COMMAND_*.
#define WIFI_CMD_START 1            // FR-9.2  -> COMMAND_START
#define WIFI_CMD_STOP 2             // FR-9.3  -> COMMAND_HOME
#define WIFI_CMD_EMERGENCY 100      // FR-9.4  -> assert emergency latch
#define WIFI_CMD_RESET_EMERGENCY 254  // SR-7  -> COMMAND_RESET_EMERGENCY

// ACK result codes.
#define WIFI_ACK_OK 0
#define WIFI_ACK_UNAUTHORIZED 1  // FR-9.5 token mismatch
#define WIFI_ACK_UNKNOWN_CMD 2
#define WIFI_ACK_BAD_CRC 3

// Fixed-length shared secret buffer (FR-9.5). A 32-byte token is ample for a
// pre-shared key and keeps the struct fixed-size for the packed wire format.
#define WIFI_TOKEN_MAX_LEN 32

// Upper bound for a decoded frame; drives the COBS reassembly buffer size.
#define WIFI_MAX_FRAME_LEN 128

#pragma pack(push, 1)
// Inbound authenticated command (FR-9.2..9.5).
struct wifi_command_packet {
  uint8_t type;                     // PACKET_ID_WIFI_COMMAND
  uint8_t command;                  // WIFI_CMD_*
  uint8_t token_len;                // valid bytes in token[]
  uint8_t token[WIFI_TOKEN_MAX_LEN];  // FR-9.5 shared secret
  uint16_t crc;                     // CRC over all preceding bytes
} __attribute__((packed));
#pragma pack(pop)

#pragma pack(push, 1)
// Outbound status snapshot (FR-9.6), emitted at 1 Hz. Fields mirror the
// meaningful subset of mower_msgs/HighLevelStatus.
struct wifi_status_packet {
  uint8_t type;             // PACKET_ID_WIFI_STATUS
  uint8_t state;            // HighLevelStatus.state
  uint8_t gps_quality;      // 0..100
  uint8_t battery_percent;  // 0..100
  uint8_t is_charging;      // bool
  uint8_t emergency;        // bool
  uint16_t crc;
} __attribute__((packed));
#pragma pack(pop)

#pragma pack(push, 1)
// Outbound command acknowledgement.
struct wifi_ack_packet {
  uint8_t type;     // PACKET_ID_WIFI_ACK
  uint8_t command;  // echoed command
  uint8_t result;   // WIFI_ACK_*
  uint16_t crc;
} __attribute__((packed));
#pragma pack(pop)

#endif  // OPENMOWER_WIFI_DATATYPES_H
