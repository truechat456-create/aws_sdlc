# MISRA C:2012 Deviation Record — eCall TCU

Static analysis is run with `cppcheck --addon=misra`. Genuine defects are
fixed in source. The findings below are **documented deviations**: advisory
or required rules that either do not apply cleanly to a hosted POSIX
embedded-Linux service, or that conflict with a deliberate project-wide
convention. The machine-readable form lives in
`ecall-tcu/misra-suppressions.txt`.

## Fixed in code (not deviated)

| Rule | Location        | Fix |
|------|-----------------|-----|
| 10.7 | `msd.c` `put_u64` | Shift amount computed into an explicit `unsigned int` local to avoid an implicit conversion inside the composite shift expression. |
| 22.8, 22.9 | `gnss_if.c`, `ecalld_config.c` | Replaced errno-dependent `strtol`/`strtoul`/`strtod` integer parses with bounded, errno-free digit parsers (`parse_u32`, `cfg_u32`); coordinate parsing routed through a single `parse_double` that zeroes and tests `errno` correctly. |

## Deviations

| Rule(s)        | Category | Justification |
|----------------|----------|---------------|
| 15.5           | Convention | Guard-clause early returns. Every function validates arguments and returns a status enum at the top; this is the house style and is clearer than a single trailing return with nested state. |
| 15.4           | Convention | Socket drain loops use a single `break` on `EAGAIN` plus guarded `continue`s; behaviourally equivalent and readable. |
| 21.6           | POSIX | `stdio` is required: `syslog`, `snprintf`, and `fopen`/`fgets` config parsing. |
| 21.10          | POSIX | `<time.h>` (`time`, `clock_gettime`) is required for the MSD timestamp and GNSS fix-age computation. |
| 21.5           | POSIX | `<signal.h>` is required for `SIGINT`/`SIGTERM` clean daemon shutdown. |
| 22.8, 22.10    | POSIX | `errno` is set to 0 before `strtod`/`strtol` and tested after — the documented libc idiom. |
| 11.5           | POSIX | `void*` conversions are mandated by the BSD sockets API (`struct sockaddr*` casts) and the injectable `psap_send_fn_t` context pointer. |
| 8.7            | API | Functions flagged "could be static" are the public library API, consumed by the service and by unit tests in separate translation units. |
| 2.5            | API | Public headers export constants (ports, limits, sentinels) that not every translation unit references. |
| 18.4           | Advisory | Idiomatic C string scanning (`p + 1` past a delimiter); equivalent to array indexing. |
| 8.1, 8.6, 8.4  | Advisory | Explicit-type / linkage notes on POSIX prototypes and single-definition objects declared in their owning headers. |
| 13.3, 17.8, 9.3, 10.8 | Advisory | Localized reviewed idioms: loop counters with `++`, in-place trim of a *copied* buffer, designated struct initialization. |

## Review

All deviations are advisory or POSIX-mandated. No deviation affects the
safety-relevant logic (MSD position policy, never-`0,0`, state machine). All
data is synthetic.
