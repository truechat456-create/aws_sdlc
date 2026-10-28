# 02 - System Requirements

Current (baseline) requirements of the OpenMower system, derived from the existing
firmware and ROS implementation. The numbered format (`FR-X.Y`, `NFR-X`, `SR-X`)
is designed so that new WiFi requirements can be appended without renumbering.

Related: [01-System-Architecture](01-System-Architecture.md),
[07-Safety-Systems](07-Safety-Systems.md),
[08-State-Machine-Design](08-State-Machine-Design.md).

## Functional Requirements

### FR-1 Autonomous Mowing
- FR-1.1 The system shall mow configured areas autonomously using a coverage plan.
- FR-1.2 The system shall skip to the next area on the S2 command (`COMMAND_S2`).
- FR-1.3 The system shall support multiple mowing areas without a perimeter wire.

### FR-2 Localization
- FR-2.1 The system shall fuse GPS (RTK), IMU and wheel odometry into an absolute
  pose (`/xbot_positioning/xb_pose`).
- FR-2.2 The system shall report GPS quality as a percentage (0-100) in the
  high-level status.
- FR-2.3 The system shall pause autonomous motion when GPS quality is lost for
  behaviors where `needs_gps()` is true.

### FR-3 Navigation
- FR-3.1 The system shall plan paths with `move_base_flex` and follow them with
  `ftc_local_planner`.
- FR-3.2 The system shall generate coverage paths via `slic3r_coverage_planner`.

### FR-4 Docking and Charging
- FR-4.1 The system shall drive to and dock at a recorded docking point.
- FR-4.2 The system shall undock before starting a mowing run from the dock.
- FR-4.3 The system shall report charging state (`is_charging`) and battery percent.

### FR-5 Mapping
- FR-5.1 The system shall record mowing areas and a docking point
  (`AreaRecordingBehavior`, `COMMAND_S1`).
- FR-5.2 The system shall persist and serve the map via `mower_map_service`.
- FR-5.3 The system shall clear all maps on `COMMAND_DELETE_MAPS` (254/255 range).

### FR-6 Command and Control
- FR-6.1 The system shall accept high-level commands through
  `mower_service/high_level_control` (`HighLevelControlSrv`).
- FR-6.2 Supported commands: `COMMAND_START=1`, `COMMAND_HOME=2`, `COMMAND_S1=3`,
  `COMMAND_S2=4`, `COMMAND_RESET_EMERGENCY=254`, `COMMAND_DELETE_MAPS=255`.
- FR-6.3 The system shall accept string actions on topic `xbot/action`.
- FR-6.4 The system shall accept teleop velocity commands, multiplexed by
  `twist_mux` onto `/ll/cmd_vel`.

### FR-7 Status Reporting
- FR-7.1 The system shall publish a latched high-level status
  (`/mower_logic/current_state`, `HighLevelStatus`) including state, sub-state,
  area/path indices, GPS quality, battery percent, charging and emergency flags.
- FR-7.2 The firmware shall publish low-level status (battery voltage/current,
  charging, rain, sound/UI availability, emergency bitmask) upward.
- FR-7.3 The system shall expose monitoring to the app via `xbot_monitoring`.

### FR-8 Remote / App Interface
- FR-8.1 The system shall expose a remote control and monitoring interface
  consumed by `openmower-app` (via `xbot_monitoring` / `xbot_mqtt` /
  `rosbridge`).
- FR-8.2 Remote velocity commands shall be routed through `/joy_vel` into
  `twist_mux`.

> WiFi extension note: new WiFi functional requirements should be appended here as
> `FR-9.x` (connectivity), `FR-10.x` (provisioning/config), etc., and new commands
> as additional `COMMAND_*` constants (FR-6.2).

## Non-Functional Requirements

- NFR-1 Portability: ROS software shall run on Raspberry Pi class hardware under
  ROS1 Noetic within the `OpenMowerOS` image.
- NFR-2 Resilience: ROS nodes shall auto-restart on failure (`respawn="true"`,
  `respawn_delay="10"` in launch files).
- NFR-3 Hardware-variant support: the system shall support v1 (serial) and v2
  (IP service bus) platforms, selected by `HARDWARE_PLATFORM`.
- NFR-4 Configurability: per-robot parameters shall be provided via
  `params/hardware_specific/<model>/*.yaml`.
- NFR-5 Determinism: the firmware control/emergency loop shall run with
  bounded, real-time latency on the RP2040 (see [04](04-Firmware-Architecture.md)).

## Safety Requirements

- SR-1 Low-level emergency latch: the firmware shall latch an emergency on lift,
  tilt, stop-button or collision and immediately zero all motor outputs.
- SR-2 Heartbeat watchdog: the firmware shall latch emergency if the ROS layer
  stops sending heartbeats (ROS sends at ~2 Hz / every 0.5 s).
- SR-3 High-level emergency: the ROS layer shall be able to assert emergency
  (`EmergencyStopSrv` / `EmergencyServiceInterface`) e.g. on navigation error.
- SR-4 Latched behavior: an emergency shall remain latched until explicitly
  cleared via `COMMAND_RESET_EMERGENCY` or the `mower_logic/reset_emergency` action.
- SR-5 Command timeout: drive outputs shall be zeroed if no `cmd_vel` is received
  within 1 s; mow output zeroed after 25 s (v1 `publishActuators`).
- SR-6 Blade gating: the blade shall run only when the current behavior requests
  it AND `mowerAllowed` is true AND no safety check tripped this cycle.

See [07-Safety-Systems](07-Safety-Systems.md) for the full safety architecture that
WiFi-triggered commands must integrate with.
