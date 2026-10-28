# Implementation Plan: GNSS-Loss Fallback

## Overview

This plan extends the eCall TCU baseline (`workshop-future-state-code/ecall-tcu`)
with a bounded, clearly-marked GNSS-loss fallback. Work proceeds bottom-up:
the new `pos_history` ring buffer first, then the additive MSD encoder
(`msd_build_ex` / `msd_encode_ex`), the config bound (`max_lkp_age_ms`), the
confidence decision + dead-reckoning, the daemon wiring in `ecalld.c`, and
finally property-based tests, integration/regression validation, and docs.

**Hard constraint (every task preserves this):** the full v1 suite —
`test_can`, `test_core`, `test_gnss`, `test_msd` (`test_ECALL_MSD_01..08`),
`test_psap` — MUST stay green. The baseline MSD region (offsets 0..43) stays
byte-for-byte, and `msd_fill_position()` / `msd_encode()` keep their v1
signatures and behavior. All new code is additive, MISRA C:2012 style, fixed
buffers, no dynamic allocation, every function returns `ecall_status_t`.

Design source of truth: `.kiro/specs/gnss-loss-fallback/design.md`.
Requirements source of truth: `.kiro/specs/gnss-loss-fallback/requirements.md`.

**Project conventions:**
- After each task, the full ctest suite is run (via the PostTaskExec hook /
  `ctest --test-dir build --output-on-failure`); test names embed requirement
  IDs (`test_ECALL_LOC_*`, `test_ECALL_POS_*`, `test_ECALL_MSD_*`), so a passing
  run is the per-task verification signal. There is no separate executable task
  for running ctest — it is automated by the hook.
- `docs/traceability.md` is updated incrementally as each module/function lands:
  `pos_history` after task 1, `msd_build_ex`/`msd_encode_ex` after task 2, config
  after task 4, `decide_position`/dead-reckoning after task 5, daemon wiring +
  `log_lkp_substitution` after task 6. Task 10.1 is the final reconciliation pass,
  not the first time traceability is touched.

## Tasks

- [ ] 1. Create the `pos_history` ring-buffer module
  - [ ] 1.1 Implement `pos_history.h` and `pos_history.c`
    - Create `ap/libs/pos_history/pos_history.h`: `POS_HISTORY_CAPACITY (16u)`, `pos_history_t { gnss_fix_t entries[16]; uint8_t head; uint8_t count; bool frozen; }`, including `ecall_status.h` and `gnss_if.h` (for `gnss_fix_t`).
    - Create `ap/libs/pos_history/pos_history.c` implementing `pos_history_init` (clears buffer, count=0, head=0, frozen=false; also the unfreeze/reset entry point), `pos_history_push` (write at `head`, advance `head=(head+1)%CAPACITY`, saturate `count` at CAPACITY/overwrite-oldest; NULL -> `ECALL_ERR_PARAM`; frozen -> `ECALL_ERR_STATE`), `pos_history_freeze` (NULL -> `ERR_PARAM`; already frozen -> `ERR_STATE`; else sets `frozen=true`), `pos_history_get_latest` (most-recent entry; empty -> `ECALL_ERR_NODATA`), `pos_history_last_valid` (newest entry with `valid==true`, scan newest->oldest; none -> `ERR_NODATA`), `pos_history_count`.
    - MISRA style: no dynamic allocation, fixed `gnss_fix_t` array, every function returns `ecall_status_t`, callers check it.
    - Update `docs/traceability.md` row(s) for this component (ECALL-LOC-09).
    - _Design: Components and Interfaces §1; Data Models (`pos_history_t`); D2, D5_
    - _Requirements: 9.1, 9.2, 9.3 (ECALL-LOC-09)_

  - [ ] 1.2 Register `pos_history` as a CMake library target
    - Add an `add_library(pos_history STATIC ap/libs/pos_history/pos_history.c)` block to `ecall-tcu/CMakeLists.txt`, matching the existing `gnss_if` pattern: `target_include_directories(pos_history PUBLIC ap/libs/pos_history)` and `target_link_libraries(pos_history PUBLIC ecall_common gnss_if)` (needs `gnss_fix_t`).
    - _Design: Architecture (Modules touched table)_
    - _Requirements: 9.1 (ECALL-LOC-09)_

  - [ ]* 1.3 Write unit tests for `pos_history` (`ap/tests/test_pos.c`)
    - `test_ECALL_POS_01_init_empty` — fresh buffer: count 0, `get_latest`/`last_valid` -> `ERR_NODATA`.
    - `test_ECALL_POS_02_push_latest` — push N, `get_latest` returns the newest.
    - `test_ECALL_POS_03_overwrite_oldest` — push > 16, `count` saturates at 16, oldest no longer retrievable, no `ERR_OVERFLOW`. **Establishes Property 8.**
    - `test_ECALL_POS_04_last_valid_scan` — invalid entries skipped, newest valid returned. **Establishes Property 10.**
    - `test_ECALL_POS_05_freeze_rejects_push` — push after freeze -> `ERR_STATE`, buffer byte-unchanged. **Establishes Property 9.**
    - `test_ECALL_POS_06_refreeze_err_state` — second freeze -> `ERR_STATE`. **Establishes Property 9.**
    - `test_ECALL_POS_07_count_accessor` — count tracks pushes and saturation.
    - `test_ECALL_POS_08_null_params` — NULL args -> `ERR_PARAM`.
    - `test_ECALL_POS_09_retention_across_trigger` — valid fix survives later invalid pushes + freeze, `rx_mono_ms` unchanged. **Establishes Property 7.**
    - Register with ctest via `ecall_add_test(test_pos pos_history)`.
    - _Design: Testing Strategy (new unit tests — pos_history); Properties 7, 8, 9, 10_
    - _Requirements: 9.1, 9.2 (ECALL-LOC-09)_

