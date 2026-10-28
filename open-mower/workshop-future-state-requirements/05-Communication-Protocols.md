# 05 - Communication Protocols

> **[MODIFIED]** Future-state revision. Adds the external WiFi remote protocol
> (FR-9.1–FR-9.6) and shows how `mower_wifi` maps it onto existing ROS
> services/topics. New content marked `[NEW]`. See
> [16-WiFi-Subsystem](16-WiFi-Subsystem.md).

Baseline communication protocols between firmware and ROS, and the ROS-internal
topic/service interfaces. Ends with the pattern for adding a new communication
channel (the WiFi integration point).

Related: [04-Firmware-Architecture](04-Firmware-Architecture.md),
[06-Software-Architecture](06-Software-Architecture.md),
[07-Safety-Systems](07-Safety-Systems.md),
[16-WiFi-Subsystem](16-WiFi-Subsystem.md).

## Protocol Overview

| Platform | Transport            | Framing / mechanism          | ROS node          |
|----------|----------------------|------------------------------|-------------------|
| v1       | UART serial          | COBS + CRC, typed packets    | `mower_comms_v1`  |
| v2       | IP (loopback/robot)  | `xbot_framework` service bus | `mower_comms_v2`  |

Selection is by `HARDWARE_PLATFORM` in `_comms.launch` (v1 => node `ll`,
v2 => node `mower_comms_v2`).

## v1 Serial Protocol (firmware <-> ROS)

Packets are fixed-layout packed structs, each prefixed by a one-byte type and
suffixed by a CRC, then COBS-encoded with a `0x00` delimiter. Encoding of an
outbound heartbeat (`mower_comms_v1/src/mower_comms.cpp`):

```cpp
struct ll_heartbeat heartbeat = {.type = PACKET_ID_LL_HEARTBEAT,
                                 .emergency_requested = (!emergency_low_level && emergency_high_level),
                                 .emergency_release_requested = ll_clear_emergency};
crc.reset();
crc.process_bytes(&heartbeat, sizeof(struct ll_heartbeat) - 2);
heartbeat.crc = crc.checksum();
size_t encoded_size = cobs.encode((uint8_t*)&heartbeat, sizeof(struct ll_heartbeat), out_buf);
out_buf[encoded_size] = 0;   // COBS packet delimiter
```

### Message Formats (packed structs, `ll_datatypes.h`)

- `ll_status` (type 1): `status_bitmask`, `uss_ranges_m[5]`, `emergency_bitmask`,
  `v_charge`, `v_system`, `charging_current`, `batt_percentage`, `crc`.
- `ll_imu` (type 2): `dt_millis`, `acceleration_mss[3]`, `gyro_rads[3]`,
  `mag_uT[3]`, `crc`.
- `ll_heartbeat` (0x42): `emergency_requested`, `emergency_release_requested`.
- `ll_ui_event` (type 3): `button_id`, `press_duration`.
- `ll_high_level_state` (0x43): `current_mode`, `gps_quality`.
- `ll_high_level_config` (0x11 req / 0x12 rsp): flexible-length, forward-compatible
  configuration (charger limits, lift/tilt periods, language, volume, hall configs,
  plus 5 reserved option bits).

Status-bitmask and emergency-bitmask bit meanings are enumerated in
[03](03-Hardware-Architecture.md) and [07](07-Safety-Systems.md).

## v2 Service Bus (firmware <-> ROS)

v2 drops hand-framed packets for an IP-based service bus provided by
`xbot_framework`. `mower_comms_v2/src/mower_comms.cpp` starts the context and binds
one interface per firmware service:

```cpp
ctx = xbot::serviceif::Start(true, bind_ip);   // bind_ip default "0.0.0.0"

emergency_service = std::make_unique<EmergencyServiceInterface>(xbot::service_ids::EMERGENCY, ctx, emergency_pub);
emergency_service->Start();
diff_drive_service = std::make_unique<DiffDriveServiceInterface>(xbot::service_ids::DIFF_DRIVE, ctx, ...);
diff_drive_service->Start();
// MOWER, IMU, POWER, BMS, GPS, INPUT, HIGH_LEVEL services follow the same pattern
```

Each service surfaces firmware outputs as ROS publishers through
`On<Field>Changed(...)` callbacks and accepts inputs via `Send<...>()` helpers
(e.g. `DiffDriveServiceInterface::SendTwist`, `GpsServiceInterface::SendRTCM`,
`EmergencyServiceInterface::SendHighLevelEmergency`).

## ROS-Internal Interfaces

### Key Topics (under `/ll/...`)

