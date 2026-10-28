//
// wifi_crc.h - CRC-CCITT (XModem) used to protect WiFi packets.
//
// This file is part of OpenMower (GPLv3). See wifi_module.h for the license header.
//
// Mirrors boost::crc_ccitt_type (poly 0x1021, init 0xFFFF, no reflection, no
// final xor) as used by the existing v1 comms, so the companion radio and the
// firmware agree on checksums without pulling in Boost on the MCU.
//

#ifndef OPENMOWER_WIFI_CRC_H
#define OPENMOWER_WIFI_CRC_H

#include <stddef.h>
#include <stdint.h>

namespace openmower {
namespace wifi {

inline uint16_t crc_ccitt(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int b = 0; b < 8; b++) {
      if (crc & 0x8000) {
        crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
      } else {
        crc = static_cast<uint16_t>(crc << 1);
      }
    }
  }
  return crc;
}

}  // namespace wifi
}  // namespace openmower

#endif  // OPENMOWER_WIFI_CRC_H
