/*
 * can_if.c - SocketCAN crash trigger + UDP manual SOS.
 */
#include "can_if.h"

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#define CAN_SOS_DGRAM_MAX   (64)

ecall_status_t can_if_decode_frame(uint32_t        can_id,
                                   const uint8_t  *data,
                                   uint8_t         dlc,
                                   trigger_kind_t *out)
{
    if ((data == NULL) || (out == NULL))
    {
        return ECALL_ERR_PARAM;
    }

    *out = TRIGGER_NONE;

    if (can_id != (uint32_t)CAN_CRASH_FRAME_ID)
    {
        return ECALL_OK; /* not our frame, not an error */
    }
    if (dlc < 1u)
    {
        return ECALL_ERR_PARSE;
    }

    if (data[0] == (uint8_t)CAN_CRASH_BYTE0_FIRED)
    {
        *out = TRIGGER_AUTOMATIC;
    }

    return ECALL_OK;
}

ecall_status_t can_if_decode_sos(const char     *payload,
                                 size_t          len,
                                 trigger_kind_t *out)
{
    size_t mlen;

    if ((payload == NULL) || (out == NULL))
    {
        return ECALL_ERR_PARAM;
    }

    *out  = TRIGGER_NONE;
    mlen  = strlen(CAN_SOS_MAGIC);

    if (len < mlen)
    {
        return ECALL_ERR_PARSE;
    }

    if (strncmp(payload, CAN_SOS_MAGIC, mlen) == 0)
    {
        *out = TRIGGER_MANUAL;
        return ECALL_OK;
    }

    return ECALL_ERR_PARSE;
}

ecall_status_t can_if_init(can_if_t *can)
{
    if (can == NULL)
    {
        return ECALL_ERR_PARAM;
    }
    (void)memset(can, 0, sizeof(*can));
    can->can_fd = -1;
    can->sos_fd = -1;
    return ECALL_OK;
}

static ecall_status_t set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        return ECALL_ERR_IO;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
    {
        return ECALL_ERR_IO;
    }
    return ECALL_OK;
}

ecall_status_t can_if_open_can(can_if_t *can, const char *ifname)
{
    int                fd;
    struct ifreq       ifr;
    struct sockaddr_can addr;

    if ((can == NULL) || (ifname == NULL))
    {
        return ECALL_ERR_PARAM;
    }
    if (strlen(ifname) >= sizeof(can->ifname))
    {
        return ECALL_ERR_OVERFLOW;
    }

    fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0)
    {
        return ECALL_ERR_IO;
    }

    (void)memset(&ifr, 0, sizeof(ifr));
    (void)strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1u);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) != 0)
    {
        (void)close(fd);
        return ECALL_ERR_IO;
    }

    (void)memset(&addr, 0, sizeof(addr));
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        (void)close(fd);
        return ECALL_ERR_IO;
    }

    if (set_nonblock(fd) != ECALL_OK)
    {
        (void)close(fd);
        return ECALL_ERR_IO;
    }

    (void)strncpy(can->ifname, ifname, sizeof(can->ifname) - 1u);
    can->can_fd = fd;
    return ECALL_OK;
}

ecall_status_t can_if_open_sos(can_if_t *can, uint16_t udp_port)
{
    int                fd;
    struct sockaddr_in addr;

    if (can == NULL)
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

    if (set_nonblock(fd) != ECALL_OK)
    {
        (void)close(fd);
        return ECALL_ERR_IO;
    }

    can->sos_fd = fd;
    return ECALL_OK;
}

ecall_status_t can_if_can_fd(const can_if_t *can, int *out_fd)
{
    if ((can == NULL) || (out_fd == NULL))
    {
        return ECALL_ERR_PARAM;
    }
    if (can->can_fd < 0)
    {
        return ECALL_ERR_STATE;
    }
    *out_fd = can->can_fd;
    return ECALL_OK;
}

ecall_status_t can_if_sos_fd(const can_if_t *can, int *out_fd)
{
    if ((can == NULL) || (out_fd == NULL))
    {
        return ECALL_ERR_PARAM;
    }
    if (can->sos_fd < 0)
    {
        return ECALL_ERR_STATE;
    }
    *out_fd = can->sos_fd;
    return ECALL_OK;
}

ecall_status_t can_if_poll_can(can_if_t *can, trigger_kind_t *out)
{
    struct can_frame frame;
    ssize_t          got;
    bool             any   = false;
    trigger_kind_t   found = TRIGGER_NONE;

    if ((can == NULL) || (out == NULL))
    {
        return ECALL_ERR_PARAM;
    }
    if (can->can_fd < 0)
    {
        return ECALL_ERR_STATE;
    }

    *out = TRIGGER_NONE;

    for (;;)
    {
        got = read(can->can_fd, &frame, sizeof(frame));
        if (got < 0)
        {
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
            {
                break;
            }
            return ECALL_ERR_IO;
        }
        if (got < (ssize_t)sizeof(frame))
        {
            continue;
        }

        any = true;
        {
            trigger_kind_t k = TRIGGER_NONE;
            const ecall_status_t ds =
                can_if_decode_frame(frame.can_id & CAN_SFF_MASK,
                                    frame.data, frame.can_dlc, &k);
            if ((ds == ECALL_OK) && (k == TRIGGER_AUTOMATIC))
            {
                found = TRIGGER_AUTOMATIC;
            }
        }
    }

    *out = found;
    return any ? ECALL_OK : ECALL_ERR_NODATA;
}

ecall_status_t can_if_poll_sos(can_if_t *can, trigger_kind_t *out)
{
    char    buf[CAN_SOS_DGRAM_MAX];
    ssize_t got;
    bool    any   = false;
    trigger_kind_t found = TRIGGER_NONE;

    if ((can == NULL) || (out == NULL))
    {
        return ECALL_ERR_PARAM;
    }
    if (can->sos_fd < 0)
    {
        return ECALL_ERR_STATE;
    }

    *out = TRIGGER_NONE;

    for (;;)
    {
        got = recv(can->sos_fd, buf, sizeof(buf) - 1u, 0);
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

        any = true;
        {
            trigger_kind_t k = TRIGGER_NONE;
            if (can_if_decode_sos(buf, (size_t)got, &k) == ECALL_OK)
            {
                if (k == TRIGGER_MANUAL)
                {
                    found = TRIGGER_MANUAL;
                }
            }
        }
    }

    *out = found;
    return any ? ECALL_OK : ECALL_ERR_NODATA;
}

ecall_status_t can_if_close(can_if_t *can)
{
    if (can == NULL)
    {
        return ECALL_ERR_PARAM;
    }
    if (can->can_fd >= 0)
    {
        (void)close(can->can_fd);
        can->can_fd = -1;
    }
    if (can->sos_fd >= 0)
    {
        (void)close(can->sos_fd);
        can->sos_fd = -1;
    }
    return ECALL_OK;
}
