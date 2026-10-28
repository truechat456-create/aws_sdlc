//
// wifi_token_provider.cpp - supplies the FR-9 WiFi shared secret to the firmware.
//
// This file is part of OpenMower (GPLv3). See src/wifi/wifi_module.h for the
// full license header.
//
// =============================================================================
// FR-9.5 / NFR-6 - WiFi token provider (firmware side)
// =============================================================================
// main.cpp's firmware setup() path pulls the shared secret through these two
// accessors:
//
//     extern const char* openmower_wifi_token();
//     extern uint8_t      openmower_wifi_token_len();
//
// On the real mower the token lives in the ll_high_level_config reserved option
// area / NVM and is loaded at boot (NFR-6: configured, never hard-coded in
// source). That config subsystem is a separate firmware module; to keep the
// firmware link self-contained here, the token is injected at build time via
// the OM_WIFI_TOKEN macro (e.g. -DOM_WIFI_TOKEN=\"...\" or a build-time config
// header). It is NOT a literal baked into the tracked source.
//
// If no token is configured the accessors return an empty token, which the
// WifiModule treats as "reject everything" (fail-safe: no auth secret -> no
// remote command is ever accepted).
// =============================================================================

#include <stddef.h>
#include <stdint.h>

#include "wifi/wifi_datatypes.h"  // WIFI_TOKEN_MAX_LEN

// OM_WIFI_TOKEN is supplied by the build/config layer (not hard-coded here).
// Default to an empty token so an unconfigured build fails safe (NFR-6 / NFR-8).
#ifndef OM_WIFI_TOKEN
#define OM_WIFI_TOKEN ""
#endif

namespace {
// Compile-time token string from configuration. Length is clamped to the wire
// limit (WIFI_TOKEN_MAX_LEN) so it always fits the packed command struct.
const char kWifiToken[] = OM_WIFI_TOKEN;

constexpr uint8_t clamp_len(size_t n) {
  return static_cast<uint8_t>(n > WIFI_TOKEN_MAX_LEN ? WIFI_TOKEN_MAX_LEN : n);
}
}  // namespace

const char* openmower_wifi_token() {
  return kWifiToken;
}

uint8_t openmower_wifi_token_len() {
  // sizeof includes the trailing NUL; subtract it for the on-wire length.
  return clamp_len(sizeof(kWifiToken) - 1);
}
