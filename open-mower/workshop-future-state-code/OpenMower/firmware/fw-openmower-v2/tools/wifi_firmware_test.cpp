//
// wifi_firmware_test.cpp - Native (host G++) verification for the FR-9 WiFi
// firmware module. Builds and runs on the workshop machine without the Pico SDK.
//
// This file is part of OpenMower (GPLv3). See src/wifi/wifi_module.h for license.
//
// Exercises the firmware WiFi module end-to-end on the host:
//   - FR-9.5 token authentication (accept valid, reject invalid)
//   - FR-9.2/9.3 START/STOP forwarded as high-level commands
//   - FR-9.4 EMERGENCY asserts the latch (via callback)
//   - CRC rejection
//   - FR-9.6 1 Hz status pump emits a framed status packet
//
// It frames packets exactly as the companion radio would (struct + CRC + COBS)
// and feeds them to WifiModule::feedByte(), then checks the observable effects.
//

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/wifi/COBS.h"
#include "../src/wifi/wifi_crc.h"
#include "../src/wifi/wifi_datatypes.h"
#include "../src/wifi/wifi_module.h"

using namespace openmower::wifi;

static int g_failures = 0;
#define CHECK(cond, msg)                                 \
  do {                                                   \
    if (!(cond)) {                                       \
      std::printf("  FAIL: %s\n", msg);                  \
      g_failures++;                                      \
    } else {                                             \
      std::printf("  ok:   %s\n", msg);                  \
    }                                                    \
  } while (0)

// ---- Test harness state captured via the module callbacks ------------------
struct Captured {
  std::vector<uint8_t> hl_commands;
  int emergency_count = 0;
  std::vector<std::vector<uint8_t>> outbound_frames;  // raw COBS frames written
};

static void cb_hl(uint8_t command, void* user) {
  static_cast<Captured*>(user)->hl_commands.push_back(command);
}
static void cb_emergency(void* user) {
  static_cast<Captured*>(user)->emergency_count++;
}
static void cb_write(const uint8_t* data, size_t len, void* user) {
  static_cast<Captured*>(user)->outbound_frames.emplace_back(data, data + len);
}

// Frame a command packet the way the companion radio does and feed it in.
static void feed_command(WifiModule& mod, uint8_t command, const char* token, uint8_t token_len,
                         bool corrupt_crc = false) {
  wifi_command_packet pkt;
  std::memset(&pkt, 0, sizeof(pkt));
  pkt.type = PACKET_ID_WIFI_COMMAND;
  pkt.command = command;
  pkt.token_len = token_len;
  if (token != nullptr && token_len > 0) {
    std::memcpy(pkt.token, token, token_len);
  }
  pkt.crc = crc_ccitt(reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt) - sizeof(uint16_t));
  if (corrupt_crc) {
    pkt.crc ^= 0xFFFF;
  }

  uint8_t encoded[WIFI_MAX_FRAME_LEN];
  size_t n = COBS::encode(reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt), encoded);
  for (size_t i = 0; i < n; i++) {
    mod.feedByte(encoded[i]);
  }
  mod.feedByte(0x00);  // delimiter -> triggers decode + dispatch
}

// Decode the last outbound frame into a status packet (if it is one).
static bool last_status(const Captured& cap, wifi_status_packet* out) {
  for (auto it = cap.outbound_frames.rbegin(); it != cap.outbound_frames.rend(); ++it) {
    const auto& frame = *it;
    if (frame.empty()) continue;
    // frame includes trailing 0x00 delimiter; strip it before COBS decode.
    size_t len = frame.size();
    if (frame.back() == 0x00) len--;
    uint8_t decoded[WIFI_MAX_FRAME_LEN];
    size_t dn = COBS::decode(frame.data(), len, decoded);
    if (dn == sizeof(wifi_status_packet) && decoded[0] == PACKET_ID_WIFI_STATUS) {
      std::memcpy(out, decoded, sizeof(wifi_status_packet));
      return true;
    }
  }
  return false;
}

