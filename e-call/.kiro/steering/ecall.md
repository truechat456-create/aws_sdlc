# eCall Project Steering

## Languages & boundaries
- The TCU is embedded Linux C code, written like production OEM TCUs. No Python or ROS in TCU code.
- Python is allowed only in `tools/`, `offboard/`, `ecall-mcp/`, and `agent/`.

## C coding standards
- C99, MISRA C:2012 style.
- No `malloc`/`free` after initialization.
- No recursion.
- Fixed-size buffers only.
- Every function returns a status enum, and callers must check it.

## Build & analysis
- Build: CMake with `-Wall -Wextra -Werror -pedantic`.
- Static analysis: `cppcheck --addon=misra`.

## Tests
- Unity, run via `ctest`.
- Test names include requirement IDs (e.g. `test_ECALL_LOC_05_*`).

## Inputs
- GNSS: NMEA 0183 over UDP port `10110`.
- Crash: CAN frame on SocketCAN `vcan0` (ID `0x0A0`, byte0 `0x01` = airbag deployed).
- Manual SOS: UDP port `10111`.

## MSD (Minimum Set of Data)
- Binary format, `<= 140` bytes.
- Layout defined in `docs/msd_layout.md`.

## Safety
- Never send empty or `0,0` coordinates.
- Every position must carry `source`, `age_s`, and `confidence`.
- A fallback position is never presented as current.
- Age `> 120 s` means `LOW` confidence.
- Freeze position history at impact.

## Documentation
- Keep `docs/traceability.md` updated.
- All data is synthetic.
