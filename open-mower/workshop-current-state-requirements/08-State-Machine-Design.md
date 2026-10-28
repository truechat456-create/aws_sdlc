# 08 - State Machine Design

Baseline operational state machine of OpenMower, implemented in `mower_logic` as a
polymorphic State pattern. Documents the states, transitions and how start/stop
commands flow - the surface a WiFi command path integrates with.

Related: [05-Communication-Protocols](05-Communication-Protocols.md),
[06-Software-Architecture](06-Software-Architecture.md),
[07-Safety-Systems](07-Safety-Systems.md).

## Design Pattern

Each operational state is a subclass of an abstract `Behavior`
(`mower_logic/src/mower_logic/behaviors/Behavior.h`). Each concrete behavior is a
process-wide singleton (e.g. `IdleBehavior::INSTANCE`). The current state is a single
pointer:

```cpp
Behavior* currentBehavior = &IdleBehavior::INSTANCE;
```

### Behavior Interface (key virtuals)

```cpp
virtual void enter() = 0;            // once, on entry (via start())
virtual Behavior* execute() = 0;     // BLOCKS; returns the NEXT behavior
virtual void exit() = 0;             // once, before leaving
virtual void reset() = 0;
virtual bool needs_gps() = 0;        // abort/resume when GPS lost
virtual bool mower_enabled() = 0;    // should the blade run
virtual bool redirect_joystick() = 0;
virtual uint8_t get_state();         // -> HighLevelStatus enum
virtual uint8_t get_sub_state();
// command hooks:
virtual void command_home()  = 0;
virtual void command_start() = 0;
virtual void command_s1()    = 0;
virtual void command_s2()    = 0;
virtual void handle_action(std::string action) = 0;
```

All transitions happen by `execute()` returning a pointer to the next behavior; the
command hooks only mutate flags on the running behavior (they never set
`currentBehavior` directly).

## Operational States

| Behavior                 | state_name       | `get_state()`                   | sub |
|--------------------------|------------------|---------------------------------|-----|
| `IdleBehavior`           | IDLE             | `HIGH_LEVEL_STATE_IDLE` (1)     | 0   |
| `MowingBehavior`         | MOWING / PAUSED  | `HIGH_LEVEL_STATE_AUTONOMOUS`(2)| 0   |
| `DockingBehavior`        | DOCKING          | `HIGH_LEVEL_STATE_AUTONOMOUS`(2)| 1   |
| `UndockingBehavior`      | UNDOCKING        | `HIGH_LEVEL_STATE_AUTONOMOUS`(2)| 2   |
| `AreaRecordingBehavior`  | AREA_RECORDING   | `HIGH_LEVEL_STATE_RECORDING`(3) | var |

`HighLevelStatus` state enum: `NULL=0, IDLE=1, AUTONOMOUS=2, RECORDING=3`, with
sub-states `SUBSTATE_1..4` packed via `SUBSTATE_SHIFT=6`.

## Transitions

```
                 no map / no dock point
   IdleBehavior ----------------------------> AreaRecordingBehavior
       |  ^                                           |
       |  |  (returns to Idle when done/aborted)      |
       |  +--------------------------------------------+
       |
 start (docked, charging)        start (not docked)
       |                                 |
       v                                 v
 UndockingBehavior --success--> MowingBehavior
       |  \                           |
       |   \--(fail/no GPS)--> Idle   | plan done / aborted (command_home)
       |                              v
       |                        DockingBehavior
       |  RETRY_INSTANCE              |
       +----(dock fail retry)<--------+
                                      |
                           charging / success
                                      v
                           IdleBehavior::DOCKED_INSTANCE
```

- **Idle is the hub.** `IdleBehavior::execute()` chooses the next state based on map
  availability, mode (manual/semiauto/auto) and readiness (battery full, motors cool,
  not rain-delayed). Docked starts go through undocking first.
- **Mowing -> Docking** on plan completion or `command_home()` (abort).
- **Docking -> Idle/DOCKED** on success, or retry via `UndockingBehavior::RETRY_INSTANCE`.
- **AreaRecording -> Idle** when recording finishes or is aborted.

## Command Handling Flow (start / stop / home)

External commands arrive at `mower_service/high_level_control`
(`HighLevelControlSrv`), advertised in `mower_logic` main:

```cpp
n->advertiseService("mower_service/high_level_control", highLevelCommand);
```

Command constants (`HighLevelControlSrv.srv`):

```
COMMAND_START=1   COMMAND_HOME=2   COMMAND_S1=3   COMMAND_S2=4
COMMAND_RESET_EMERGENCY=254        COMMAND_DELETE_MAPS=255
```

`highLevelCommand()` dispatches to the current behavior's hooks:

| Command                  | Effect                                                     |
|--------------------------|------------------------------------------------------------|
| `COMMAND_START`          | `currentBehavior->command_start()` (start/resume mowing)   |
| `COMMAND_HOME`           | `currentBehavior->command_home()` (go dock / abort)        |
| `COMMAND_S1`             | `currentBehavior->command_s1()` (record / pause)           |
| `COMMAND_S2`             | `currentBehavior->command_s2()` (skip area)                |
| `COMMAND_DELETE_MAPS`    | clear map (only idle/docked/recording) then abort          |
| `COMMAND_RESET_EMERGENCY`| `setEmergencyMode(false)`                                  |

The driving loop then realizes the transition:

```cpp
currentBehavior->start(config, &sharedState);
Behavior* newBehavior = currentBehavior->execute();   // blocks until done
currentBehavior->exit();
currentBehavior = newBehavior;                         // state swap
```

So a "start" command sets a flag (e.g. `manual_start_mowing`) on `IdleBehavior`; its
blocking `execute()` observes the flag, returns the next behavior, and the loop
swaps it in. A parallel path routes `xbot/action` strings to
`currentBehavior->handle_action()`.

## Status Publication

`updateUI()` (1 Hz) publishes `HighLevelStatus` on `/mower_logic/current_state`
(latched), packing `state = (get_state() & 0b11111) | (get_sub_state() <<
SUBSTATE_SHIFT)` plus `state_name`, `sub_state_name`, area/path indices.
`checkSafety()` (0.5 Hz) fills `emergency`, `is_charging`, `gps_quality_percent`,
`battery_percent` on the same struct. See [07-Safety-Systems](07-Safety-Systems.md).

## Extension Points for WiFi

1. **Reuse commands (preferred).** A WiFi/app client calls
   `mower_service/high_level_control` with existing `COMMAND_*` values and subscribes
   `/mower_logic/current_state` for feedback - no state-machine changes required.
2. **New command.** Add a `COMMAND_*` constant in `HighLevelControlSrv.srv` and a
   matching `case` in `highLevelCommand()`, then a hook on the relevant behaviors.
3. **New state.** Subclass `Behavior`, implement the pure virtuals, add a static
   `INSTANCE`, and wire its transitions into `IdleBehavior::execute()` (and any state
   that should reach it). Map it to a `HighLevelStatus` state value.

In all cases commands pass through the existing safety gating in
[07-Safety-Systems](07-Safety-Systems.md); a WiFi command cannot override a latched
emergency.
