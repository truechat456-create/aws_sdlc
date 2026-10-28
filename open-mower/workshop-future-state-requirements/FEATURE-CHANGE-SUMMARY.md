# Feature Change Summary — WiFi Remote Start/Stop (FR-9)

This document records every change made to the baseline design set
(`workshop-current-state-requirements/`) to produce the future-state set
(`workshop-future-state-requirements/`) for the WiFi remote start/stop feature.

## Feature Overview

Add remote start, stop, emergency-stop and live status over WiFi, implemented as a
single host-side ROS node (`mower_wifi`) on the Raspberry Pi. The node is a thin,
token-authenticated bridge onto the mower's **existing** ROS command and status
interfaces. **No firmware, hardware, message, state-machine or safety-authority
changes are required.**

## Requirements Added

| Req    | Summary                                               |
|--------|-------------------------------------------------------|
| FR-9.1 | WiFi connectivity for remote command reception        |
| FR-9.2 | Remote start command via WiFi                         |
| FR-9.3 | Remote stop command via WiFi                          |
| FR-9.4 | Remote emergency stop, < 1 s response time            |
| FR-9.5 | Token-based authentication for remote commands        |
| FR-9.6 | Status feedback via WiFi at 1 Hz                      |
| NFR-6  | Security: authenticated, configured (not hard-coded) token |
| NFR-7  | Remote responsiveness: < 1 s e-stop, 1 Hz status      |
| NFR-8  | Fail-safe on WiFi link loss                           |
| SR-7   | Remote emergency asserts (not replaces) the chain     |
| SR-8   | Remote safety commands require authentication         |

## Documents Changed

Each baseline document was copied verbatim, then annotated with `[NEW]` (added
sections) and `[MODIFIED]` (changed sections) markers.

| Document | Change type | What changed |
|----------|-------------|--------------|
| 01-System-Architecture | `[MODIFIED]` | Added WiFi subsystem row to subsystem table; new "WiFi Remote Subsystem" and "Updated Cross-Layer Flow" sections; platform-agnostic note. |
| 02-System-Requirements | `[MODIFIED]` | Added FR-9.1–FR-9.6; NFR-6/7/8; SR-7/8. |
| 03-Hardware-Architecture | `[MODIFIED]` | New "WiFi Feature Hardware Impact" section: confirms no mainboard/GPIO/config change (host-side WiFi). |
| 04-Firmware-Architecture | `[MODIFIED]` | Marked the generic plug-in pattern as not exercised by FR-9; new "WiFi Feature: No Firmware Change" section. |
| 05-Communication-Protocols | `[MODIFIED]` | New "External WiFi Remote Protocol" (JSON schemas, command mapping); note that `mower_wifi` reuses existing services. |
| 06-Software-Architecture | `[MODIFIED]` | Added `mower_wifi` to layout, node graph, launch composition and data-flow diagram. |
| 07-Safety-Systems | `[MODIFIED]` | New "WiFi Remote Emergency Stop", "< 1 s budget" table, and "WiFi Authentication and Link Safety" sections. |
| 08-State-Machine-Design | `[MODIFIED]` | New "WiFi Remote Command Mapping" section; confirms no new state/command/behavior. |
| 16-WiFi-Subsystem | `[NEW]` | New document: full subsystem design, node interfaces, parameters, control flow, protocol, safety integration, deployment. |
| FEATURE-CHANGE-SUMMARY | `[NEW]` | This document. |

## Requirement Traceability Matrix

Each FR-9 requirement traced from requirement → design element(s) → document(s).

| Req | Design element | Realized in document(s) |
|-----|----------------|-------------------------|
| FR-9.1 | `mower_wifi` TCP server on Raspberry Pi WiFi; new ROS package; launch entry | 16; 06 (layout/launch); 05 (transport); 01 (subsystem) |
| FR-9.2 | `START` → `HighLevelControlSrv{COMMAND_START=1}` on `mower_service/high_level_control` | 16; 05 (mapping); 08 (command mapping); 02 (FR-9.2) |
| FR-9.3 | `STOP` → `HighLevelControlSrv{COMMAND_HOME=2}` | 16; 05 (mapping); 08 (command mapping); 02 (FR-9.3) |
| FR-9.4 | `EMERGENCY` → `EmergencyStopSrv` on `ll/_service/emergency`, issued immediately; firmware latch final; budget table | 16; 07 (e-stop path + < 1 s budget); 05 (mapping); 04 (no FW change); 08 |
| FR-9.5 | Per-message `token` validated before relay; reject + log on mismatch | 16 (control flow); 05 (protocol); 07 (auth); 02 (FR-9.5, NFR-6, SR-8) |
| FR-9.6 | 1 Hz ROS timer republishing cached `/mower_logic/current_state` as JSON | 16 (status pump); 05 (status message); 08 (status feedback); 02 (FR-9.6) |

### Supporting requirement trace

| Req | Design element | Document(s) |
|-----|----------------|-------------|
| NFR-6 | Token from config/param, never hard-coded; secured network assumed | 16 (params); 07; 02 |
| NFR-7 | Emergency issued off the hot path; 1 Hz status; negligible added latency | 07 (budget); 16; 02 |
| NFR-8 | Firmware heartbeat watchdog unaffected by WiFi link loss | 07; 04; 02 |
| SR-7 | WiFi emergency asserts existing chain; cannot clear LL latch; reset via COMMAND_RESET_EMERGENCY | 07; 08; 02 |
| SR-8 | Start/stop/emergency acted on only after successful auth | 07; 16; 02 |

## Interfaces Reused (no new ROS message/service types)

| Existing interface | Type | Used for |
|--------------------|------|----------|
| `mower_service/high_level_control` | `mower_msgs/HighLevelControlSrv` | START, STOP, RESET_EMERGENCY (FR-9.2/9.3) |
| `ll/_service/emergency` | `mower_msgs/EmergencyStopSrv` | EMERGENCY (FR-9.4) |
| `/mower_logic/current_state` | `mower_msgs/HighLevelStatus` | status feedback (FR-9.6) |

## Explicitly Unchanged

- Firmware (`fw-openmower-v2`, `xesc_firmware`): no new service/packet; Core 0/Core 1
  design, emergency latch and heartbeat watchdog untouched.
- Hardware: no mainboard, GPIO, power-rail or `ll_high_level_config` change.
- State machine: no new state, behavior or `COMMAND_*` constant.
- Safety authority: the firmware low-level emergency latch remains final.

## Assumptions and Out of Scope

- **Assumption:** the WiFi feature is host-side; the Raspberry Pi provides WiFi via
  `OpenMowerOS`. Deployment is on a trusted/secured network (TLS is a future `FR-10`).
- **Out of scope for FR-9:** transport encryption (TLS), WiFi provisioning/pairing,
  and any firmware-resident WiFi module. These are noted as candidate future
  requirements in [16-WiFi-Subsystem](16-WiFi-Subsystem.md).
