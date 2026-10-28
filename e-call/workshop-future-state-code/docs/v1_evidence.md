# eCall TCU v1 — Evidence: GNSS loss at impact → position NOT_AVAILABLE

This note captures a live end-to-end run of the **baseline v1** TCU
(`ecalld`) on the `workshop-future-state` instance, demonstrating the known
v1 gap: when GNSS is lost at the moment of a crash, the MSD reports
`NOT_AVAILABLE` rather than a stale or estimated position, and never `0,0`.
All data is synthetic.

## Setup

- Host: `workshop-future-state` (Ubuntu 22.04, kernel 6.8.0-1066-aws)
- `ecalld` running via systemd, PSAP target `127.0.0.1:5000`
- `vcan0` up for the CAN crash trigger
- Tools: `tools/gnss_sim.py` (NMEA route, `--cut-at-impact`),
  `tools/crash_inject.sh` (CAN/UDP trigger), `tools/msd_capture.py`
  (UDP MSD capture + decode per `docs/msd_layout.md`)

## Scenario (impact case)

1. Start an MSD listener on `127.0.0.1:5000`.
2. `gnss_sim.py --rate 5 --lat 48.1173 --lon 11.5167 --course 90 --speed 60
   --cut-at-impact 6` — drives a route, then **cuts GNSS at t=6 s** (impact).
3. Wait **4 s** so the last fix exceeds the 2 s freshness window
   (`MSD_MAX_FIX_AGE_MS = 2000`).
4. `crash_inject.sh auto` — CAN frame `0A0#0100000000000000` on `vcan0`
   (byte0 `0x01` = airbag deployed → AUTOMATIC).
5. Capture and decode the MSD.

### GNSS route (then cut)

```
gnss_sim -> udp 127.0.0.1:10110 @ 5.0 Hz (synthetic data)
GNSS will be cut after 6.0 s (impact)
t=   0.0s  48.11730,11.51670
t=   1.0s  48.11730,11.51692
t=   2.0s  48.11730,11.51715
t=   3.0s  48.11730,11.51737
t=   4.0s  48.11730,11.51760
t=   5.0s  48.11730,11.51782
--- GNSS cut (impact). No more sentences. ---
```

### Daemon log (syslog/journal)

```
ecalld[28477]: eCall AUTO: MSD id=3 bytes=44 position_trusted=0
ecalld[28477]: MSD sent in 1 attempt(s)
```

### Captured + decoded MSD

```
=== Decoded MSD ===
size_bytes        : 44
version           : 1
message_id        : 3
activation        : AUTOMATIC
test_call         : False
position_trusted  : False
vin               : SYNTHVIN000000001
timestamp_utc     : 1793071030
latitude_mas      : 2147483647
longitude_mas     : 2147483647
position          : NOT_AVAILABLE
direction_deg     : 90
passengers        : 1
raw_hex           : 0100000003010053594e544856494e303030303030303031000000006ae017b67fffffff7fffffff005a0100
```

**Result: `position = NOT_AVAILABLE`** — as expected.

Byte-level confirmation (big-endian, see `docs/msd_layout.md`):

- offset 6 (flags) = `0x00` → `position_trusted` bit clear, not a test call
- offsets 32–35 (latitude_mas)  = `7fffffff` = `0x7FFFFFFF` = NOT_AVAILABLE
- offsets 36–39 (longitude_mas) = `7fffffff` = `0x7FFFFFFF` = NOT_AVAILABLE
- Coordinates are **never** `0,0`; absence is signalled explicitly.

## Control case (trigger while GNSS is fresh)

To prove the `NOT_AVAILABLE` result is caused by fix staleness (not by GNSS
never reaching the daemon), the same flow was run **without** cutting GNSS and
with the trigger fired ~2 s in, while fixes were still arriving:

```
=== Decoded MSD ===
message_id        : 4
activation        : MANUAL
position_trusted  : True
latitude_mas      : 173222280
longitude_mas     : 41461572
position          : 48.117300, 11.517103
raw_hex           : 0100000004020253594e544856494e303030303030303031000000006ae018000a5329880278a744005a0100
```

Here `position_trusted = True` and the MSD carries the live route position
(`48.117300, 11.517103`), with flags byte `0x02` (position-trusted bit set).

## Conclusion

| Case                         | Fix freshness at trigger | `position_trusted` | MSD position            |
|------------------------------|--------------------------|--------------------|-------------------------|
| Impact (GNSS cut, +4 s)      | stale (> 2 s)            | False              | **NOT_AVAILABLE**       |
| Control (GNSS live)          | fresh (<= 2 s)           | True               | 48.117300, 11.517103    |

The baseline behaves per spec requirements **ECALL-05** and **ECALL-08**:
a position is reported only from a live fix at most 2 s old; otherwise the
MSD reports `NOT_AVAILABLE`, never a fallback and never `0,0`. The impact case
is precisely the v1 limitation that motivates future-state work: there is no
GNSS-loss fallback, so a crash that knocks out GNSS yields an MSD with no
position.
