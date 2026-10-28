# 01 - System Architecture

> **[MODIFIED]** Future-state revision. This document now includes the WiFi remote
> start/stop subsystem (FR-9.1–FR-9.6). New and changed content is marked with
> `[NEW]` / `[MODIFIED]`. See [16-WiFi-Subsystem](16-WiFi-Subsystem.md) and
> [FEATURE-CHANGE-SUMMARY](FEATURE-CHANGE-SUMMARY.md).

Baseline architecture of the OpenMower project, captured as the foundation for the
WiFi feature integration. This document defines the three-layer system model and the
boundaries that any new subsystem (including WiFi) must respect.

Related documents: [03-Hardware-Architecture](03-Hardware-Architecture.md),
[04-Firmware-Architecture](04-Firmware-Architecture.md),
[05-Communication-Protocols](05-Communication-Protocols.md),
[06-Software-Architecture](06-Software-Architecture.md),
[16-WiFi-Subsystem](16-WiFi-Subsystem.md).

## Three-Layer Overview

OpenMower converts an off-the-shelf robotic mower (e.g. YardForce Classic 500) into a
GPS-guided, map-aware, app-controlled machine. The system is split into three layers,
each owned by a dedicated repository in the ecosystem:

```
+-----------------------------------------------------------------+
|  ROS SOFTWARE LAYER  (Raspberry Pi / Linux, ROS1)               |
|  open_mower_ros - navigation, planning, state machine, comms    |
+-----------------------------------------------------------------+
            ^  serial (v1 COBS) / IP service bus (v2 xbot_framework)
            v
+-----------------------------------------------------------------+
|  FIRMWARE LAYER  (RP2040 mainboard + xESC motor controllers)    |
|  fw-openmower-v2 - motor control, sensors, HW abstraction       |
|  xesc_firmware   - per-motor VESC-style controller firmware     |
+-----------------------------------------------------------------+
            ^  UART / SPI / I2C / GPIO / PWM
            v
+-----------------------------------------------------------------+
|  HARDWARE LAYER  (mainboard, motors, sensors, battery, charger) |
|  hw-openmower-universal / -yardforce / -sabo                    |
+-----------------------------------------------------------------+
```

The repositories backing each layer (per the project README):

| Layer    | Repository                          | Responsibility                                  |
|----------|-------------------------------------|-------------------------------------------------|
| Software | `open_mower_ros`                    | ROS navigation, planning and state machine      |
| Software | `openmower-app`                     | Mobile and web app                              |
| Software | `OpenMowerOS`                       | Operating system image                          |
| Firmware | `fw-openmower-v2`                   | Mainboard firmware: motor control, sensors, HAL |
| Firmware | `xesc_firmware`                     | xESC motor controller firmware                  |
| Hardware | `hw-openmower-universal/-yardforce/-sabo` | Mainboard PCB designs                     |

## Layer Responsibilities

### Hardware Layer
The mainboard carries the RP2040 microcontroller, power/charging circuitry, sensor
front-ends (IMU, ultrasonic, rain, hall sensors) and connectors to the three motors
(left drive, right drive, mow) driven by xESC controllers. See
[03-Hardware-Architecture](03-Hardware-Architecture.md).

### Firmware Layer
The mainboard firmware runs the real-time control loop: it reads sensors, drives
motors through the xESCs, enforces the low-level emergency latch, and exposes its
state to the Raspberry Pi. The xESC firmware runs closed-loop motor control.
See [04-Firmware-Architecture](04-Firmware-Architecture.md).

### ROS Software Layer
The Raspberry Pi runs ROS1 (Noetic). It performs localization (GPS RTK + IMU +
wheel odometry Kalman fusion), path planning (coverage planner + move_base_flex),
the high-level behavior state machine (`mower_logic`), map management, and the
bridge to the firmware (`mower_comms_v1` / `mower_comms_v2`).
See [06-Software-Architecture](06-Software-Architecture.md).

## Main Subsystem Boundaries