- [ ] 2. Extend the MSD encoder additively (baseline bytes preserved)
  - [ ] 2.1 Add extension types and constants to `msd.h`
    - Add `MSD_LKP_LOW_CONF_AGE_MS (120000u)`, `MSD_DEFAULT_MAX_LKP_AGE_MS (60000u)`, `MSD_RECENT_MAX (8u)`.
    - Add `msd_confidence_t { MSD_CONF_PRECISE=0, MSD_CONF_ESTIMATED=1, MSD_CONF_LOW=2, MSD_CONF_LOCATION_UNAVAILABLE=3 }`.
    - Add `msd_recent_delta_t { int16_t lat_delta_mas; int16_t lon_delta_mas; }`, `msd_oem_block_t { uint16_t last_fix_age_s; uint8_t confidence; int32_t dr_lat_mas; int32_t dr_lon_mas; uint16_t search_radius_m; uint8_t breadcrumb_count; }`, and `msd_ext_t { uint8_t recent_count; msd_recent_delta_t recent[MSD_RECENT_MAX]; msd_oem_block_t oem; }`.
    - Leave `msd_input_t`, `msd_fill_position`, `msd_encode`, `msd_deg_to_mas` unchanged.
    - _Design: Components and Interfaces §2 (msd.h additions); Data Models (confidence enum); D4, D7_
    - _Requirements: 1.1, 2.1, 2.2, 4.1 (ECALL-LOC-01, -02, -04)_

  - [ ] 2.2 Implement `msd_build_ex()` in `msd.c`
    - Populate `base` (baseline fields incl. lat/lon/position_trusted) and `ext` (deltas + OEM block) from a confidence decision: PRECISE -> live fix, `position_trusted=true`; ESTIMATED/LOW -> last-valid fix, `position_trusted=false`; LOCATION_UNAVAILABLE -> `0x7FFFFFFF` sentinel, `position_trusted=false`.
    - Enforce `base->position_trusted = (conf == MSD_CONF_PRECISE)` — never set for other tiers.
    - Compute each breadcrumb delta as `recent[i]` minus the primary coordinate, clamped to the `int16` range; set `oem.breadcrumb_count == ext->recent_count`.
    - Fill OEM fields: `last_fix_age_s` (0..86400, clamped), `confidence`, `dr_lat_mas`/`dr_lon_mas` (from caller), `search_radius_m` (clamped), `oem_breadcrumb_count` mirror.
    - NULL `base` / out-of-range coordinate -> `ECALL_ERR_PARAM` (same as baseline via `msd_deg_to_mas`).
    - Update `docs/traceability.md` row(s) for this component (ECALL-LOC-01, -02, -04, -05, -08).
    - _Design: Components and Interfaces §2 (`msd_build_ex`); Error Handling (`msd_build_ex` parameter errors); D3, D7_
    - _Requirements: 1.1, 1.2, 1.3, 2.1, 2.2, 2.3, 4.2, 4.3, 5.1, 5.4, 8.4 (ECALL-LOC-01, -02, -04, -05, -08)_

  - [ ] 2.3 Implement `msd_encode_ex()` in `msd.c` with v1 equivalence
    - Emit baseline region (offsets 0..43) byte-identical to `msd_encode()`, then append the recent-location TLV (`rl_type=0x52`, `rl_len=1+4*breadcrumb_count`, `breadcrumb_count`, then N × (i16 BE lat delta, i16 BE lon delta)) and the OEM TLV (`oem_type=0x4F`, `oem_len=12`, `last_fix_age_s` u16 BE, `confidence` u8, `dr_lat_mas` i32 BE, `dr_lon_mas` i32 BE, `search_radius_m` u16 BE, `oem_breadcrumb_count` u8), all big-endian.
    - Runtime overflow guard: compute total length, return `ECALL_ERR_OVERFLOW` without writing past `out_cap` if it would exceed `out_cap` or `MSD_MAX_BYTES` (140).
    - `ext == NULL` encodes only the baseline 44-byte record (v1-compatible path); make `msd_encode(base,...)` equivalent to `msd_encode_ex(base, NULL, ...)` (either delegate or share a helper) so `test_ECALL_MSD_01..08` stay green.
    - Update `docs/traceability.md` row(s) for this component (ECALL-LOC-05).
    - _Design: Components and Interfaces §2 (`msd_encode_ex`); Data Models (MSD byte layout + byte-budget proof); Error Handling (oversize guard); D3, D4_
    - _Requirements: 5.1, 5.2, 5.3 (ECALL-LOC-05)_

  - [ ]* 2.4 Write unit + boundary tests for the extended encoder (new `test_ECALL_MSD_09..`, do NOT alter `test_ECALL_MSD_01..08`)
    - Baseline bytes 0..43 byte-identical to `msd_encode()` for the same `base`. **Establishes Property 12.**
    - Worst case (8 breadcrumbs) encodes to 95 bytes and `*out_len <= 140`; overflow guard returns `ECALL_ERR_OVERFLOW`. **Establishes Property 12.**
    - Encoded `breadcrumb_count`, OEM `oem_breadcrumb_count` mirror, and number of 4-byte delta records all equal `ext->recent_count`; `rl_len == 1 + 4 * recent_count`. **Establishes Property 13.**
    - Delta clamping to `int16` for breadcrumbs beyond ±32767 mas.
    - Confidence->flag rule: `position_trusted` true iff `conf == PRECISE`.
    - _Design: Testing Strategy (new unit tests — MSD/decision `test_ECALL_LOC_11..13`); Properties 12, 13_
    - _Requirements: 5.2 (ECALL-LOC-05); baseline ECALL-04 preserved_

