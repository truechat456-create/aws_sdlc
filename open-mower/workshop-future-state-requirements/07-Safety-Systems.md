# 07 - Safety Systems

> **[MODIFIED]** Future-state revision. Adds the WiFi remote emergency-stop path
> (FR-9.4) and its < 1 s response-time budget, plus WiFi auth/link-loss safety rules
> (SR-7, SR-8, NFR-6, NFR-8). New content marked `[NEW]`. The baseline safety
> authority is unchanged — the firmware latch remains final.

Baseline safety architecture of OpenMower: the emergency-stop chain, the safety
state, response-time expectations and watchdog mechanisms. Any new feature
(including WiFi command paths) must integrate with - and must not weaken - this chain.

Related: [02-System-Requirements](02-System-Requirements.md) (SR-1..SR-8),
[04-Firmware-Architecture](04-Firmware-Architecture.md),
[08-State-Machine-Design](08-State-Machine-Design.md),
[16-WiFi-Subsystem](16-WiFi-Subsystem.md).

## Emergency Stop Chain

Safety is enforced at two levels, with the **firmware as the final authority**:

```
Low Level (firmware, RP2040)                High Level (ROS)
----------------------------                ----------------
lift / tilt / stop-button / collision       navigation error, lost sensors,
  -> set emergency_bitmask (LATCH)            ESC fault, user E-stop
  -> zero all motor outputs immediately       -> EmergencyStopSrv / heartbeat flag
                                               -> request LL latch
        ^                                                 |
        |------------- heartbeat (every 0.5s) ------------|
        v
  loss of heartbeat -> LL latches emergency (watchdog)
```

### Low-Level Emergency Bits (`ll_datatypes.h`)

```c
#define LL_EMERGENCY_BIT_LATCH (1 << 0)  // Any emergency latch
#define LL_EMERGENCY_BIT_STOP  (1 << 1)  // Stop
#define LL_EMERGENCY_BIT_LIFT  (1 << 2)  // Lift (or tilt)
```

The CoverUI variant adds STOP1/STOP2, LIFT/LIFTX and LBUMP/RBUMP bits. The firmware
sets `LL_EMERGENCY_BIT_LATCH` whenever any condition trips, and clears motor outputs
regardless of host state.

## High-Level Emergency (ROS)

v1 tracks emergency with three flags in `mower_comms_v1` and mirrors the LL bitmask:

```cpp
bool emergency_high_level;   // ROS thinks there should be an emergency
bool emergency_low_level;    // LL board thinks there should be an emergency
bool is_emergency() { return emergency_high_level || emergency_low_level; }
```

`publishActuators()` zeroes `speed_l/speed_r/speed_mow` when `is_emergency()` is
true, and sends a heartbeat each cycle requesting the LL latch when HL has emergency
but LL does not yet know.

v2 models emergency as a service with a reason bitmask
(`EmergencyServiceInterface`). `SetHighLevelEmergency(true)` sets
`HIGH_LEVEL | LATCH`; the periodic `Heartbeat()` re-asserts the HL reason. Known
reasons: `LATCH`, `TIMEOUT_INPUTS`, `STOP`, `LIFT`, `LIFT_MULTIPLE`, `COLLISION`,
`TIMEOUT_HIGH_LEVEL`, `HIGH_LEVEL`. On service disconnect the interface defaults to
`TIMEOUT_HIGH_LEVEL` (fail-safe).

The ROS `Emergency` message published on `/ll/emergency`:

```
time stamp
bool active_emergency
bool latched_emergency
string reason
```

## Safety State Machine (in `mower_logic`)

`setEmergencyMode(bool)` is the choke point: it calls `stopBlade()` + `stopMoving()`
then invokes the `ll/_service/emergency` service (with retry). `stopBlade()` sets the
global `std::atomic<bool> mowerAllowed = false`.

`checkSafety()` runs on a ~0.5 Hz timer and each tick:

1. Sets `mowerAllowed = true`, then reads the latched emergency.
2. If latched: `currentBehavior->requestPause(PAUSE_EMERGENCY)`. If the robot is
   idle/recording/docked AND charging (cannot move), it auto-clears via
   `setEmergencyMode(false)`; otherwise it keeps the pause.
3. On sensor timeout or ESC error it calls `setEmergencyMode(true)` directly.
4. Finally gates the blade:
   `setMowerEnabled(currentBehavior != nullptr && mowerAllowed && currentBehavior->mower_enabled())`.

