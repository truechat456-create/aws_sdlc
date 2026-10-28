/*
 * psap_if.h - PSAP (Public Safety Answering Point) transport.
 *
 * Sends the encoded MSD over UDP to a configurable host:port, with up to
 * PSAP_MAX_ATTEMPTS total send attempts (1 initial + 2 retries = 3 tries).
 *
 * Baseline v1: fire-and-retry over UDP; no acknowledgement protocol.
 */
#ifndef PSAP_IF_H
#define PSAP_IF_H

#include "ecall_status.h"

#define PSAP_MAX_ATTEMPTS   (3u)     /* total attempts incl. the first      */
#define PSAP_HOST_MAX       (64u)

typedef struct
{
    int      fd;                    /* UDP socket, -1 when closed           */
    char     host[PSAP_HOST_MAX];   /* dotted-quad or hostname              */
    uint16_t port;
    uint8_t  last_attempts;         /* attempts used on the last send       */
} psap_if_t;

ecall_status_t psap_if_init(psap_if_t *psap);

/* Resolve/configure the destination and create the socket. */
ecall_status_t psap_if_open(psap_if_t  *psap,
                            const char *host,
                            uint16_t    port);

/*
 * Send the MSD bytes. Retries on transient failure up to PSAP_MAX_ATTEMPTS.
 * Returns ECALL_OK on the first successful send, ECALL_ERR_TIMEOUT if all
 * attempts failed. The number of attempts used is stored in last_attempts.
 */
ecall_status_t psap_if_send_msd(psap_if_t     *psap,
                                const uint8_t *msd,
                                size_t         len);

/*
 * Testable core: performs up to max_attempts calls to send_fn, stopping at
 * the first success. send_fn returns ECALL_OK on success or an error code.
 * Writes the attempt count to *attempts_used.
 */
typedef ecall_status_t (*psap_send_fn_t)(void          *ctx,
                                         const uint8_t *buf,
                                         size_t         len);

ecall_status_t psap_if_send_with_retry(psap_send_fn_t send_fn,
                                       void          *ctx,
                                       const uint8_t *buf,
                                       size_t         len,
                                       uint8_t        max_attempts,
                                       uint8_t       *attempts_used);

ecall_status_t psap_if_close(psap_if_t *psap);

#endif /* PSAP_IF_H */
