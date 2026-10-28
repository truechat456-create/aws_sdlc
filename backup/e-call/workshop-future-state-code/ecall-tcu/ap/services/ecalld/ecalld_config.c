/*
 * ecalld_config.c - "key = value" config parser.
 */
#include "ecalld_config.h"

#include "gnss_if.h"
#include "can_if.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define LINE_MAX_LEN   (256)

ecall_status_t ecalld_config_defaults(ecalld_config_t *cfg)
{
    if (cfg == NULL)
    {
        return ECALL_ERR_PARAM;
    }

    (void)memset(cfg, 0, sizeof(*cfg));
    cfg->gnss_port  = (uint16_t)GNSS_DEFAULT_UDP_PORT;
    cfg->sos_port   = (uint16_t)CAN_SOS_DEFAULT_PORT;
    (void)strncpy(cfg->can_ifname, "vcan0", sizeof(cfg->can_ifname) - 1u);
    (void)strncpy(cfg->psap_host, "127.0.0.1", sizeof(cfg->psap_host) - 1u);
    cfg->psap_port  = (uint16_t)5000;
    (void)snprintf(cfg->vin, sizeof(cfg->vin), "%s", "SYNTHVIN000000001");
    cfg->passengers = 1u;
    cfg->test_call  = false;
    return ECALL_OK;
}

/* Bounded non-negative decimal parse (no errno/strto* dependency). */
static uint32_t cfg_u32(const char *s)
{
    uint32_t v = 0u;
    size_t   i;

    if (s == NULL)
    {
        return 0u;
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
    return v;
}

/* Trim leading/trailing ASCII whitespace in place; returns new start. */
static char *trim(char *s)
{
    size_t len;

    while ((*s == ' ') || (*s == '\t'))
    {
        ++s;
    }

    len = strlen(s);
    while (len > 0u)
    {
        char c = s[len - 1u];
        if ((c == ' ') || (c == '\t') || (c == '\r') || (c == '\n'))
        {
            s[len - 1u] = '\0';
            --len;
        }
        else
        {
            break;
        }
    }
    return s;
}

ecall_status_t ecalld_config_parse_line(ecalld_config_t *cfg,
                                        const char      *line)
{
    char  work[LINE_MAX_LEN];
    char *eq;
    char *key;
    char *val;

    if ((cfg == NULL) || (line == NULL))
    {
        return ECALL_ERR_PARAM;
    }
    if (strlen(line) >= (size_t)LINE_MAX_LEN)
    {
        return ECALL_ERR_OVERFLOW;
    }

    (void)strcpy(work, line);

    {
        char *t = trim(work);
        if ((t[0] == '\0') || (t[0] == '#'))
        {
            return ECALL_OK; /* blank or comment */
        }
    }

    eq = strchr(work, '=');
    if (eq == NULL)
    {
        return ECALL_ERR_PARSE;
    }

    *eq = '\0';
    key = trim(work);
    val = trim(eq + 1);

    if (strcmp(key, "gnss_port") == 0)
    {
        cfg->gnss_port = (uint16_t)cfg_u32(val);
    }
    else if (strcmp(key, "sos_port") == 0)
    {
        cfg->sos_port = (uint16_t)cfg_u32(val);
    }
    else if (strcmp(key, "can_ifname") == 0)
    {
        (void)strncpy(cfg->can_ifname, val, sizeof(cfg->can_ifname) - 1u);
        cfg->can_ifname[sizeof(cfg->can_ifname) - 1u] = '\0';
    }
    else if (strcmp(key, "psap_host") == 0)
    {
        (void)strncpy(cfg->psap_host, val, sizeof(cfg->psap_host) - 1u);
        cfg->psap_host[sizeof(cfg->psap_host) - 1u] = '\0';
    }
    else if (strcmp(key, "psap_port") == 0)
    {
        cfg->psap_port = (uint16_t)cfg_u32(val);
    }
    else if (strcmp(key, "vin") == 0)
    {
        (void)snprintf(cfg->vin, sizeof(cfg->vin), "%s", val);
    }
    else if (strcmp(key, "passengers") == 0)
    {
        cfg->passengers = (uint8_t)cfg_u32(val);
    }
    else if (strcmp(key, "test_call") == 0)
    {
        cfg->test_call = (strcmp(val, "1") == 0) ||
                         (strcmp(val, "true") == 0);
    }
    else
    {
        /* unknown key: ignore for forward-compatibility */
    }

    return ECALL_OK;
}

ecall_status_t ecalld_config_load(ecalld_config_t *cfg, const char *path)
{
    FILE *fp;
    char  line[LINE_MAX_LEN];

    if (cfg == NULL)
    {
        return ECALL_ERR_PARAM;
    }

    (void)ecalld_config_defaults(cfg);

    if (path == NULL)
    {
        return ECALL_OK;
    }

    fp = fopen(path, "re");
    if (fp == NULL)
    {
        return ECALL_OK; /* no file -> defaults only */
    }

    while (fgets(line, (int)sizeof(line), fp) != NULL)
    {
        (void)ecalld_config_parse_line(cfg, line);
    }

    (void)fclose(fp);
    return ECALL_OK;
}
