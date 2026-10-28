/*
 * ecalld_config.h - Configuration loading for the ecalld service.
 *
 * Simple "key = value" config file. Unknown keys are ignored. All keys are
 * optional; defaults are applied first.
 */
#ifndef ECALLD_CONFIG_H
#define ECALLD_CONFIG_H

#include "ecall_status.h"
#include "msd.h"   /* MSD_VIN_LEN */

#define ECALLD_PSAP_HOST_MAX   (64u)
#define ECALLD_IFNAME_MAX      (16u)

typedef struct
{
    uint16_t gnss_port;                      /* UDP NMEA   (10110)          */
    uint16_t sos_port;                       /* UDP SOS    (10111)          */
    char     can_ifname[ECALLD_IFNAME_MAX];  /* "vcan0"                     */
    char     psap_host[ECALLD_PSAP_HOST_MAX];/* PSAP IP                     */
    uint16_t psap_port;                      /* PSAP UDP port               */
    char     vin[MSD_VIN_LEN + 1u];          /* vehicle VIN (synthetic)     */
    uint8_t  passengers;                     /* default occupant count      */
    bool     test_call;                      /* mark MSDs as test           */
} ecalld_config_t;

/* Populate cfg with built-in defaults. */
ecall_status_t ecalld_config_defaults(ecalld_config_t *cfg);

/* Parse one "key = value" line into cfg. Blank/comment lines return OK. */
ecall_status_t ecalld_config_parse_line(ecalld_config_t *cfg,
                                        const char      *line);

/* Load a config file on top of defaults. Missing file -> defaults only. */
ecall_status_t ecalld_config_load(ecalld_config_t *cfg, const char *path);

#endif /* ECALLD_CONFIG_H */