- [ ] 3. Checkpoint — modules build and existing + new unit tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [ ] 4. Extend ecalld configuration with the max-LKP-age bound
  - [ ] 4.1 Add config fields, constants, defaults, and parse branch
    - In `ecalld_config.h`: add `ECALLD_MAX_LKP_AGE_DEFAULT_MS (60000u)`, `ECALLD_MAX_LKP_AGE_MIN_MS (2000u)`, `ECALLD_MAX_LKP_AGE_MAX_MS (600000u)`; add `uint32_t max_lkp_age_ms;` and `bool lkp_age_defaulted;` to `ecalld_config_t`.
    - In `ecalld_config.c`: `ecalld_config_defaults()` sets `max_lkp_age_ms = ECALLD_MAX_LKP_AGE_DEFAULT_MS` and `lkp_age_defaulted = true`; add a `max_lkp_age_ms` branch to `ecalld_config_parse_line()` using `cfg_u32` that accepts a value only when `MIN <= v <= MAX` (sets value and clears `lkp_age_defaulted`), else keeps the default and leaves `lkp_age_defaulted = true` (covers zero/non-numeric via `cfg_u32` returning 0).
    - Update `docs/traceability.md` row(s) for this component (ECALL-LOC-07).
    - _Design: Components and Interfaces §4 (config); Data Models (`ecalld_config_t` new field); Error Handling (config invalid/missing/unreadable)_
    - _Requirements: 7.1, 7.2, 7.3, 7.4 (ECALL-LOC-07)_

  - [ ] 4.2 Document the `max_lkp_age_ms` key in `ecalld.conf`
    - Add a commented, documented `max_lkp_age_ms = ...` key to `ap/services/ecalld/ecalld.conf` (default 60000, range 2000..600000).
    - _Design: Components and Interfaces §4 (`ecalld.conf` gains a documented key)_
    - _Requirements: 7.1 (ECALL-LOC-07)_

  - [ ]* 4.3 Write config validation unit tests (`test_ECALL_LOC_07`)
    - Missing/absent file -> default 60000, `lkp_age_defaulted == true`; non-numeric/zero/out-of-range (`< 2000` or `> 600000`) -> default 60000, defaulted true; in-range value -> that value, defaulted false; effective value is a single `uint32_t` for the run. **Establishes Property 6.**
    - _Design: Testing Strategy (`test_ECALL_LOC_07`); Property 6_
    - _Requirements: 7.2, 7.3, 7.4 (ECALL-LOC-07)_

