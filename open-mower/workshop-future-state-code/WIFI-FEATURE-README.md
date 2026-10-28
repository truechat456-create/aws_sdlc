# WiFi Remote Start/Stop (FR-9) — Implementation

This folder is the **future-state code** for the WiFi remote start/stop feature.
It is a copy of `workshop-current-state-code/` plus the FR-9 implementation,
derived from the design set in `workshop-future-state-requirements/`
(see `16-WiFi-Subsystem.md`, `05-Communication-Protocols.md`, `04-Firmware-Architecture.md`).

## What was added

| # | Deliverable | Location |
|---|-------------|----------|
| 1 | WiFi module firmware (RP2040) | `OpenMower/firmware/fw-openmower-v2/src/wifi/wifi_module.{h,cpp}`, `wifi_datatypes.h`, `wifi_crc.h`, `COBS.h` |
| 2 | Modified firmware `main.cpp` integrating WiFi packet handling | `OpenMower/firmware/fw-openmower-v2/src/main.cpp` |
| 3 | New ROS package with WebSocket server on port **8765** | `open_mower_ros/src/mower_wifi_bridge/` |
| 4 | Token-based authentication | `protocol.authorize()` (host) + `WifiModule` token check (firmware) |
| 5 | Start/stop/emergency-stop command handlers | `protocol.map_command()`, `bridge_node._execute()`, firmware `dispatchCommand()` |
| 6 | Status monitoring at 1 Hz | `bridge_node._on_status_timer` (host), `WifiModule::tick` (firmware) |
| 7 | Test client script | `open_mower_ros/src/mower_wifi_bridge/scripts/wifi_test_client.py` |
| 8 | Build and validation scripts | `scripts/build.sh`, `scripts/validate.sh`, `scripts/e2e_smoke.py` |

## Two layers, one feature

The requirements set specifies the FR-9 feature primarily as a **host-side** ROS
node (`16-WiFi-Subsystem.md`) and documents a **firmware-resident** WiFi module as
the MCU-side plug-in pattern for a future/companion-radio variant
(`04-Firmware-Architecture.md`, "How a New Module (WiFi) Plugs In"). This task asked
for both, so both are implemented and kept consistent:

- **Host-side (primary): `mower_wifi_bridge`** — a token-authenticated WebSocket
  server (port 8765) that relays onto the mower's **existing** ROS services/topics.
  It introduces no new ROS message/service types.
  - `START` → `mower_service/high_level_control` `COMMAND_START` (FR-9.2)
  - `STOP` → `mower_service/high_level_control` `COMMAND_HOME` (FR-9.3)
  - `EMERGENCY` → `ll/_service/emergency` `{emergency=1}`, issued immediately (FR-9.4, < 1 s)
  - `RESET_EMERGENCY` → `COMMAND_RESET_EMERGENCY` (SR-7)
  - 1 Hz status from `/mower_logic/current_state` (FR-9.6)

- **Firmware (companion-radio variant): `wifi_module`** — follows the Core 1 service
  plug-in pattern. It authenticates packets (FR-9.5), forwards high-level commands to
  the host, and on `EMERGENCY` asserts the Core 0 emergency latch directly (FR-9.4).
  It can only **assert**, never clear, the latch, so the firmware remains the final
  safety authority (SR-1, SR-7).

The on-the-wire command codes match `mower_msgs/HighLevelControlSrv` on both layers,
so the two paths map 1:1.

## Build

```bash
./scripts/build.sh
```

- Installs `g++` if missing, builds the firmware WiFi module natively (host G++),
  confirms the integrated firmware `main.cpp` compiles, and builds the ROS package
  with catkin when a ROS toolchain is present (otherwise runs a Python syntax check,
  since ROS builds run inside the OpenMower container).

## Validate

```bash
./scripts/validate.sh
```

Runs three stages, all of which must pass:
1. **Firmware module** native test — auth accept/reject, START/STOP forward,
   EMERGENCY assert, CRC rejection, 1 Hz status pump.
2. **Host bridge logic** — real WebSocket handshake + framing, token auth, command
   mapping/execution, verified ~1 Hz status broadcast.
3. **End-to-end smoke** — the shipped `wifi_test_client.py` driven against a fake
   bridge server (FR-9.1/9.2/9.4/9.5/9.6).

## Manual use (against a running mower)

```bash
# watch status
rosrun mower_wifi_bridge wifi_test_client.py --host <mower-ip> --token <SECRET> --listen 5

# start / stop / emergency
rosrun mower_wifi_bridge wifi_test_client.py --host <mower-ip> --token <SECRET> --command START
rosrun mower_wifi_bridge wifi_test_client.py --host <mower-ip> --token <SECRET> --command EMERGENCY
```

The node loads `params/wifi.yaml`; set a strong `token` (NFR-6 — never hard-coded)
and deploy only on a trusted/secured network (TLS is a future FR-10, out of scope).

## Requirement traceability

Every FR-9 requirement is referenced in code comments at its implementation site.

| Req | Firmware (`src/wifi/…`, `main.cpp`) | Host (`mower_wifi_bridge/…`) |
|-----|-------------------------------------|------------------------------|
| FR-9.1 connectivity | companion-radio UART intake (`feedByte`) | `ws_server` bind on port 8765 |
| FR-9.2 START | `WIFI_CMD_START` → `on_high_level_command` | `map_command` → `COMMAND_START` |
| FR-9.3 STOP | `WIFI_CMD_STOP` → `on_high_level_command` | `map_command` → `COMMAND_HOME` |
| FR-9.4 EMERGENCY (<1 s) | `WIFI_CMD_EMERGENCY` → `on_emergency` (latch) | `_execute` → `EmergencyStopSrv` immediately |
| FR-9.5 auth | `tokenMatches` (constant-time) before dispatch | `authorize` (`hmac.compare_digest`) before relay |
| FR-9.6 status 1 Hz | `WifiModule::tick` emits status packet | `rospy.Timer` broadcasts JSON status |
| SR-7 reset | `WIFI_CMD_RESET_EMERGENCY` forwarded, cannot clear latch | `COMMAND_RESET_EMERGENCY` via high-level service |
| NFR-6 token | injected via `begin(token,…)`, not hard-coded | `~token` ROS param |
```
