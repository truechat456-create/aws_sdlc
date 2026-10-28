# Requirements Document

## Introduction

This feature adds a bounded, clearly-marked GNSS-loss fallback to the eCall TCU
onboard software. In the baseline (v1), the TCU uses live GNSS only: when no
fresh fix (at most 2 s old) is available, the MSD position is emitted as
`NOT_AVAILABLE`. Field analysis of automatic eCall sessions shows that in severe
rollover and tunnel-exit crashes the live GNSS fix is frequently lost or not yet
reacquired at the instant of the trigger, so the PSAP receives a valid voice
call with no position — even though a valid fix existed seconds before impact.

This is a specification gap, not a software defect: the baseline behaves exactly
as specified. These requirements introduce a last-known-position fallback that
reuses the most recent valid GNSS fix, within a configurable maximum age, when
no fresh live fix is available. The fallback is explicitly marked (the
position-trusted flag is cleared) and bounded (beyond the configured age the
position remains `NOT_AVAILABLE`). A live fix is still preferred, and empty or
`0,0` coordinates are never transmitted.

## Trigger

The originating, approved change case for this feature is documented in
`ecall-mcp/docs/change_case.md`.

- **PO-2026-041** — public-opinion signal, a safety-critical social cluster
  (37 clustered posts over 6 weeks, rising trend): "eCall connects but operator
  has no location." Highest-priority change signal (selection order:
  safety > volume > trend).
- **JIRA-ECALL-1182** — corroborating Jira defect report, "eCall received
  without coordinates", safety-critical / high.
- **Classification:** P2 — requirement gap. The software meets the existing
  requirements (baseline ECALL-05 / ECALL-08); the gap is that the specification
  provides no bounded last-known-position fallback for the GNSS-loss scenario.

**Supporting hypothesis (H2):** no GNSS fix available at tunnel exit / signal
reacquisition lag. All missing-location events are rollover or tunnel-exit
crashes where the live fix is lost or not yet reacquired at the trigger moment
(`location_api_response = NO_FIX`), while a valid fix existed 5–60 s earlier.
Competing hypotheses (hardware damage, backend stripping the position, customer
misunderstanding) were rejected.

**Field evidence:** 9 of 40 events arrived with a missing MSD position. Every
one is a severe rollover or tunnel-exit crash with `NO_FIX` at trigger time, a
recent last-known valid fix available (5–60 s before impact) with a continuous
breadcrumb trail, and a backend payload byte-identical to the vehicle payload
(the position was already `NOT_AVAILABLE` on the wire).

## Scope

- **In scope:** the TCU onboard software only. These requirements introduce a
  bounded, clearly-marked GNSS-loss fallback for the MSD position.
- **112 regulated eCall flow:** UNCHANGED. The regulated emergency-call flow is
  not altered by this feature.
- **Offboard console:** UNCHANGED behaviorally. The offboard console only
  displays data; there are no behavioral changes offboard.
- **Coexistence with the baseline:** these requirements do NOT replace baseline
  ECALL-05 / ECALL-08 until approved. They coexist with the baseline policy — a
  live fix at most 2 s old is still preferred, the absence of any usable
  position is still signalled explicitly as `NOT_AVAILABLE`, and empty or `0,0`
  coordinates are never transmitted.

## Glossary

- **TCU**: Telematics Control Unit. The embedded Linux device whose onboard
  software detects a crash or manual SOS, assembles the MSD, and transmits it to
  the PSAP.
- **MSD**: Minimum Set of Data. The fixed, big-endian binary record (at most 140
  bytes) emitted by the TCU to the PSAP, carrying activation type, VIN,
  timestamp, position and related fields.
- **PSAP**: Public Safety Answering Point. The emergency call center that
  receives the MSD and the voice call.
- **GNSS**: Global Navigation Satellite System. The source of position fixes,
  delivered to the TCU as NMEA 0183 GGA/RMC sentences over UDP 10110.
- **GNSS fix**: a position report (latitude, longitude) with a receive timestamp
  and validity derived from GNSS input.
- **Fix age / freshness**: the elapsed time between a GNSS fix's receive
  timestamp and the current time. A fix is "fresh" when its age is at most the
  2 s freshness window.
- **2 s freshness window**: the baseline threshold for a live fix
  (`MSD_MAX_FIX_AGE_MS = 2000`). A fix at most 2 s old is a live, trusted fix.
- **Last-known position**: the most recent valid GNSS fix, reused as the MSD
  position when no fresh live fix is available, provided the fix is within the
  configurable maximum last-known-position age.
- **Maximum last-known-position age**: a configurable upper bound on the age of
  a fix that may be reused as a last-known position. Read from the TCU
  configuration file, with a safe default when absent or invalid.
- **Position-trusted flag**: flags bit1 of the MSD. Set when the MSD position
  comes from a live fix at most 2 s old; cleared when a last-known position is
  used.
