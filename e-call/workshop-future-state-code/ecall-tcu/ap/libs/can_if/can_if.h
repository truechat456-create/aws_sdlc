/*
 * can_if.h - Crash / trigger interface.
 *
 *  - SocketCAN (vcan0): CAN frame ID 0x0A0, byte0 == 0x01 => AUTOMATIC
 *    (airbag deployed) eCall trigger.
 *  - UDP port 10111: ASCII payload "SOS" => MANUAL eCall trigger.
 *
 * Baseline v1.
 */
#ifndef CAN_IF_H
#define CAN_IF_H

#include "ecall_status.h"

#define CAN_CRASH_FRAME_ID      (0x0A0u)  /* airbag / crash status frame    */
#define CAN_CRASH_BYTE0_FIRED   (0x01u)   /* byte0 value = airbag deployed  */
#define CAN_SOS_DEFAULT_PORT     (10111)   /* manual SOS UDP port            */
#define CAN_SOS_MAGIC           "SOS"

/* Trigger classification. */
typedef enum
{
    TRIGGER_NONE = 0,
    TRIGGER_AUTOMATIC,   /* crash / airbag via CAN                         */
    TRIGGER_MANUAL       /* manual SOS via UDP                             */
} trigger_kind_t;

typedef struct
{
    int            can_fd;   /* SocketCAN raw socket, -1 when closed       */
    int            sos_fd;   /* UDP SOS socket, -1 when closed             */
    char           ifname[16];
} can_if_t;

ecall_status_t can_if_init(can_if_t *can);

/* Bind the SocketCAN raw socket to the named interface (e.g. "vcan0"). */
ecall_status_t can_if_open_can(can_if_t *can, const char *ifname);

/* Open the manual-SOS UDP listener (use CAN_SOS_DEFAULT_PORT). */
ecall_status_t can_if_open_sos(can_if_t *can, uint16_t udp_port);

ecall_status_t can_if_can_fd(const can_if_t *can, int *out_fd);
ecall_status_t can_if_sos_fd(const can_if_t *can, int *out_fd);

/*
 * Drain the CAN socket. Sets *out = TRIGGER_AUTOMATIC if a crash frame was
 * seen, else TRIGGER_NONE. ECALL_ERR_NODATA when nothing was pending.
 */
ecall_status_t can_if_poll_can(can_if_t *can, trigger_kind_t *out);

/*
 * Drain the SOS socket. Sets *out = TRIGGER_MANUAL if a valid SOS datagram
 * was seen, else TRIGGER_NONE. ECALL_ERR_NODATA when nothing was pending.
 */
ecall_status_t can_if_poll_sos(can_if_t *can, trigger_kind_t *out);

/*
 * Pure decoder for a raw 8-byte CAN payload with its id. Exposed for tests.
 * Returns TRIGGER_AUTOMATIC or TRIGGER_NONE via *out.
 */
ecall_status_t can_if_decode_frame(uint32_t        can_id,
                                   const uint8_t  *data,
                                   uint8_t         dlc,
                                   trigger_kind_t *out);

/* Pure decoder for an SOS datagram. Exposed for tests. */
ecall_status_t can_if_decode_sos(const char     *payload,
                                 size_t          len,
                                 trigger_kind_t *out);

ecall_status_t can_if_close(can_if_t *can);

#endif /* CAN_IF_H */