- [ ] 5. Implement the confidence decision and dead-reckoning
  - [ ] 5.1 Implement `decide_position()` (daemon-local helper in `ecalld.c`)
    - Pure function of the frozen history, `now_ms`, and `cfg.max_lkp_age_ms`. Call `pos_history_last_valid`; compute `age_ms = now_ms - rx_mono_ms` clamped `>= 0`. Tiers: live && `live_age_ms <= MSD_MAX_FIX_AGE_MS` (2000) -> PRECISE (primary=live, trusted=1); no live && last-valid `age_ms <= max_lkp_age_ms` -> ESTIMATED if `age_ms <= MSD_LKP_LOW_CONF_AGE_MS` (120000) else LOW (primary=last-valid, trusted=0, `used_lkp=true`); else -> LOCATION_UNAVAILABLE (no primary, trusted=0, sentinel `0x7FFFFFFF`, never `0,0`). Output `conf`, `primary`, `recent[]`, `recent_n`, `age_ms`, `used_lkp`.
    - Update `docs/traceability.md` row(s) for this component (ECALL-LOC-01, -04, -08).
    - _Design: Confidence / Position Decision Model (tiering + threshold reconciliation); Position-decision states table; D1, D7_
    - _Requirements: 1.1, 1.3, 4.1, 4.2, 8.1, 8.2, 8.3, 8.4 (ECALL-LOC-01, -04, -08)_

  - [ ] 5.2 Implement dead-reckoning and search-radius helpers
    - Straight-line extrapolation from the last valid fix along `course_deg` at `speed_knots` over elapsed age (`distance_m = speed_knots*0.514444*age_s`; flat-earth local metric; WGS-84) for `dr_lat_mas`/`dr_lon_mas`; monotonic non-decreasing `search_radius_m = clamp(BASE_RADIUS_M + speed_mps*age_s*K, 0, 65535)` with named constants `BASE_RADIUS_M`, `K`. At `age==0` or `speed==0`, DR position equals the last fix and radius equals the base floor. DR estimate feeds only the OEM block, never the baseline primary lat/lon.
    - _Design: Confidence / Position Decision Model (dead-reckoning method and assumptions); D6_
    - _Requirements: 3.2, 4.1 (ECALL-LOC-03, -04)_

  - [ ]* 5.3 Write decision + dead-reckoning unit/boundary tests (`test_ECALL_LOC_01,02,04,05,08,10,14`)
    - Tier boundaries: `age==2000` PRECISE, `age==120000` ESTIMATED, `age==120001` LOW (when max>=120001), `age==max_lkp_age_ms` last-known, `age==max_lkp_age_ms+1` LOCATION_UNAVAILABLE. **Establishes Properties 5, 11.**
    - Live preference over an available last-known fix. **Establishes Property 1.**
    - No fix / over-max -> `0x7FFFFFFF` sentinel, untrusted, never `0,0`. **Establishes Properties 3, 4.**
    - DR distance and `search_radius_m` monotonic non-decreasing in age and speed; base case at age/speed 0. **Establishes Property 14.**
    - _Design: Testing Strategy (`test_ECALL_LOC_01,02,04,05,08,10,14`); Properties 1, 3, 4, 5, 11, 14_
    - _Requirements: 1.1, 1.3, 2.1, 3.2, 4.1, 4.2, 5.1, 8.1, 8.3 (ECALL-LOC-01..05, -08)_

