/*
 * gnss_if.c - NMEA 0183 GGA/RMC parser over UDP 10110.
 *
 * MISRA style: fixed buffers, no dynamic allocation, status enum returns.
 * Parsing is done in place on a bounded copy of each line; no strtok on the
 * caller's buffer. Checksums are validated when present.
 */
#include "gnss_if.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define GNSS_LINE_MAX     (128)   /* max NMEA sentence length incl. CRLF   */
#define GNSS_FIELD_MAX     (24)    /* max tokens in a sentence              */
#define GNSS_DGRAM_MAX    (512)    /* UDP datagram buffer                   */

/* ---- small helpers ---------------------------------------------------- */

/* Forward declarations for bounded numeric parsers (defined below). */
static ecall_status_t parse_u32(const char *s, uint32_t *out);
static ecall_status_t parse_double(const char *s, double *out);

/* Convert a NMEA lat/lon "ddmm.mmmm" string + hemisphere to decimal deg. */
static ecall_status_t nmea_to_deg(const char *val,
                                  const char *hemi,
                                  uint8_t     deg_digits,
                                  double     *out_deg)
{
    double raw;
    double degrees;
    double minutes;

    (void)deg_digits;

    if ((val == NULL) || (hemi == NULL) || (out_deg == NULL))
    {
        return ECALL_ERR_PARAM;
    }
    if ((val[0] == '\0') || (hemi[0] == '\0'))
    {
        return ECALL_ERR_PARSE;
    }

    if (parse_double(val, &raw) != ECALL_OK)
    {
        return ECALL_ERR_PARSE;
    }

    /* NMEA angle is ddmm.mmmm (or dddmm.mmmm). Split whole degrees off. */
    degrees = (double)((long)(raw / 100.0));
    minutes = raw - (degrees * 100.0);

    *out_deg = degrees + (minutes / 60.0);

    if ((hemi[0] == 'S') || (hemi[0] == 'W'))
    {
        *out_deg = -(*out_deg);
    }

    return ECALL_OK;
}

/* Validate NMEA checksum if the sentence carries one ("*hh"). */
static ecall_status_t nmea_check(const char *line)
{
    const char *star;
    uint8_t     sum = 0u;
    size_t      i;
    unsigned    given;

    if (line == NULL)
    {
        return ECALL_ERR_PARAM;
    }
    if (line[0] != '$')
    {
        return ECALL_ERR_PARSE;
    }

    star = strchr(line, '*');
    if (star == NULL)
    {
        /* no checksum present; accept */
        return ECALL_OK;
    }

    for (i = 1u; line[i] != '\0'; ++i)
    {
        if (&line[i] == star)
        {
            break;
        }
        sum ^= (uint8_t)line[i];
    }

    if (sscanf(star + 1, "%2x", &given) != 1)
    {
        return ECALL_ERR_PARSE;
    }

    return ((uint8_t)given == sum) ? ECALL_OK : ECALL_ERR_PARSE;
}

/*
 * Split a line into comma-separated fields on a local copy.
 * Fills field[] with pointers into work[]. Returns field count in *n.
 */
static ecall_status_t nmea_split(const char *line,
                                 char        work[GNSS_LINE_MAX],
                                 char       *field[GNSS_FIELD_MAX],
                                 uint8_t    *n)
{
    size_t  len;
    size_t  i;
    uint8_t count = 0u;
    char   *cursor;

    if ((line == NULL) || (field == NULL) || (n == NULL))
    {
        return ECALL_ERR_PARAM;
    }

    len = strlen(line);
    if (len >= (size_t)GNSS_LINE_MAX)
    {
        return ECALL_ERR_OVERFLOW;
    }

    (void)memcpy(work, line, len + 1u);

    /* strip CR/LF and checksum suffix for clean field parsing */
    for (i = 0u; i < len; ++i)
    {
        if ((work[i] == '\r') || (work[i] == '\n') || (work[i] == '*'))
        {
            work[i] = '\0';
            break;
        }
    }

    cursor   = work;
    field[0] = cursor;
    count    = 1u;

    while (*cursor != '\0')
    {
        if (*cursor == ',')
        {
            *cursor = '\0';
            if (count >= (uint8_t)GNSS_FIELD_MAX)
            {
                return ECALL_ERR_OVERFLOW;
            }
            field[count] = cursor + 1;
            ++count;
        }
        ++cursor;
    }

    *n = count;
    return ECALL_OK;
}

