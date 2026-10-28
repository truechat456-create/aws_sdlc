/*
 * gnss_if.h - GNSS interface. Parses NMEA 0183 GGA/RMC sentences received
 * over UDP port 10110 and maintains the latest fix with a receive timestamp
 * and validity flag.
 *
 * Baseline v1: live GNSS only. No fallback, no history.
 */
#ifndef GNSS_IF_H
#define GNSS_IF_H

#include "ecall_status.h"

#define GNSS_DEFAULT_UDP_PORT   (10110)

/* A single GNSS fix. Coordinates are decimal degrees; WGS-84. */
typedef struct
{
    bool     valid;        /* true once a valid fix has been parsed        */
    double   lat_deg;      /* latitude,  positive north                    */
    double   lon_deg;      /* longitude, positive east                     */
    double   course_deg;   /* course over ground, degrees true (RMC)       */
    double   speed_knots;  /* speed over ground, knots (RMC)               */
    uint8_t  satellites;   /* satellites in use (GGA)                      */
    uint64_t rx_mono_ms;   /* monotonic ms when this fix was received      */
} gnss_fix_t;

/* GNSS context. Fixed size, no dynamic allocation. */
typedef struct
{
    int        fd;         /* UDP socket, -1 when closed                   */
    gnss_fix_t latest;     /* most recent valid fix                        */
} gnss_if_t;

/* Initialise the context (does not open a socket). */
ecall_status_t gnss_if_init(gnss_if_t *gnss);

/* Open the UDP listener on the given port (use GNSS_DEFAULT_UDP_PORT). */
ecall_status_t gnss_if_open(gnss_if_t *gnss, uint16_t udp_port);

/* Return the socket fd for epoll registration. */
ecall_status_t gnss_if_fd(const gnss_if_t *gnss, int *out_fd);

/*
 * Read all pending datagrams and update the latest fix.
 * Returns ECALL_OK if at least one datagram was processed,
 * ECALL_ERR_NODATA if the socket had nothing to read.
 */
ecall_status_t gnss_if_poll(gnss_if_t *gnss);

/*
 * Parse a single NMEA sentence (GGA or RMC). Exposed for unit tests.
 * On a valid fix sentence, updates *fix (sets fields it can derive).
 * rx_mono_ms is stamped into the fix.
 */
ecall_status_t gnss_if_parse_sentence(const char *line,
                                      uint64_t    rx_mono_ms,
                                      gnss_fix_t *fix);

/* Copy out the latest fix. ECALL_ERR_NODATA if none yet. */
ecall_status_t gnss_if_get_latest(const gnss_if_t *gnss, gnss_fix_t *out_fix);

/* Close the socket. */
ecall_status_t gnss_if_close(gnss_if_t *gnss);

#endif /* GNSS_IF_H */