- [ ] 6. Wire the fallback into the daemon (`ecalld.c`)
  - [ ] 6.1 Add and initialize the position history in `app_t`
    - Add `pos_history_t hist;` to `app_t` (static storage, no heap); call `pos_history_init(&app.hist)` in `main()` during startup.
    - After `ecalld_config_load()` (syslog open), emit the default-applied event when `app.cfg.lkp_age_defaulted` is set (covers missing/unopenable/rejected config).
    - _Design: Components and Interfaces §3 (app_t field; config default syslog §4)_
    - _Requirements: 7.5, 9.1 (ECALL-LOC-07, -09)_

  - [ ] 6.2 Capture breadcrumbs on the GNSS branch
    - After a successful `gnss_if_poll(&app.gnss)` on the GNSS fd, read `gnss_if_get_latest(&app.gnss, &f)` and `(void)pos_history_push(&app.hist, &f)` (ignore `ERR_STATE` while frozen). Only valid fixes reach the ring because `gnss_if_poll` updates `latest` only on a valid parse.
    - _Design: Components and Interfaces §3 (a) Capture breadcrumbs_
    - _Requirements: 9.2 (ECALL-LOC-09)_

  - [ ] 6.3 Freeze the history at the trigger
    - In `handle_trigger()`, after a successful `ecall_core_on_trigger()` (and the duplicate-trigger `ERR_STATE` early return) and before `run_ecall_sequence()`, call `(void)pos_history_freeze(&app->hist)` for a stable impact snapshot.
    - _Design: Components and Interfaces §3 (b) Freeze at the trigger; D5_
    - _Requirements: 9.1 (ECALL-LOC-09)_

  - [ ] 6.4 Assemble and send the extended MSD in `run_ecall_sequence()`
    - Call `decide_position(&app->hist, now_ms, app->cfg.max_lkp_age_ms, ...)` over the frozen history, then `msd_build_ex(...)` (passing NULL primary for LOCATION_UNAVAILABLE) with `search_radius_from_age(...)` and direction from `primary.course_deg`, then `msd_encode_ex(...)`, then `psap_if_send_msd(...)` (up to 3 attempts). Retain the existing summary line (`position_trusted=%d` distinguishes live vs fallback on the wire).
    - On the close/reset path call `ecall_core_on_close` / `ecall_core_reset` then `pos_history_init(&app->hist)` to unfreeze for the next session.
    - Update `docs/traceability.md` row(s) for this component (ECALL-LOC-08, -09).
    - _Design: Components and Interfaces §3 (c) Decide confidence then unfreeze_
    - _Requirements: 8.1, 8.3, 9.1 (ECALL-LOC-08, -09)_

  - [ ] 6.5 Emit substitution and fix-age syslog logging
    - Implement `log_lkp_substitution(age_s, lat_deg, lon_deg)`: emit a last-known-position substitution entry before MSD transmission (same synchronous step, well under 1 s) identifying it as a substitution with a timestamp, the fix age in whole seconds (rounded to nearest, clamped to 86400; "age unknown" when `rx_mono_ms` cannot yield a valid age), and the substituted lat/lon in decimal degrees with >= 5 decimal places. Retry up to 3 times on write failure; on total failure record a single logging-failure indication and proceed without blocking (return `(void)`-ignored, never gates transmission).
    - Update `docs/traceability.md` row(s) for this component (ECALL-LOC-03, -06).
    - _Design: Components and Interfaces §3 (Substitution + fix-age logging); Error Handling (syslog write failure; unknown timestamp)_
    - _Requirements: 3.1, 3.2, 3.3, 6.1, 6.2, 6.3, 6.4 (ECALL-LOC-03, -06)_