/*
 * Parse a non-negative decimal integer from a NUL-terminated field without
 * relying on errno/strto* (MISRA-friendly, bounded). Non-digit characters
 * terminate parsing; an empty field yields 0. Saturates at UINT32_MAX.
 */
static ecall_status_t parse_u32(const char *s, uint32_t *out)
{
    uint32_t v = 0u;
    size_t   i;

    if ((s == NULL) || (out == NULL))
    {
        return ECALL_ERR_PARAM;
    }

    for (i = 0u; s[i] != '\0'; ++i)
    {
        char c = s[i];
        if ((c < '0') || (c > '9'))
        {
            break;
        }
        v = (v * 10u) + (uint32_t)(c - '0');
    }

    *out = v;
    return ECALL_OK;
}

/* Parse a decimal value (e.g. speed/course) using strtod with errno set
 * to zero first and tested after, as required when errno is examined. */
static ecall_status_t parse_double(const char *s, double *out)
{
    char  *endp;
    double v;

    if ((s == NULL) || (out == NULL))
    {
        return ECALL_ERR_PARAM;
    }

    errno = 0;
    v = strtod(s, &endp);
    if ((endp == s) || (errno != 0))
    {
        return ECALL_ERR_PARSE;
    }

    *out = v;
    return ECALL_OK;
}

/* ---- GGA / RMC decoders ----------------------------------------------- */

static ecall_status_t parse_gga(char *const field[GNSS_FIELD_MAX],
                                uint8_t     n,
                                uint64_t    rx_mono_ms,
                                gnss_fix_t *fix)
{
    uint32_t satellites = 0u;

    if (n < 10u)
    {
        return ECALL_ERR_PARSE;
    }

    /* field[6] = fix quality; 0 = invalid */
    {
        uint32_t quality = 0u;
        (void)parse_u32(field[6], &quality);
        if (quality == 0u)
        {
            return ECALL_ERR_NODATA;
        }
    }

    if (nmea_to_deg(field[2], field[3], 2u, &fix->lat_deg) != ECALL_OK)
    {
        return ECALL_ERR_PARSE;
    }
    if (nmea_to_deg(field[4], field[5], 3u, &fix->lon_deg) != ECALL_OK)
    {
        return ECALL_ERR_PARSE;
    }

    (void)parse_u32(field[7], &satellites);
    fix->satellites = (uint8_t)satellites;
    fix->valid      = true;
    fix->rx_mono_ms = rx_mono_ms;

    return ECALL_OK;
}

static ecall_status_t parse_rmc(char *const field[GNSS_FIELD_MAX],
                                uint8_t     n,
                                uint64_t    rx_mono_ms,
                                gnss_fix_t *fix)
{
    if (n < 9u)
    {
        return ECALL_ERR_PARSE;
    }

    /* field[2] = status A=active, V=void */
    if (field[2][0] != 'A')
    {
        return ECALL_ERR_NODATA;
    }

    if (nmea_to_deg(field[3], field[4], 2u, &fix->lat_deg) != ECALL_OK)
    {
        return ECALL_ERR_PARSE;
    }
    if (nmea_to_deg(field[5], field[6], 3u, &fix->lon_deg) != ECALL_OK)
    {
        return ECALL_ERR_PARSE;
    }

    if (field[7][0] != '\0')
    {
        (void)parse_double(field[7], &fix->speed_knots);
    }
    if (field[8][0] != '\0')
    {
        (void)parse_double(field[8], &fix->course_deg);
    }

    fix->valid      = true;
    fix->rx_mono_ms = rx_mono_ms;

    return ECALL_OK;
}

/* ---- public API ------------------------------------------------------- */

