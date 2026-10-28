/*
 * psap_if.c - UDP MSD transport with bounded retries.
 */
#include "psap_if.h"

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

ecall_status_t psap_if_send_with_retry(psap_send_fn_t send_fn,
                                       void          *ctx,
                                       const uint8_t *buf,
                                       size_t         len,
                                       uint8_t        max_attempts,
                                       uint8_t       *attempts_used)
{
    uint8_t        attempt;
    ecall_status_t st = ECALL_ERR_TIMEOUT;

    if ((send_fn == NULL) || (buf == NULL) || (attempts_used == NULL))
    {
        return ECALL_ERR_PARAM;
    }
    if ((len == 0u) || (max_attempts == 0u))
    {
        return ECALL_ERR_PARAM;
    }

    *attempts_used = 0u;

    for (attempt = 0u; attempt < max_attempts; ++attempt)
    {
        *attempts_used = (uint8_t)(attempt + 1u);
        st = send_fn(ctx, buf, len);
        if (st == ECALL_OK)
        {
            break;
        }
    }

    return (st == ECALL_OK) ? ECALL_OK : ECALL_ERR_TIMEOUT;
}

ecall_status_t psap_if_init(psap_if_t *psap)
{
    if (psap == NULL)
    {
        return ECALL_ERR_PARAM;
    }
    (void)memset(psap, 0, sizeof(*psap));
    psap->fd = -1;
    return ECALL_OK;
}

ecall_status_t psap_if_open(psap_if_t  *psap,
                            const char *host,
                            uint16_t    port)
{
    int fd;

    if ((psap == NULL) || (host == NULL))
    {
        return ECALL_ERR_PARAM;
    }
    if (strlen(host) >= (size_t)PSAP_HOST_MAX)
    {
        return ECALL_ERR_OVERFLOW;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return ECALL_ERR_IO;
    }

    (void)strncpy(psap->host, host, (size_t)PSAP_HOST_MAX - 1u);
    psap->port = port;
    psap->fd   = fd;
    return ECALL_OK;
}

/* Adapter so the real UDP path reuses the testable retry core. */
typedef struct
{
    int                fd;
    struct sockaddr_in dst;
} udp_ctx_t;

static ecall_status_t udp_send_once(void          *ctx,
                                    const uint8_t *buf,
                                    size_t         len)
{
    const udp_ctx_t *uc = (const udp_ctx_t *)ctx;
    ssize_t          sent;

    sent = sendto(uc->fd, buf, len, 0,
                  (const struct sockaddr *)&uc->dst, sizeof(uc->dst));
    if (sent < 0)
    {
        return ECALL_ERR_IO;
    }
    if ((size_t)sent != len)
    {
        return ECALL_ERR_IO;
    }
    return ECALL_OK;
}

ecall_status_t psap_if_send_msd(psap_if_t     *psap,
                                const uint8_t *msd,
                                size_t         len)
{
    udp_ctx_t      uc;
    ecall_status_t st;

    if ((psap == NULL) || (msd == NULL))
    {
        return ECALL_ERR_PARAM;
    }
    if (psap->fd < 0)
    {
        return ECALL_ERR_STATE;
    }
    if (len == 0u)
    {
        return ECALL_ERR_PARAM;
    }

    (void)memset(&uc, 0, sizeof(uc));
    uc.fd              = psap->fd;
    uc.dst.sin_family  = AF_INET;
    uc.dst.sin_port    = htons(psap->port);
    if (inet_pton(AF_INET, psap->host, &uc.dst.sin_addr) != 1)
    {
        return ECALL_ERR_PARAM;
    }

    st = psap_if_send_with_retry(udp_send_once, &uc, msd, len,
                                 (uint8_t)PSAP_MAX_ATTEMPTS,
                                 &psap->last_attempts);
    return st;
}

ecall_status_t psap_if_close(psap_if_t *psap)
{
    if (psap == NULL)
    {
        return ECALL_ERR_PARAM;
    }
    if (psap->fd >= 0)
    {
        (void)close(psap->fd);
        psap->fd = -1;
    }
    return ECALL_OK;
}