- [ ] 7. Checkpoint — full daemon builds and unit tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [ ] 8. Property-based test suite
  - [ ]* 8.1 Add a PBT harness and wire it into ctest
    - Add a property-based testing harness for C (e.g. theft, or a Unity-driven random generator), not a hand-rolled fuzzer; register the PBT test executable(s) with ctest alongside the Unity tests.
    - _Design: Testing Strategy (Property-based tests)_
    - _Requirements: 1.1 (ECALL-LOC-01)_

  - [ ]* 8.2 Implement decision/encoder properties P1–P6, P11–P14 (>= 100 iterations each)
    - P1 trusted iff fresh live fix; P2 last-known in bounded band, untrusted; P3 out-of-bound/no fix -> NOT_AVAILABLE untrusted; P4 NOT_AVAILABLE never encoded as `0,0`; P5 monotonic threshold boundaries; P6 config collapses to safe default; P11 confidence tiering + 120 s boundary; P12 MSD size within budget + baseline bytes identical; P13 breadcrumb-count consistency; P14 DR position + radius monotonic. Each as one PBT test with a feature-tagged comment.
    - _Design: Correctness Properties (P1–P6, P11–P14); Testing Strategy_
    - _Requirements: 1.1, 1.2, 1.3, 2.1, 2.2, 2.3, 4.1, 4.2, 4.3, 5.1, 5.2, 5.3, 5.4, 7.2, 7.3, 7.4, 8.1, 8.3, 8.4 (ECALL-LOC-01, -02, -04, -05, -07, -08)_

  - [ ]* 8.3 Implement pos_history properties P7–P10 (>= 100 iterations each)
    - P7 latest valid fix retained across invalid input / trigger; P8 ring capacity + overwrite-oldest; P9 freeze immutability; P10 `last_valid` correctness. Each as one PBT test with a feature-tagged comment.
    - _Design: Correctness Properties (P7–P10); Testing Strategy_
    - _Requirements: 1.1, 9.1, 9.2 (ECALL-LOC-01, -09)_

- [ ] 9. Integration and regression validation
  - [ ] 9.1 Run the full ctest suite including all v1 regression tests
    - Build with `-Wall -Wextra -Werror -pedantic` and run the complete `ctest` suite; confirm `test_can`, `test_core`, `test_gnss`, `test_msd` (`test_ECALL_MSD_01..08`), `test_psap`, plus new `test_pos` / `test_ECALL_LOC_*` / `test_ECALL_MSD_09..` all pass (hard constraint: v1 stays green).
    - _Design: Testing Strategy (hard constraint first; Build & analysis)_
    - _Requirements: 5.2 (ECALL-LOC-05); baseline ECALL-04 preserved_

  - [ ]* 9.2 Add an automated integration scenario test driven by the simulator tools
    - Script `tools/gnss_sim.py --cut-at-impact` (build breadcrumbs, then cut reception) + `tools/crash_inject.sh` (AUTOMATIC trigger) into an automated check: impact case -> last-known position, `position_trusted=0`, populated recent-location + OEM block (confidence ESTIMATED/LOW, non-zero breadcrumb count), substitution syslog entry. Control case 1 (fresh GNSS, no cut) -> PRECISE, `position_trusted=1`. Control case 2 (cut long before trigger, `age_ms > max_lkp_age_ms`) -> LOCATION_UNAVAILABLE sentinel.
    - _Design: Testing Strategy (Integration scenario + control cases)_
    - _Requirements: 8.1, 8.3, 1.3, 4.2 (ECALL-LOC-08, -01, -04)_

  - [ ]* 9.3 Run static analysis
    - Run `cppcheck --addon=misra` over the new code and confirm the strict-warnings build is clean (fixed buffers, no dynamic allocation, `ecall_status_t` returns).
    - _Design: Testing Strategy (Build & analysis)_
    - _Requirements: 9.1 (ECALL-LOC-09)_

