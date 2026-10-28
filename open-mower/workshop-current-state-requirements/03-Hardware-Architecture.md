# 03 - Hardware Architecture

Baseline hardware context for the OpenMower mainboard, focused on the interfaces and
expansion points relevant to adding a WiFi subsystem.

> Scope note: the hardware PCB design repositories (`hw-openmower-universal`,
> `hw-openmower-yardforce`, `hw-openmower-sabo`) and the firmware repository
> (`fw-openmower-v2`) are included as git submodules and are not checked out in this
> workspace. The details below are reconstructed from the project README and from the
> firmware-facing data structures and parameters in `open_mower_ros`
> (`mower_comms_v1/src/ll_datatypes.h`, `mower_comms_v2`, and the hardware-specific
> param files). Pin-level numbers should be confirmed against the board schematic
> before relying on them for the WiFi build.

Related: [01-System-Architecture](01-System-Architecture.md),
[04-Firmware-Architecture](04-Firmware-Architecture.md).

## Mainboard Components

The OpenMower mainboard replaces the stock mower brain and integrates:

- **RP2040 microcontroller** - main low-level controller (the "LL board").
- **Three motor controller channels (xESC)** - left drive, right drive, mow motor,
  each a VESC-style controller running `xesc_firmware`.
- **IMU** - 9-DOF: acceleration, gyro and magnetometer (`ll_imu` carries
  `acceleration_mss[3]`, `gyro_rads[3]`, `mag_uT[3]`).
- **Ultrasonic sensors (USS)** - up to 5 range channels (`uss_ranges_m[5]`).
- **Power / charging front-end** - system voltage, charge voltage, charge current
  sensing and a controllable charger (`v_system`, `v_charge`, `charging_current`).
- **Rain sensor** - reported via the status bitmask.
- **Hall sensors** - up to `MAX_HALL_INPUTS = 10` inputs (4 on OM board + 6 on the
  stock CoverUI), configurable as lift/tilt or stop.
- **Sound module (DFPlayer)** - optional, 5V/3V3 selectable (`dfp_is_5v`).
- **CoverUI board** - optional stock user interface, reported by `ui_board_available`.

## Microcontroller: RP2040 (Dual-Core)

| Property        | Value / Note                                              |
|-----------------|-----------------------------------------------------------|
| Cores           | 2 x ARM Cortex-M0+ (dual-core)                            |
| Clock           | 133 MHz typical                                           |
| SRAM            | 264 KB                                                    |
| Flash           | External QSPI (board dependent)                          |
| PIO             | 2 x programmable I/O blocks (8 state machines)           |
| Peripherals     | 2x UART, 2x SPI, 2x I2C, PWM, ADC, USB                    |

The dual-core design and how firmware uses it is covered in
[04-Firmware-Architecture](04-Firmware-Architecture.md).

## Communication Buses

| Bus   | Typical use on the mainboard                                       |
|-------|--------------------------------------------------------------------|
| UART  | xESC links; host link to the Raspberry Pi (v1 serial COBS); GPS    |
| SPI   | IMU and/or high-rate sensor front-ends                             |
| I2C   | Low-rate peripherals (sensors, power-management ICs, CoverUI)      |
| PWM   | Motor duty-cycle outputs via the xESC channels                     |
| USB   | RP2040 programming / debug                                         |
| GPIO  | Hall inputs, rain sensor, power-rail enables, status lines         |

### Host Link (Firmware <-> Raspberry Pi)
- **v1**: UART serial, COBS-framed packets with CRC (see
  [05-Communication-Protocols](05-Communication-Protocols.md)).
- **v2**: the firmware exposes an IP-based service bus (`xbot_framework`), bound by
  the ROS node to `bind_ip` (default `0.0.0.0`); GPS, baud rate and port index are
  passed as service parameters (`services/gps/baud_rate`, `services/gps/port_index`).

## Power Rails and Enables

The firmware controls and reports several rails, surfaced in `ll_status.status_bitmask`:

| Bit | Rail / signal        | ROS field (`Status.msg`)     |
|-----|----------------------|------------------------------|
| 0   | Initialized          | `mower_status`               |
| 1   | Raspberry Pi power   | `raspberry_pi_power`         |
| 2   | Charging             | `is_charging`                |
| 3   | ESC power            | `esc_power`                  |
| 4   | Rain detected        | `rain_detected`              |
| 5   | Sound available      | `sound_module_available`     |
| 6   | Sound busy           | `sound_module_busy`          |
| 7   | UI board available   | `ui_board_available`         |

Charger limits are configurable from ROS (`v_charge_cutoff`, `i_charge_cutoff`,
`v_battery_cutoff`) via the `ll_high_level_config` packet.

## Available Resources for Expansion (WiFi)

For a WiFi subsystem, the relevant expansion surfaces are:

- **Host-side (preferred)**: the Raspberry Pi already has networking; a WiFi feature
  implemented as a ROS node needs no mainboard changes. This is the lowest-risk path.
- **MCU-side**: if WiFi connectivity must be exposed through the firmware, a
  companion WiFi module would attach via a spare **UART** or **SPI** bus and be
  modeled as a new firmware module / service (see
  [04-Firmware-Architecture](04-Firmware-Architecture.md)) rather than by overloading
  the host link.
- **GPIO**: spare GPIO lines (not consumed by the hall/USS/power set above) are
  available for module enable/reset/interrupt signaling; confirm free pins against
  the board schematic before assignment.

The `ll_high_level_config` struct is explicitly designed as a flexible-length,
forward-compatible structure with `reserved_for_future_use1..5` option bits, giving a
sanctioned place to add configuration flags for new hardware without breaking packet
compatibility.