int main() {
  const char* kToken = "super-secret-token";
  const uint8_t kTokenLen = static_cast<uint8_t>(std::strlen(kToken));

  std::printf("FR-9 firmware WiFi module - native verification\n");

  // --- FR-9.5: valid token accepted, START forwarded (FR-9.2) ---------------
  {
    Captured cap;
    WifiModule mod;
    WifiModuleCallbacks cb;
    cb.on_high_level_command = &cb_hl;
    cb.on_emergency = &cb_emergency;
    cb.write_bytes = &cb_write;
    cb.user = &cap;
    mod.begin(cb, kToken, kTokenLen);

    feed_command(mod, WIFI_CMD_START, kToken, kTokenLen);
    CHECK(cap.hl_commands.size() == 1 && cap.hl_commands[0] == WIFI_CMD_START,
          "FR-9.2 valid token -> START forwarded");
    CHECK(mod.acceptedCommandCount() == 1, "accepted command counter incremented");

    feed_command(mod, WIFI_CMD_STOP, kToken, kTokenLen);
    CHECK(cap.hl_commands.size() == 2 && cap.hl_commands[1] == WIFI_CMD_STOP,
          "FR-9.3 valid token -> STOP forwarded");
  }

  // --- FR-9.5: invalid token rejected, no state change (SR-8) ---------------
  {
    Captured cap;
    WifiModule mod;
    WifiModuleCallbacks cb;
    cb.on_high_level_command = &cb_hl;
    cb.on_emergency = &cb_emergency;
    cb.write_bytes = &cb_write;
    cb.user = &cap;
    mod.begin(cb, kToken, kTokenLen);

    feed_command(mod, WIFI_CMD_START, "wrong-token", 11);
    CHECK(cap.hl_commands.empty(), "FR-9.5 invalid token -> command NOT forwarded");
    CHECK(mod.rejectedAuthCount() == 1, "FR-9.5 rejected-auth counter incremented");
    CHECK(cap.emergency_count == 0, "FR-9.5 invalid token -> no emergency");
  }

  // --- FR-9.4: EMERGENCY asserts latch via callback -------------------------
  {
    Captured cap;
    WifiModule mod;
    WifiModuleCallbacks cb;
    cb.on_high_level_command = &cb_hl;
    cb.on_emergency = &cb_emergency;
    cb.write_bytes = &cb_write;
    cb.user = &cap;
    mod.begin(cb, kToken, kTokenLen);

    feed_command(mod, WIFI_CMD_EMERGENCY, kToken, kTokenLen);
    CHECK(cap.emergency_count == 1, "FR-9.4 valid token -> EMERGENCY asserted");
  }

  // --- CRC rejection --------------------------------------------------------
  {
    Captured cap;
    WifiModule mod;
    WifiModuleCallbacks cb;
    cb.on_high_level_command = &cb_hl;
    cb.on_emergency = &cb_emergency;
    cb.write_bytes = &cb_write;
    cb.user = &cap;
    mod.begin(cb, kToken, kTokenLen);

    feed_command(mod, WIFI_CMD_START, kToken, kTokenLen, /*corrupt_crc=*/true);
    CHECK(cap.hl_commands.empty(), "bad CRC -> command dropped");
    CHECK(mod.badCrcCount() == 1, "bad CRC counter incremented");
  }

  // --- FR-9.6: 1 Hz status pump emits framed status -------------------------
  {
    Captured cap;
    WifiModule mod;
    WifiModuleCallbacks cb;
    cb.on_high_level_command = &cb_hl;
    cb.on_emergency = &cb_emergency;
    cb.write_bytes = &cb_write;
    cb.user = &cap;
    mod.begin(cb, kToken, kTokenLen);

    WifiStatusSnapshot snap;
    snap.state = 2;
    snap.gps_quality = 97;
    snap.battery_percent = 83;
    snap.is_charging = false;
    snap.emergency = false;
    mod.setStatus(snap);

    mod.tick(0);       // first tick emits immediately
    mod.tick(500);     // < 1000 ms: no new emission
    size_t after_500 = cap.outbound_frames.size();
    mod.tick(1000);    // >= 1000 ms since first: emit again
    size_t after_1000 = cap.outbound_frames.size();

    CHECK(after_500 == 1, "FR-9.6 no extra status emitted before 1 s");
    CHECK(after_1000 == 2, "FR-9.6 status emitted at 1 Hz");

    wifi_status_packet sp;
    bool got = last_status(cap, &sp);
    CHECK(got, "FR-9.6 status frame decodes");
    if (got) {
      CHECK(sp.state == 2 && sp.gps_quality == 97 && sp.battery_percent == 83,
            "FR-9.6 status fields mirror snapshot");
    }
  }

  std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
  return g_failures == 0 ? 0 : 1;
}
