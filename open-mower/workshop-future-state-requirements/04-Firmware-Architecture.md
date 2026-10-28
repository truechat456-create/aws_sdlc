# 04 - Firmware Architecture

> **[MODIFIED]** Future-state revision. The WiFi remote feature (FR-9.1–FR-9.6) is
> host-side and adds **no firmware module or service**. This document retains the
> baseline module/service plug-in pattern (still valid for any future MCU-side
> feature) and adds a note confirming firmware is unchanged for FR-9. See
> [16-WiFi-Subsystem](16-WiFi-Subsystem.md).

Baseline firmware design for the RP2040 mainboard and the module/service pattern that
a new subsystem (such as WiFi) plugs into.

> Scope note: the firmware source (`fw-openmower-v2`, `xesc_firmware`) is a git
> submodule not checked out in this workspace. The firmware-facing contract below is
> reconstructed from the authoritative ROS side (`mower_comms_v1`, `mower_comms_v2`)
> which mirrors the firmware packet and service definitions exactly, plus the project
> README. Treat core/loop timing figures as design intent to confirm against the
> firmware repo.

Related: [03-Hardware-Architecture](03-Hardware-Architecture.md),
[05-Communication-Protocols](05-Communication-Protocols.md),
[07-Safety-Systems](07-Safety-Systems.md).

## Dual-Core Design

The RP2040 has two Cortex-M0+ cores. The firmware splits work to keep hard real-time
control isolated from host communication:

- **Core 0 - Control / safety loop**: samples sensors (IMU, USS, hall, rain, power),
  evaluates the emergency conditions, computes/forwards motor setpoints to the xESC
  channels, and enforces the low-level emergency latch. This loop must remain
  deterministic; it is the authority for SR-1/SR-2 in
  [02-System-Requirements](02-System-Requirements.md).
- **Core 1 - Communication / service loop**: handles the host link to the Raspberry
  Pi (v1 serial COBS framing, or v2 `xbot_framework` service bus), GPS passthrough,
  and configuration exchange. It never bypasses the Core 0 safety authority.

Shared state between cores is exchanged through lock-protected / inter-core FIFO
mechanisms so that a stall in communication cannot block the control loop.

## Main Loop Structure

```
setup()                       // init buses, sensors, xESC links, set INITIALIZED bit
loop (Core 0):                // fixed-rate control cycle
    read sensors (IMU, USS, hall, rain, power)
    evaluate emergency conditions -> update emergency_bitmask (latch)
    if emergency or host-timeout: zero all motor setpoints
    apply motor setpoints to xESC channels
    publish ll_status / ll_imu upward

loop (Core 1):                // communication cycle
    receive host packets (heartbeat, config, cmd) -> decode + CRC check
    apply high-level requests (emergency set/clear, duty cycles, config)
    pump GPS / service traffic
    emit status/service responses
```

The `INITIALIZED` bit in `ll_status.status_bitmask` (bit 0) signals that `setup()`
succeeded; the ROS layer treats all other bits as meaningless until it is set.

## Packet Handling Pipeline (v1)

The v1 host link is a byte stream framed with COBS and protected by a CRC. Each
packet begins with a one-byte type ID. The ROS mirror of the dispatch table
(`mower_comms_v1/src/ll_datatypes.h`) defines the packet contract:

```c
#define PACKET_ID_LL_STATUS                1
#define PACKET_ID_LL_IMU                   2
#define PACKET_ID_LL_UI_EVENT              3
#define PACKET_ID_LL_HIGH_LEVEL_CONFIG_REQ 0x11
#define PACKET_ID_LL_HIGH_LEVEL_CONFIG_RSP 0x12
#define PACKET_ID_LL_HEARTBEAT             0x42
#define PACKET_ID_LL_HIGH_LEVEL_STATE      0x43
```

Pipeline per received packet: `COBS decode -> verify CRC -> switch(type) ->
copy into the matching packed struct -> act`. Outbound packets follow the reverse:
`fill struct -> append CRC -> COBS encode -> write`.

