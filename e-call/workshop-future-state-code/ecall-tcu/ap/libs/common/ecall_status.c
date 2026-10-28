/*
 * ecall_status.c - Status strings and monotonic time helper.
 */
#include "ecall_status.h"

#include <time.h>

const char *ecall_status_str(ecall_status_t status)
{
    const char *str;

    switch (status)
    {
        case ECALL_OK:              str = "OK";              break;
        case ECALL_ERR_PARAM:       str = "ERR_PARAM";       break;
        case ECALL_ERR_STATE:       str = "ERR_STATE";       break;
        case ECALL_ERR_IO:          str = "ERR_IO";          break;
        case ECALL_ERR_PARSE:       str = "ERR_PARSE";       break;
        case ECALL_ERR_NODATA:      str = "ERR_NODATA";      break;
        case ECALL_ERR_OVERFLOW:    str = "ERR_OVERFLOW";    break;
        case ECALL_ERR_TIMEOUT:     str = "ERR_TIMEOUT";     break;
        case ECALL_ERR_UNSUPPORTED: str = "ERR_UNSUPPORTED"; break;
        case ECALL_ERR_INTERNAL:    str = "ERR_INTERNAL";    break;
        default:                    str = "ERR_UNKNOWN";     break;
    }

    return str;
}

ecall_status_t ecall_monotonic_ms(uint64_t *out_ms)
{
    struct timespec ts;

    if (out_ms == NULL)
    {
        return ECALL_ERR_PARAM;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        return ECALL_ERR_IO;
    }

    *out_ms = ((uint64_t)ts.tv_sec * 1000u) +
              ((uint64_t)ts.tv_nsec / 1000000u);

    return ECALL_OK;
}