- [ ] 10. Update code-adjacent documentation and traceability
  - [ ] 10.1 Extend `docs/traceability.md`
    - Add `ECALL-LOC-01..09` rows mapping each requirement to its module/function (`pos_history`, `msd_build_ex`/`msd_encode_ex`, `decide_position`, `ecalld_config_*`, `log_lkp_substitution`) and its tests (`test_ECALL_LOC_*`, `test_ECALL_POS_*`, `test_ECALL_MSD_09+`), following the existing table style.
    - _Design: Requirement-to-Component Traceability_
    - _Requirements: 1.1, 2.1, 3.1, 4.1, 5.1, 6.1, 7.1, 8.1, 9.1 (ECALL-LOC-01..09)_

  - [ ] 10.2 Update `docs/msd_layout.md` and record any MISRA deviations
    - Append the recent-location TLV (`0x52`) and OEM TLV (`0x4F`) layout to `docs/msd_layout.md` with the 95-byte worst-case byte-budget proof; if `math.h` (dead-reckoning) or other deviations are introduced, record them in `docs/misra_deviations.md`.
    - _Design: Data Models (MSD byte layout + byte-budget proof); D3, D6_
    - _Requirements: 5.2 (ECALL-LOC-05)_

- [ ] 11. Final checkpoint — ensure all tests pass
  - Ensure all tests pass, ask the user if questions arise.

## Notes

- Tasks marked with `*` are optional and can be skipped for a faster MVP:
  property-based tests (8.1–8.3), the automated integration scenario (9.2),
  static analysis (9.3), and the per-module unit tests (1.3, 2.4, 4.3, 5.3).
  The core module/encoder/config/decision/daemon tasks and the v1 regression
  run (9.1) are required.
- Every requirement `ECALL-LOC-01..09` is covered: -01 (2.2, 5.1), -02 (2.2,
  5.1), -03 (5.2, 6.5), -04 (5.1, 2.2), -05 (2.2, 2.3), -06 (6.5), -07 (4.1,
  6.1), -08 (5.1, 6.4), -09 (1.1, 6.2, 6.3, 6.4).
- Hard constraint upheld throughout: all v1 tests (`test_can`, `test_core`,
  `test_gnss`, `test_msd`, `test_psap`) stay green; baseline MSD bytes 0..43 and
  `msd_fill_position()` / `msd_encode()` are unchanged (design D4).
- Per-property coverage: P1 (5.3, 8.2), P2 (8.2), P3 (5.3, 8.2), P4 (5.3, 8.2),
  P5 (5.3, 8.2), P6 (4.3, 8.2), P7 (1.3, 8.3), P8 (1.3, 8.3), P9 (1.3, 8.3),
  P10 (1.3, 8.3), P11 (5.3, 8.2), P12 (2.4, 8.2), P13 (2.4, 8.2), P14 (5.3, 8.2).
- Checkpoints (3, 7, 11) ensure incremental validation at module, daemon, and
  final boundaries.
- Verification is automated: after each task the full ctest suite runs (via the
  PostTaskExec hook / `ctest --test-dir build --output-on-failure`), and the
  requirement-tagged test names (`test_ECALL_LOC_*`, `test_ECALL_POS_*`,
  `test_ECALL_MSD_*`) make a passing run the per-task verification signal — there
  is no standalone "run ctest" task.
- Traceability is continuous: `docs/traceability.md` rows are added as each
  component lands (after tasks 1, 2, 4, 5, 6), and task 10.1 is the final
  reconciliation pass rather than the first traceability touch.

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1.1", "2.1", "4.2"] },
    { "id": 1, "tasks": ["1.2", "2.2", "4.1"] },
    { "id": 2, "tasks": ["1.3", "2.3", "4.3", "5.2"] },
    { "id": 3, "tasks": ["2.4", "5.1"] },
    { "id": 4, "tasks": ["5.3", "6.1"] },
    { "id": 5, "tasks": ["6.2", "6.3", "6.4"] },
    { "id": 6, "tasks": ["6.5", "8.1"] },
    { "id": 7, "tasks": ["8.2", "8.3", "9.1"] },
    { "id": 8, "tasks": ["9.2", "9.3", "10.1", "10.2"] }
  ]
}
```
