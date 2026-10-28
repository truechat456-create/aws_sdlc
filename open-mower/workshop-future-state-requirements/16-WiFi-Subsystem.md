# 16 - WiFi Subsystem `[NEW]`

Design of the WiFi remote start/stop subsystem that satisfies requirements
FR-9.1–FR-9.6. This document is new in the future-state set.

Related: [01-System-Architecture](01-System-Architecture.md),
[02-System-Requirements](02-System-Requirements.md) (FR-9, NFR-6..8, SR-7..8),
[05-Communication-Protocols](05-Communication-Protocols.md),
[06-Software-Architecture](06-Software-Architecture.md),
[07-Safety-Systems](07-Safety-Systems.md),
[08-State-Machine-Design](08-State-Machine-Design.md).

## Purpose and Approach

The WiFi subsystem lets a remote client start, stop and emergency-stop the mower over
WiFi, and receive live status, without any firmware or hardware change. It is a single
host-side ROS node, `mower_wifi`, running on the Raspberry Pi. It is a thin,
authenticated bridge: it translates an external network protocol into the mower's
**existing** ROS command and status interfaces and never touches motors, `/ll/cmd_vel`
or the firmware link directly. This keeps the full safety chain
([07-Safety-Systems](07-Safety-Systems.md)) and state machine
([08-State-Machine-Design](08-State-Machine-Design.md)) intact and platform-agnostic
(works on v1 and v2).

## Component Overview

```
+--------------------+     WiFi / TCP (JSON, token)     +--------------------------+
|  Remote client     | <------------------------------> |  mower_wifi (ROS node)   |
|  (app / script)    |                                  |  - TCP server (FR-9.1)   |
+--------------------+                                  |  - token auth  (FR-9.5)  |
                                                        |  - command mapper        |
                                                        |  - 1 Hz status pump      |
                                                        +------------+-------------+
                                                                     |
     mower_service/high_level_control (START/STOP/RESET) <-----------+
     ll/_service/emergency (EMERGENCY, < 1 s)            <-----------+
     /mower_logic/current_state (HighLevelStatus) ------- (sub) ----->
```

## Requirement-to-Design Mapping

| Req    | Design element                                                            |
|--------|---------------------------------------------------------------------------|
| FR-9.1 | `mower_wifi` opens a TCP listen socket on a configurable port (param).    |
| FR-9.2 | `START` → service call `HighLevelControlSrv{command=COMMAND_START=1}`.     |
| FR-9.3 | `STOP` → service call `HighLevelControlSrv{command=COMMAND_HOME=2}`.       |
| FR-9.4 | `EMERGENCY` → `EmergencyStopSrv{emergency=true}` on `ll/_service/emergency`, issued immediately (< 1 s). |
| FR-9.5 | Every message carries a `token`; validated before any relay; mismatch → rejected, logged, no state change. |
| FR-9.6 | ROS timer at 1 Hz reads latched `/mower_logic/current_state` and sends a JSON status frame to each client. |

## Node Design (`mower_wifi`)

### Interfaces
- **Service clients:** `mower_service/high_level_control`
  (`mower_msgs/HighLevelControlSrv`), `ll/_service/emergency`
  (`mower_msgs/EmergencyStopSrv`).
- **Subscriber:** `/mower_logic/current_state` (`mower_msgs/HighLevelStatus`),
  latched — the node caches the latest message.
- **External:** a TCP server socket (port from param); line-delimited JSON.

### Parameters (`params/wifi.yaml`)
```yaml
wifi:
  port: 8080                # FR-9.1 listen port
  token: "CHANGE_ME"        # FR-9.5 shared secret (NFR-6: not hard-coded)
  status_rate_hz: 1.0       # FR-9.6 status update rate
  bind_ip: "0.0.0.0"
```

### Control Flow (pseudocode)
```python
on_message(conn, msg):
    if msg.token != param.token:            # FR-9.5 / SR-8
        send(conn, {"error": "unauthorized"}); log_warn("rejected"); return
    if   msg.command == "START":            # FR-9.2
        call high_level_control(COMMAND_START)
    elif msg.command == "STOP":             # FR-9.3
        call high_level_control(COMMAND_HOME)
    elif msg.command == "EMERGENCY":        # FR-9.4 (issued immediately, < 1 s)
        call emergency(EmergencyStopSrv(emergency=True))
    elif msg.command == "RESET_EMERGENCY":  # SR-7
        call high_level_control(COMMAND_RESET_EMERGENCY)
    else:
        send(conn, {"error": "unknown_command"})

on_status_timer(1 Hz):                      # FR-9.6
    for conn in clients: send(conn, json_of(latest_high_level_status))

on_current_state(msg):                      # cache latched status
    latest_high_level_status = msg
```

## Protocol

See [05-Communication-Protocols](05-Communication-Protocols.md), "External WiFi
Remote Protocol", for the full message schemas. Summary:
- Client → node: `{ "token", "command" }` where command ∈ {START, STOP, EMERGENCY,
  RESET_EMERGENCY}.
- Node → client (1 Hz): JSON mirror of `HighLevelStatus` (state, state_name,
  battery_percent, gps_quality_percent, is_charging, emergency).

## Safety Integration

- Start/stop go through `mower_service/high_level_control` and are subject to the
  same behavior logic and blade gating (SR-6) as any other command.
- Emergency goes through `ll/_service/emergency`; the firmware latch (SR-1) is the
  final authority — WiFi cannot override or silently clear it (SR-7).
- The < 1 s budget (FR-9.4) is met because the emergency call is issued on receipt,
  not on the 1 Hz loop (see the budget table in
  [07-Safety-Systems](07-Safety-Systems.md)).
- WiFi link loss never disables the firmware heartbeat watchdog (SR-2, NFR-8).

## Deployment

1. Build `mower_wifi` with catkin (package construction follows the "Adding a New
   ROS Package" recipe in [06-Software-Architecture](06-Software-Architecture.md)).
2. Add the node to `open_mower.launch` (or `include/_wifi.launch`), gated by
   `unless="$(optenv OM_NO_WIFI False)"`, loading `params/wifi.yaml`.
3. Set a strong `token` (NFR-6) and deploy on a trusted/secured network.

## Out of Scope (for FR-9)

- Firmware-resident WiFi (ESP companion module) — would use the MCU-side plug-in
  pattern in [04-Firmware-Architecture](04-Firmware-Architecture.md).
- TLS/transport encryption and WiFi provisioning/pairing — candidate future
  requirements (`FR-10.x`), noted but not required by FR-9.
