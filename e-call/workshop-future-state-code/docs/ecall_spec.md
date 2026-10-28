# eCall TCU Specification — Baseline v1

Status: **v1 (existing production baseline)**
Scope: Embedded Linux TCU software that detects a crash or manual SOS,
assembles a Minimum Set of Data (MSD), and transmits it to a PSAP, then
places the call. All data is synthetic.

> **v1 location policy:** location is **live GNSS only**. There is no
> GNSS-loss fallback in this baseline. When no fresh fix is available the MSD
> position is `NOT_AVAILABLE`.

## Inputs

- **GNSS**: NMEA 0183 (GGA/RMC) over **UDP 10110**.
- **Crash**: SocketCAN frame on **vcan0**, ID **0x0A0**, byte0 **0x01** =
  airbag deployed → AUTOMATIC trigger.
- **Manual SOS**: ASCII `"SOS"` over **UDP 10111** → MANUAL trigger.

## Output

- Binary MSD (<= 140 bytes) to a configurable PSAP `host:port` over UDP,
  with up to 3 send attempts.

## Requirements

| ID       | Requirement |
|----------|-------------|
| ECALL-01 | The TCU shall detect an AUTOMATIC trigger from CAN frame ID `0x0A0` when byte0 == `0x01`, and a MANUAL trigger from the ASCII payload `"SOS"` on UDP 10111. |
| ECALL-02 | The TCU shall parse NMEA 0183 GGA and RMC sentences from UDP 10110 and maintain the latest valid fix with its receive timestamp and validity. |
| ECALL-03 | The TCU shall implement the session state machine IDLE → TRIGGERED → MSD_SENT → VOICE_CONNECTED → CLOSED, and shall ignore duplicate triggers while a session is active. |
| ECALL-04 | The TCU shall encode the MSD as a binary record of at most 140 bytes containing version, message id, activation type, test flag, VIN, timestamp, latitude/longitude in milliarcseconds, position-trusted flag, direction and passenger count. |
| ECALL-05 | The TCU shall set the MSD position from a live GNSS fix only when that fix is at most 2 seconds old; otherwise the position shall be `NOT_AVAILABLE` and the position-trusted flag shall be clear. No fallback position shall be used. |
| ECALL-06 | The TCU shall transmit the MSD to the configured PSAP over UDP and shall retry on failure for a total of up to 3 attempts. |
| ECALL-07 | The TCU service (`ecalld`) shall run as a single-threaded epoll event loop, read its configuration from a file, and log events to syslog, deployable as a systemd unit. |
| ECALL-08 | The TCU shall never transmit empty or `0,0` coordinates; absence of a trusted position shall be represented explicitly as `NOT_AVAILABLE`. |

## Non-goals (v1)

- No GNSS-loss fallback / dead-reckoning / last-known-position substitution.
- No voice-codec implementation (voice path is modelled as a state only).
- No PSAP acknowledgement protocol (fire-and-retry over UDP).

## Build, analysis, test

- Build: CMake, `-Wall -Wextra -Werror -pedantic`.
- Static analysis: `cppcheck --addon=misra`.
- Tests: Unity via `ctest`; test names embed requirement IDs.