## Module / Service Integration Pattern (v2)

v2 replaces the flat packet switch with a **service bus** (`xbot_framework`). Each
functional unit on the firmware is a *service* identified by a numeric service ID
(e.g. `EMERGENCY`, `DIFF_DRIVE`, `MOWER`, `IMU`, `POWER`, `BMS`, `GPS`, `INPUT`,
`HIGH_LEVEL`). The ROS side binds one `*ServiceInterface` per service. This is the
pattern a WiFi module follows.

Each service exposes:
- **Inputs** the host can set (e.g. twist, emergency add/clear mask, config).
- **Outputs** the host subscribes to via `On<Field>Changed(...)` callbacks.
- **Lifecycle** callbacks: `OnServiceConnected`, `OnServiceDisconnected`,
  `OnConfigurationRequested`, `OnTransactionStart/End`.

Example from the ROS-side GPS service binding (`GpsServiceInterface.h`):

```cpp
class GpsServiceInterface : public GpsServiceInterfaceBase {
  bool OnConfigurationRequested(uint16_t service_id) override;
  void OnPositionChanged(const double* new_value, uint32_t length) override;
  void OnFixTypeChanged(const char* new_value, uint32_t length) override;
  // ...service outputs surfaced as ROS publishers
};
```

## How a New Module (WiFi) Plugs In

> `[MODIFIED]` The generic pattern below remains the reference for any **MCU-side**
> feature. For the WiFi remote start/stop feature (FR-9) it is **NOT exercised** —
> see "WiFi Feature: No Firmware Change" below. Keep this section as the template for
> future firmware-resident subsystems.

1. **Firmware side**: define a new service with a unique service ID, declare its
   fields (inputs it accepts, outputs it reports) and implement its connect/config
   lifecycle on Core 1. It must not take the control-loop authority from Core 0.
2. **Config**: add any persistent WiFi options into the forward-compatible
   `ll_high_level_config` struct (it reserves `reserved_for_future_use1..5` option
   bits and is explicitly documented as growable without breaking compatibility).
3. **ROS side**: add a matching `WifiServiceInterface` subclass in `mower_comms_v2`
   (mirroring the existing `*ServiceInterface` classes) that is instantiated and
   `Start()`ed in `mower_comms.cpp`.

See [05-Communication-Protocols](05-Communication-Protocols.md) for the concrete
wiring of a new service/channel end-to-end.

## WiFi Feature: No Firmware Change `[NEW]`

The WiFi remote start/stop feature (FR-9.1–FR-9.6) is implemented entirely in the ROS
Software layer by the `mower_wifi` node. It reaches the mower through existing ROS
service/topic interfaces that already resolve down to the firmware via
`mower_comms_*`. Therefore:

- **No new firmware service or `PACKET_ID_*`** is added for FR-9.
- **Core 0 (control/safety) and Core 1 (comms)** are unchanged; the firmware
  heartbeat watchdog (SR-2) and the `emergency_bitmask` latch (SR-1) are untouched.
- A remote emergency (FR-9.4) arrives as a normal high-level emergency request
  (`EmergencyStopSrv` → `ll/_service/emergency`), which the firmware already honors
  by zeroing motor outputs. The **firmware remains the final safety authority** — a
  WiFi command cannot override a latched low-level emergency (SR-7).

If a future requirement demands WiFi connectivity exposed *through* the firmware
(e.g. an ESP companion module), the generic plug-in pattern above applies; that is
out of scope for FR-9.

## Low-Level Emergency Authority

Regardless of version, the firmware is the final safety authority: it owns the
`emergency_bitmask` latch and will zero motor outputs on lift/tilt/stop/collision or
on loss of the host heartbeat, independent of what the host requests. Any WiFi-driven
command still passes through this authority - it cannot override a latched emergency.
Details in [07-Safety-Systems](07-Safety-Systems.md).