- **NOT_AVAILABLE**: the explicit representation of an absent usable position.
  Encoded in the MSD as the sentinel `0x7FFFFFFF` in milliarcseconds for both
  latitude and longitude; never emitted as empty or `0,0` coordinates.

## Requirements

### Requirement 1: Last-known-position fallback (ECALL-LOC-01)

**User Story:** As a PSAP operator responding to a severe crash, I want the TCU to fall back to the most recent valid GNSS fix when no fresh live fix exists, so that I receive a usable crash location even when GNSS is lost at impact.

#### Acceptance Criteria

1. WHILE no live GNSS fix that is both valid and no older than 2000 ms is available, AND the most recent valid GNSS fix is within the configurable maximum last-known-position age, THE TCU SHALL use that most recent valid GNSS fix as the last-known position for the MSD latitude and longitude.
2. WHEN the TCU uses a last-known position for the MSD, THE TCU SHALL clear the position-trusted flag (flags bit1).
3. IF no valid GNSS fix exists within the configurable maximum last-known-position age, THEN THE TCU SHALL set the MSD latitude and longitude to the NOT_AVAILABLE sentinel (0x7FFFFFFF milliarcseconds) and clear the position-trusted flag (flags bit1).
4. WHERE the configurable maximum last-known-position age is not overridden by valid configuration, THE TCU SHALL apply a default of 3600000 ms, accepting configured values within the inclusive range 2000 ms to 86400000 ms.

### Requirement 2: Mark last-known position as untrusted (ECALL-LOC-02)

**User Story:** As a PSAP operator, I want a last-known position to be clearly marked as not current, so that I can distinguish a live fix from an older fallback position.

#### Acceptance Criteria

1. WHEN a last-known (fallback) position is used as the position in the MSD, THE TCU SHALL set the position-trusted flag (flags bit1) to 0 while populating the latitude and longitude fields with the last-known coordinate values.
2. WHEN a live GNSS fix no older than 2000 ms is used as the position in the MSD, THE TCU SHALL set the position-trusted flag (flags bit1) to 1.
3. IF no position (neither a live fix no older than 2000 ms nor a last-known position) is available at MSD assembly, THEN THE TCU SHALL set the position-trusted flag (flags bit1) to 0 and set the latitude and longitude fields to the NOT_AVAILABLE value.

### Requirement 3: Record last-known fix age (ECALL-LOC-03)

**User Story:** As a PSAP operator and as an engineer analyzing events, I want the age of a last-known fix recorded, so that I can judge how recent the fallback position is.

#### Acceptance Criteria

1. WHEN a last-known position supplies the fallback position for a session, THE TCU SHALL record, in the syslog event log before MSD transmission, the age of that last-known fix as a non-negative integer number of seconds.
2. WHEN the TCU records the last-known fix age, THE TCU SHALL compute the age as the elapsed time from the fix receive timestamp to the point of recording, rounded to the nearest whole second, and clamped to a maximum of 86400 seconds.
3. IF a last-known position is used but its fix receive timestamp is unavailable, THEN THE TCU SHALL record a last-known fix age entry in the syslog event log indicating that the age is unknown, and SHALL proceed with transmission.

### Requirement 4: Bound the fallback by maximum age (ECALL-LOC-04)

**User Story:** As a safety engineer, I want the fallback bounded by a maximum age, so that the TCU never reports a stale position as the crash location.

#### Acceptance Criteria

1. WHILE the age of the most recent valid GNSS fix is less than or equal to the configurable maximum last-known-position age (in milliseconds), THE TCU SHALL be permitted to use that fix as the last-known position for the MSD.
2. IF the age of the most recent valid GNSS fix is strictly greater than the configurable maximum last-known-position age, THEN THE TCU SHALL set the MSD latitude and longitude to the NOT_AVAILABLE sentinel (0x7FFFFFFF milliarcseconds).
3. WHEN the TCU sets the MSD position to NOT_AVAILABLE because the most recent valid fix is older than the maximum last-known-position age, THE TCU SHALL clear the position-trusted flag (flags bit1).
4. WHERE the configurable maximum last-known-position age is provided, THE TCU SHALL interpret it in milliseconds within the inclusive range 0 ms to 86400000 ms.

### Requirement 5: Never transmit empty or 0,0 coordinates (ECALL-LOC-05)

**User Story:** As a PSAP operator, I want absence of a position signalled explicitly, so that I am never misled by a bogus `0,0` location.

#### Acceptance Criteria

