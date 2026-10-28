# eCall TCU — Baseline v1

Production-style embedded Linux eCall TCU software. Detects a crash or manual
SOS, builds a Minimum Set of Data (MSD) from live GNSS, sends it to a PSAP,
and drives the call state machine.

> **Baseline scope:** location is **live GNSS only** — there is intentionally
> **no GNSS-loss fallback** in v1. When no fresh fix is available the MSD
> position is reported as `NOT_AVAILABLE`, never `0,0`. All data is synthetic.

See the spec and traceability matrix in the top-level `../docs/`:
`ecall_spec.md`, `msd_layout.md`, `traceability.md`.

## Layout

```
ecall-tcu/
├── CMakeLists.txt
├── ap/
│   ├── libs/
│   │   ├── common/     status enum + monotonic clock
│   │   ├── gnss_if/    NMEA 0183 GGA/RMC over UDP 10110
│   │   ├── can_if/     vcan0 crash frame (0x0A0) + UDP 10111 SOS
│   │   ├── ecall_core/ IDLE->TRIGGERED->MSD_SENT->VOICE_CONNECTED->CLOSED
│   │   ├── msd/        binary MSD encoder (<= 140 bytes)
│   │   └── psap_if/    UDP MSD transport, 3 attempts
│   ├── services/ecalld/  epoll daemon, config, syslog, systemd unit
│   └── tests/            Unity tests (run via ctest)
├── third_party/unity/    vendored minimal Unity harness
└── tools/                gnss_sim.py, crash_inject.sh
```

## Build

```sh
cmake -S . -B build
cmake --build build
```

Compiles with `-Wall -Wextra -Werror -pedantic` (C99).

## Test

```sh
ctest --test-dir build --output-on-failure
```

Test names embed requirement IDs (e.g. `test_ECALL_MSD_05_*`).

## Static analysis

```sh
cppcheck --addon=misra --std=c99 \
  -I ap/libs/common -I ap/libs/gnss_if -I ap/libs/can_if \
  -I ap/libs/ecall_core -I ap/libs/msd -I ap/libs/psap_if \
  ap/libs ap/services
```

## Run (local, synthetic)

Terminal 1 — start the daemon (logs to syslog/journal):

```sh
./build/ecalld ap/services/ecalld/ecalld.conf
```

Terminal 2 — feed synthetic GNSS:

```sh
python3 tools/gnss_sim.py --rate 1.0
# simulate GNSS loss at a crash:
python3 tools/gnss_sim.py --cut-at-impact 20
```

Terminal 3 — inject a trigger:

```sh
./tools/crash_inject.sh manual     # UDP "SOS" -> 127.0.0.1:10111
./tools/crash_inject.sh auto       # CAN 0x0A0#01.. on vcan0
```

For the CAN path, bring up a virtual CAN interface first:

```sh
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set up vcan0
```

## Configuration

`ap/services/ecalld/ecalld.conf` (`key = value`, `#` comments). Keys:
`gnss_port`, `sos_port`, `can_ifname`, `psap_host`, `psap_port`, `vin`,
`passengers`, `test_call`. Missing file falls back to built-in defaults.