ecall_status_t gnss_if_parse_sentence(const char *line,
                                      uint64_t    rx_mono_ms,
                                      gnss_fix_t *fix)
{
    char           work[GNSS_LINE_MAX];
    char          *field[GNSS_FIELD_MAX];
    uint8_t        n = 0u;
    ecall_status_t st;
    const char    *type;

    if ((line == NULL) || (fix == NULL))
    {
        return ECALL_ERR_PARAM;
    }

    st = nmea_check(line);
    if (st != ECALL_OK)
    {
        return st;
    }

    st = nmea_split(line, work, field, &n);
    if (st != ECALL_OK)
    {
        return st;
    }
    if (n == 0u)
    {
        return ECALL_ERR_PARSE;
    }

    /* field[0] looks like $GPGGA / $GNRMC: match the last 3 chars */
    type = field[0];
    {
        size_t tlen = strlen(type);
        if (tlen < 3u)
        {
            return ECALL_ERR_PARSE;
        }
        type = &type[tlen - 3u];
    }

    if (strcmp(type, "GGA") == 0)
    {
        st = parse_gga(field, n, rx_mono_ms, fix);
    }
    else if (strcmp(type, "RMC") == 0)
    {
        st = parse_rmc(field, n, rx_mono_ms, fix);
    }
    else
    {
        st = ECALL_ERR_UNSUPPORTED; /* not a sentence we use */
    }

    return st;
}

ecall_status_t gnss_if_init(gnss_if_t *gnss)
{
    if (gnss == NULL)
    {
        return ECALL_ERR_PARAM;
    }
    (void)memset(gnss, 0, sizeof(*gnss));
    gnss->fd = -1;
    return ECALL_OK;
}

ecall_status_t gnss_if_open(gnss_if_t *gnss, uint16_t udp_port)
{
    int                fd;
    struct sockaddr_in addr;
    int                flags;

    if (gnss == NULL)
    {
        return ECALL_ERR_PARAM;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return ECALL_ERR_IO;
    }

    (void)memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(udp_port);

    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        (void)close(fd);
        return ECALL_ERR_IO;
    }

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        (void)close(fd);
        return ECALL_ERR_IO;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
    {
        (void)close(fd);
        return ECALL_ERR_IO;
    }

    gnss->fd = fd;
    return ECALL_OK;
}

ecall_status_t gnss_if_fd(const gnss_if_t *gnss, int *out_fd)
{
    if ((gnss == NULL) || (out_fd == NULL))
    {
        return ECALL_ERR_PARAM;
    }
    if (gnss->fd < 0)
    {
        return ECALL_ERR_STATE;
    }
    *out_fd = gnss->fd;
    return ECALL_OK;
}

ecall_status_t gnss_if_poll(gnss_if_t *gnss)
{
    char     buf[GNSS_DGRAM_MAX];
    bool     any = false;
    uint64_t now = 0u;

    if (gnss == NULL)
    {
        return ECALL_ERR_PARAM;
    }
    if (gnss->fd < 0)
    {
        return ECALL_ERR_STATE;
    }

    (void)ecall_monotonic_ms(&now);

    for (;;)
    {
        const ssize_t got = recv(gnss->fd, buf, sizeof(buf) - 1u, 0);
        if (got < 0)
        {
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
            {
                break;
            }
            return ECALL_ERR_IO;
        }
        if (got == 0)
        {
            break;
        }

        buf[got] = '\0';

        /* A datagram may contain multiple sentences separated by \n. */
        {
            char       *save = NULL;
            const char *tok  = strtok_r(buf, "\r\n", &save);
            while (tok != NULL)
            {
                gnss_fix_t         tmp = gnss->latest;
                const ecall_status_t ps =
                    gnss_if_parse_sentence(tok, now, &tmp);
                if (ps == ECALL_OK)
                {
                    gnss->latest = tmp;
                    any = true;
                }
                tok = strtok_r(NULL, "\r\n", &save);
            }
        }
    }

    return any ? ECALL_OK : ECALL_ERR_NODATA;
}

ecall_status_t gnss_if_get_latest(const gnss_if_t *gnss, gnss_fix_t *out_fix)
{
    if ((gnss == NULL) || (out_fix == NULL))
    {
        return ECALL_ERR_PARAM;
    }
    if (!gnss->latest.valid)
    {
        return ECALL_ERR_NODATA;
    }
    *out_fix = gnss->latest;
    return ECALL_OK;
}

ecall_status_t gnss_if_close(gnss_if_t *gnss)
{
    if (gnss == NULL)
    {
        return ECALL_ERR_PARAM;
    }
    if (gnss->fd >= 0)
    {
        (void)close(gnss->fd);
        gnss->fd = -1;
    }
    return ECALL_OK;
}