1. IF no usable position is available, THEN THE TCU SHALL set both the latitude and longitude fields of the MSD to the `NOT_AVAILABLE` sentinel value (`0x7FFFFFFF` milliarcseconds).
2. WHEN the TCU transmits an MSD, THE TCU SHALL populate both the latitude and longitude fields with a value that is either a measured coordinate in milliarcseconds or the `NOT_AVAILABLE` sentinel, and SHALL NOT leave either field unset.
3. WHEN the TCU transmits an MSD whose position is not `NOT_AVAILABLE`, THE TCU SHALL set both the latitude and longitude fields to the measured coordinates and SHALL NOT transmit a latitude of 0 milliarcseconds combined with a longitude of 0 milliarcseconds.
4. WHEN a usable position is available, THE TCU SHALL set the MSD latitude and longitude fields to the coordinates of that position rather than to the `NOT_AVAILABLE` sentinel.

### Requirement 6: Log last-known-position substitution (ECALL-LOC-06)

**User Story:** As an engineer auditing eCall events, I want each fallback substitution logged, so that I can trace when and where a last-known position was used.

#### Acceptance Criteria

1. WHEN the TCU substitutes a last-known position, THE TCU SHALL write a syslog entry within 1 second of the substitution that identifies the entry as a last-known-position substitution event and includes a timestamp of when the substitution occurred.
2. WHEN the TCU writes the substitution syslog entry, THE TCU SHALL include the fix age expressed in whole seconds, representing the elapsed time between the last valid position fix and the substitution event.
3. WHEN the TCU writes the substitution syslog entry, THE TCU SHALL include the substituted latitude and longitude coordinates in decimal degrees with at least 5 decimal places of precision.
4. IF the TCU cannot write the substitution syslog entry, THEN THE TCU SHALL retry the write up to 3 times and, if all attempts fail, record a logging-failure indication while proceeding with the eCall without blocking.

### Requirement 7: Configurable maximum last-known-position age with safe default (ECALL-LOC-07)

**User Story:** As an integrator, I want the maximum last-known-position age to be configurable with a safe default, so that the fallback bound can be tuned and remains safe when configuration is missing or invalid.

#### Acceptance Criteria

1. WHEN ecalld starts, THE TCU SHALL read the maximum last-known-position age, expressed in whole milliseconds, from the `key = value` configuration file.
2. IF the configuration file is absent or cannot be opened, THEN THE TCU SHALL apply the built-in default maximum last-known-position age of 60000 ms.
3. IF the maximum last-known-position age key is missing, non-numeric, zero, or outside the accepted range of 2000 ms to 600000 ms inclusive, THEN THE TCU SHALL reject the configured value and apply the built-in default maximum last-known-position age of 60000 ms.
4. WHILE ecalld is running after startup, THE TCU SHALL use a single effective maximum last-known-position age that is either the accepted configured value or the built-in default of 60000 ms.
5. WHEN the TCU applies the built-in default because the configured maximum last-known-position age was absent, unreadable, or rejected, THE TCU SHALL emit a syslog event indicating that the default maximum last-known-position age was applied.

### Requirement 8: Prefer a live fix over a last-known position (ECALL-LOC-08)

**User Story:** As a PSAP operator, I want a current live fix used whenever one is available, so that I receive the most accurate position and the fallback is used only when needed.

#### Acceptance Criteria

1. WHEN an eCall session is triggered AND a live GNSS fix whose age is at most 2000 ms is available, THE TCU SHALL use that live fix for the MSD latitude and longitude and set the position-trusted flag (flags bit1), in preference to any last-known position.
2. WHILE a live GNSS fix whose age is at most 2000 ms is available, THE TCU SHALL NOT use a last-known position for the MSD latitude and longitude, regardless of whether a last-known position exists.
3. WHEN an eCall session is triggered AND no live GNSS fix whose age is at most 2000 ms is available AND a last-known position is available, THE TCU SHALL use the last-known position for the MSD latitude and longitude as the fallback.
4. IF the TCU uses a last-known position for the MSD latitude and longitude, THEN THE TCU SHALL clear the position-trusted flag (flags bit1).

### Requirement 9: Retain the most recent valid fix across a crash trigger (ECALL-LOC-09)

**User Story:** As a PSAP operator, I want the last valid fix preserved through the crash trigger, so that a last-known position is still available when live GNSS reception is lost at impact.

#### Acceptance Criteria

1. WHEN a crash trigger occurs, THE TCU SHALL preserve the most recent valid GNSS fix and its receive timestamp, held unchanged from their values immediately before the trigger, for retrieval throughout the resulting session until that session reaches the CLOSED state.
2. IF live GNSS reception is lost at or after the crash trigger, THEN THE TCU SHALL continue to make the preserved most recent valid GNSS fix and its receive timestamp available for the duration of the session, without overwriting them with any later invalid or absent fix.
3. IF no valid GNSS fix exists at the time of the crash trigger, THEN THE TCU SHALL report the retained position as NOT_AVAILABLE and SHALL NOT substitute any fabricated or fallback position.
