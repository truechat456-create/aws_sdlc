# MSD Binary Layout (baseline v1)

The Minimum Set of Data (MSD) is a fixed, big-endian binary record emitted by
the TCU to the PSAP. Total size is **44 bytes used**, well within the
**140-byte** cap mandated by the steering rules. All multi-byte integers are
**big-endian (network order)**. All data is synthetic.

## Field table

| Offset | Size | Type   | Field              | Notes                                             |
|-------:|-----:|--------|--------------------|---------------------------------------------------|
| 0      | 1    | u8     | version            | = 1 (`MSD_VERSION`)                               |
| 1      | 4    | u32 BE | message_id         | increments per eCall session                      |
| 5      | 1    | u8     | activation         | 1 = AUTOMATIC (crash), 2 = MANUAL (SOS)           |
| 6      | 1    | u8     | flags              | bit0 = test_call, bit1 = position_trusted         |
| 7      | 17   | ascii  | VIN                | space-padded, not NUL-terminated on the wire      |
| 24     | 8    | u64 BE | timestamp_utc      | seconds since Unix epoch                          |
| 32     | 4    | i32 BE | latitude_mas       | milliarcseconds, or 0x7FFFFFFF = NOT_AVAILABLE    |
| 36     | 4    | i32 BE | longitude_mas      | milliarcseconds, or 0x7FFFFFFF = NOT_AVAILABLE    |
| 40     | 2    | u16 BE | direction_deg      | travel direction, 0..359                          |
| 42     | 1    | u8     | passengers         | occupant count (best estimate)                    |
| 43     | 1    | u8     | reserved           | 0                                                 |

Total used: **44 bytes** (offsets 0..43).

## Coordinate encoding

Coordinates are stored in **milliarcseconds** (mas):

```
1 degree = 3 600 000 mas
mas = round(degrees * 3_600_000)
```

Latitude is positive north, longitude positive east. Conversion is performed
by `msd_deg_to_mas()`.

## Position policy (v1 — live GNSS only)

- A position is **trusted** only when the newest live GNSS fix is **<= 2 s**
  old (`MSD_MAX_FIX_AGE_MS = 2000`). In that case lat/lon carry the fix and
  `position_trusted` (flags bit1) is set.
- Otherwise (no fix, or a fix older than 2 s) both lat and lon are written as
  `0x7FFFFFFF` (**NOT_AVAILABLE**) and `position_trusted` is cleared.
- **Baseline v1 has no fallback.** It never substitutes a last-known or
  estimated position for a missing live fix, and it never emits `0,0`.

## Flags byte (offset 6)

| Bit | Mask | Meaning            |
|----:|------|--------------------|
| 0   | 0x01 | test_call          |
| 1   | 0x02 | position_trusted   |
| 2-7 | --   | reserved (0)       |
