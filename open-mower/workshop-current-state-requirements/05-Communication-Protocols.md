# 05 - Communication Protocols

Baseline communication protocols between firmware and ROS, and the ROS-internal
topic/service interfaces. Ends with the pattern for adding a new communication
channel (the WiFi integration point).

Related: [04-Firmware-Architecture](04-Firmware-Architecture.md),
[06-Software-Architecture](06-Software-Architecture.md),
[07-Safety-Systems](07-Safety-Systems.md).

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