| Subsystem        | Layer    | Key component(s)                               |
|------------------|----------|------------------------------------------------|
| Motion control   | FW + SW  | xESC firmware, `DiffDriveServiceInterface`     |
| Localization     | SW       | `xbot_positioning`, `xbot_driver_gps`          |
| Navigation       | SW       | `move_base_flex`, `ftc_local_planner`, slic3r  |
| Behavior / state | SW       | `mower_logic` (Behavior state machine)         |
| Mapping          | SW       | `mower_map`                                    |
| Low-level bridge | SW       | `mower_comms_v1` / `mower_comms_v2`            |
| Safety / E-Stop  | FW + SW  | LL emergency latch, `EmergencyServiceInterface`|
| Monitoring / app | SW       | `xbot_monitoring`, `xbot_mqtt`, `openmower-app`|
| **WiFi remote `[NEW]`** | **SW** | **`mower_wifi` node (FR-9.1–FR-9.6)**   |

### WiFi Remote Subsystem `[NEW]`

The WiFi remote start/stop feature (FR-9.1–FR-9.6) is added as a **host-side ROS
subsystem** on the Raspberry Pi — the lowest-risk path identified in the baseline
([03-Hardware-Architecture](03-Hardware-Architecture.md), "Available Resources for
Expansion"). It introduces **no mainboard or firmware changes**: the Raspberry Pi
already has WiFi networking.

The subsystem is a new ROS package, `mower_wifi`, that exposes an authenticated
network endpoint and bridges remote clients to the **existing** control and status
interfaces:

- Commands → `mower_service/high_level_control` (`HighLevelControlSrv`) — FR-9.2/9.3.
- Emergency → `ll/_service/emergency` (`EmergencyStopSrv`) directly — FR-9.4.
- Status ← `/mower_logic/current_state` (`HighLevelStatus`, latched) — FR-9.6.

Because `mower_wifi` sits entirely in the ROS Software layer and reuses the existing
command path, it inherits the full safety chain
([07-Safety-Systems](07-Safety-Systems.md)) and the state machine
([08-State-Machine-Design](08-State-Machine-Design.md)) unchanged. Full design in
[16-WiFi-Subsystem](16-WiFi-Subsystem.md).

## Cross-Layer Data Flow

1. The firmware publishes low-level status (battery, charging, rain, emergency,
   IMU, wheel ticks, GPS) upward to the ROS layer.
2. `mower_comms_*` translates those into ROS topics under the `/ll/...` namespace
   (e.g. `/ll/mower_status`, `/ll/imu/data_raw`, `/ll/position/gps`, `/ll/emergency`).
3. `xbot_positioning` fuses GPS + IMU + odometry into `/xbot_positioning/xb_pose`.
4. `mower_logic` runs the behavior state machine and emits velocity commands and a
   high-level status (`/mower_logic/current_state`).
5. Velocity commands pass through `twist_mux` to `/ll/cmd_vel`, back down through
   `mower_comms_*` to the firmware and the drive xESCs.
6. The emergency path is bidirectional: either layer can latch an emergency, and
   the ROS layer must send periodic heartbeats to keep the firmware armed
   (see [07-Safety-Systems](07-Safety-Systems.md)).

## Hardware Platform Versions

Two platform generations coexist, selected by the `HARDWARE_PLATFORM` environment
variable in `_comms.launch`:

- **v1** (`HARDWARE_PLATFORM=1`): serial COBS protocol via `mower_comms_v1`, GPS
  driven by a separate `xbot_driver_gps` node on the Pi.
- **v2** (`HARDWARE_PLATFORM=2`): `xbot_framework` IP-based service bus via
  `mower_comms_v2`, with GPS handled as a firmware service.

The WiFi feature targets this split explicitly: the v2 service-bus model is the
primary extension surface. See [05-Communication-Protocols](05-Communication-Protocols.md)
for the pattern used to add a new communication channel.

> `[MODIFIED]` For the WiFi remote feature, the chosen integration is host-side and
> therefore **platform-agnostic**: `mower_wifi` works identically on v1 and v2
> because it talks to the version-independent ROS command/status interfaces, not the
> firmware link. The WiFi channel itself is the Raspberry Pi network stack, not a new
> firmware packet/service.

## Updated Cross-Layer Flow (WiFi) `[NEW]`

```
remote client --WiFi/TCP(token auth)--> mower_wifi (ROS, Raspberry Pi)
    start/stop   --> mower_service/high_level_control (HighLevelControlSrv)
    e-stop       --> ll/_service/emergency (EmergencyStopSrv)   [< 1 s, FR-9.4]
    status (1Hz) <-- /mower_logic/current_state (HighLevelStatus) [FR-9.6]
mower_wifi never drives motors or /ll/cmd_vel directly.
```