| Topic                              | Type                       | Direction |
|------------------------------------|----------------------------|-----------|
| `/ll/mower_status`                 | `mower_msgs/Status`        | FW -> ROS |
| `/ll/imu/data_raw`                 | `sensor_msgs/Imu`          | FW -> ROS |
| `/ll/power`                        | `mower_msgs/Power`         | FW -> ROS |
| `/ll/bms`                          | `mower_msgs/Bms`           | FW -> ROS |
| `/ll/position/gps`                 | `xbot_msgs/AbsolutePose`   | FW -> ROS |
| `/ll/emergency`                    | `mower_msgs/Emergency`     | FW -> ROS |
| `/ll/diff_drive/measured_twist`    | `geometry_msgs/TwistStamped` | FW -> ROS |
| `/ll/cmd_vel`                      | `geometry_msgs/Twist`      | ROS -> FW |
| `/ll/position/gps/rtcm`            | `rtcm_msgs/Message`        | ROS -> FW |
| `/mower_logic/current_state`       | `mower_msgs/HighLevelStatus` | internal (latched) |
| `xbot/action`                      | `std_msgs/String`          | internal  |

### Key Services

| Service                                 | Type                         |
|-----------------------------------------|------------------------------|
| `mower_service/high_level_control`      | `mower_msgs/HighLevelControlSrv` |
| `ll/_service/mow_enabled`               | `mower_msgs/MowerControlSrv` |
| `ll/_service/emergency`                 | `mower_msgs/EmergencyStopSrv`|

> `[NEW]` The `mower_wifi` node is a **client** of `mower_service/high_level_control`
> and `ll/_service/emergency`, and a **subscriber** of `/mower_logic/current_state`.
> It introduces no new ROS message or service type — it reuses the existing ones.

## External WiFi Remote Protocol `[NEW]`

The WiFi feature (FR-9) adds one *external* channel: a network protocol between a
remote client and the `mower_wifi` node on the Raspberry Pi. This is a host-side
protocol and is independent of the firmware link (works on v1 and v2).

### Transport and Framing
- **Transport:** TCP (or WebSocket) over the Raspberry Pi WiFi interface, bound to a
  configurable port (FR-9.1). Deployment assumes a trusted/secured network (NFR-6).
- **Encoding:** line-delimited JSON messages, one object per message.

### Message Types (client → node)

```json
{ "token": "<secret>", "command": "START" }        // FR-9.2 -> COMMAND_START (1)
{ "token": "<secret>", "command": "STOP" }         // FR-9.3 -> COMMAND_HOME  (2)
{ "token": "<secret>", "command": "EMERGENCY" }    // FR-9.4 -> EmergencyStopSrv
{ "token": "<secret>", "command": "RESET_EMERGENCY" } // -> COMMAND_RESET_EMERGENCY (254)
```

- **FR-9.5 auth:** every message carries `token`. The node validates it against the
  configured secret *before* relaying; a mismatch yields
  `{ "error": "unauthorized" }` and no state change (SR-8).

### Status Message (node → client), 1 Hz — FR-9.6

```json
{
  "state": 2, "state_name": "MOWING", "sub_state_name": "",
  "battery_percent": 83.0, "gps_quality_percent": 97.0,
  "is_charging": false, "emergency": false, "stamp": 1730000000.0
}
```

Fields mirror `mower_msgs/HighLevelStatus` read from `/mower_logic/current_state`.

### Command Mapping

| WiFi command      | ROS call                                              | Req. |
|-------------------|-------------------------------------------------------|------|
| `START`           | `HighLevelControlSrv{command=COMMAND_START=1}`        | FR-9.2 |
| `STOP`            | `HighLevelControlSrv{command=COMMAND_HOME=2}`         | FR-9.3 |
| `EMERGENCY`       | `EmergencyStopSrv{emergency=true}` on `ll/_service/emergency` | FR-9.4 |
| `RESET_EMERGENCY` | `HighLevelControlSrv{command=COMMAND_RESET_EMERGENCY=254}` | SR-7 |

The emergency path calls `ll/_service/emergency` directly (not the 1 Hz status loop)
so it meets the < 1 s budget (FR-9.4 / NFR-7).

## Pattern for Adding a New Channel (WiFi)

The established extension pattern, matching the existing services:

1. **Define the contract.** Add a new firmware service ID and field set, or, for v1,
   a new `PACKET_ID_*` and packed struct in `ll_datatypes.h` (type byte + payload +
   CRC).
2. **Bind it in `mower_comms_v2`.** Create a `WifiServiceInterface : public
   WifiServiceInterfaceBase`, instantiate it in `main()` with a service ID and the
   ROS publishers it needs, then call `Start()` - identical to how `GpsServiceInterface`
   / `EmergencyServiceInterface` are wired above.
3. **Expose ROS interfaces.** Advertise topics/services under a stable namespace
   (e.g. `/ll/wifi/...` or a new `/wifi/...` root) so the app and `mower_logic` can
   consume them.
4. **Config.** Carry persistent WiFi settings in the forward-compatible
   `ll_high_level_config` struct (reserved option bits) rather than a new config
   packet, to preserve wire compatibility.

If WiFi is purely host-side (Raspberry Pi networking), it needs no firmware channel
at all - it becomes a new ROS package (see
[06-Software-Architecture](06-Software-Architecture.md)) bridging to the existing
`/mower_logic` command/status interfaces.