So the blade runs only when all three agree (SR-6). If `currentBehavior` ever
becomes `nullptr`, the driving loop forces `setEmergencyMode(true)`.

### Latch and Reset

An emergency stays latched until explicitly cleared by:
- `COMMAND_RESET_EMERGENCY` (254) on `HighLevelControlSrv`,
- the `mower_logic/reset_emergency` string action on `xbot/action`,
- the auto-clear-when-docked logic above,
- once at startup (`setEmergencyMode(false)` before the main loop).

## Response Time and Watchdogs

| Mechanism                 | Timing / behavior                                    |
|---------------------------|------------------------------------------------------|
| LL emergency latch        | Immediate motor cut-off in the Core 0 control loop   |
| Heartbeat (ROS -> FW)      | Sent every 0.5 s (`sendEmergencyHeartbeatTimerTask`) |
| Heartbeat watchdog (FW)   | FW latches emergency if heartbeats stop (SR-2)       |
| `cmd_vel` timeout         | Drive speeds zeroed after 1 s without `cmd_vel`      |
| Mow timeout               | Mow speed zeroed after 25 s without `cmd_vel`        |
| Safety sweep              | `checkSafety()` ~every 0.5 s re-evaluates gating     |

## Integration Rules for New Features (WiFi)

- A WiFi-initiated start/stop command MUST go through
  `mower_service/high_level_control`, not directly to motors or `/ll/cmd_vel`.
- WiFi MUST be able to trigger an emergency (recommended: a reason bit / the
  existing reset command) but MUST NOT be able to silently clear a latched
  low-level emergency - only the firmware can release its own latch once conditions
  clear and a reset is requested.
- Loss of the WiFi link MUST NOT disable the existing heartbeat/watchdog path; the
  firmware watchdog remains the ultimate guarantee (SR-2).
- Any new emergency reason surfaced over WiFi should map onto the existing v2
  `EmergencyReason` bitmask rather than introducing a parallel mechanism.

## WiFi Remote Emergency Stop (FR-9.4) `[NEW]`

The `mower_wifi` node implements remote emergency stop by calling the **existing**
`ll/_service/emergency` service (`EmergencyStopSrv`, `emergency=true`) as soon as an
authenticated `EMERGENCY` message is received. This is the same entry point used by
`mower_logic::setEmergencyMode(true)` and by the v2 `EmergencyServiceInterface`, so
it inherits the full latch behavior (SR-1, SR-4) with no new safety mechanism.

Design rules for the remote emergency path:

- **Direct, not polled.** The emergency call does NOT wait on the 1 Hz status loop
  (FR-9.6); it is issued immediately on receipt so it meets the < 1 s budget
  (FR-9.4, NFR-7).
- **Assert, don't replace (SR-7).** A WiFi emergency asserts the existing chain; it
  cannot clear a latched low-level emergency. Reset must use
  `COMMAND_RESET_EMERGENCY` and is subject to the usual conditions.
- **Authenticated (SR-8).** The `EMERGENCY` message is token-checked (FR-9.5), but
  the check is a single in-memory comparison so it adds negligible latency.
- **Link-independent fail-safe (NFR-8).** If the WiFi link drops, the firmware
  heartbeat watchdog (SR-2) still latches emergency on its own; remote absence never
  makes the robot less safe.

### Remote E-Stop Response-Time Budget (< 1 s, FR-9.4) `[NEW]`

| Stage                                              | Target        |
|----------------------------------------------------|---------------|
| WiFi message receive + token check (`mower_wifi`)  | < 50 ms       |
| `EmergencyStopSrv` call → `mower_comms_*`          | < 100 ms      |
| `mower_comms_*` → firmware latch + motor cut-off   | < 100 ms      |
| **End-to-end remote emergency**                    | **< 1 s** ✔   |

The dominant real guarantee is still the firmware latch, which zeroes motor outputs
immediately once the request reaches it.

## WiFi Authentication and Link Safety `[NEW]`

- **SR-8 / FR-9.5:** `mower_wifi` rejects any command without a valid token and does
  not relay it; rejections are logged (NFR-6) and leave mower state unchanged.
- **NFR-6:** the token is read from configuration (param/file), never hard-coded,
  and the deployment assumes a trusted/secured network link.
- **NFR-8:** WiFi is additive — all baseline watchdogs (heartbeat, `cmd_vel`
  timeout, mow timeout, `checkSafety` sweep) continue to operate regardless of WiFi
  connectivity.
