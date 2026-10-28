/*
 * ecall_status.h - Shared status codes and common types for the eCall TCU.
 *
 * Baseline v1. Follows MISRA C:2012 style: every function returns a status
 * enum, callers check it. No dynamic allocation after init. C99.
 */
#ifndef ECALL_STATUS_H
#define ECALL_STATUS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Canonical status enum returned by (nearly) every function in the TCU. */
typedef enum
{
    ECALL_OK = 0,            /* success                                      */
    ECALL_ERR_PARAM,         /* invalid argument (NULL, out of range)        */
    ECALL_ERR_STATE,         /* operation not valid in current state         */
    ECALL_ERR_IO,            /* socket / file I/O failure                    */
    ECALL_ERR_PARSE,         /* malformed input (NMEA, CAN, SOS)             */
    ECALL_ERR_NODATA,        /* no data available yet (would block)          */
    ECALL_ERR_OVERFLOW,      /* buffer / size limit exceeded                 */
    ECALL_ERR_TIMEOUT,       /* retries exhausted / timed out                */
    ECALL_ERR_UNSUPPORTED,   /* feature not supported in this baseline       */
    ECALL_ERR_INTERNAL       /* unexpected internal error                    */
} ecall_status_t;

/* Convert a status code to a short constant string for logging. */
const char *ecall_status_str(ecall_status_t status);

/*
 * Monotonic time helper. Returns ECALL_OK and writes milliseconds since an
 * unspecified but monotonic epoch. Used for fix age and retry timing.
 */
ecall_status_t ecall_monotonic_ms(uint64_t *out_ms);

#endif /* ECALL_STATUS_H */
