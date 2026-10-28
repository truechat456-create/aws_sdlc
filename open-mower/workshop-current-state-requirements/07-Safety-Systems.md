# 07 - Safety Systems

Baseline safety architecture of OpenMower: the emergency-stop chain, the safety
state, response-time expectations and watchdog mechanisms. Any new feature
(including WiFi command paths) must integrate with - and must not weaken - this chain.

Related: [02-System-Requirements](02-System-Requirements.md) (SR-1..SR-6),
[04-Firmware-Architecture](04-Firmware-Architecture.md),
[08-State-Machine-Design](08-State-Machine-Design.md).

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
