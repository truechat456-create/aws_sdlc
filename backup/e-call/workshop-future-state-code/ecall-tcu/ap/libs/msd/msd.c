/*
 * msd.c - Binary MSD encoder (big-endian). See docs/msd_layout.md.
 *
 * Fixed layout, total 54 bytes used (well under the 140-byte cap). No
 * dynamic allocation. Every helper returns a status enum.
 */
#include "msd.h"

#include <string.h>
#include <math.h>

/* Field offsets (bytes) within the MSD buffer. */
#define OFF_VERSION        (0u)   /* u8                                     */
#define OFF_MESSAGE_ID     (1u)   /* u32 BE                                 */
#define OFF_ACTIVATION     (5u)   /* u8                                     */
#define OFF_FLAGS          (6u)   /* u8: bit0 test, bit1 position_trusted   */
#define OFF_VIN            (7u)   /* 17 bytes, space-padded ASCII           */
#define OFF_TIMESTAMP      (24u)  /* u64 BE, Unix seconds                   */
#define OFF_LAT            (32u)  /* i32 BE, milliarcseconds                */
#define OFF_LON            (36u)  /* i32 BE, milliarcseconds                */
#define OFF_DIRECTION      (40u)  /* u16 BE, degrees                        */
#define OFF_PASSENGERS     (42u)  /* u8                                     */
#define OFF_RESERVED       (43u)  /* u8, 0                                  */
#define MSD_USED_BYTES     (44u)

#define FLAG_TEST_BIT      (0x01u)
#define FLAG_POS_TRUSTED   (0x02u)

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xFFu);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static void put_u64(uint8_t *p, uint64_t v)
{
    uint8_t i;
    for (i = 0u; i < 8u; ++i)
    {
        unsigned int shift = (unsigned int)(7u - i) * 8u;
        p[i] = (uint8_t)((v >> shift) & 0xFFu);
    }
}

ecall_status_t msd_deg_to_mas(double deg, int32_t *out_mas)
{
    double scaled;

    if (out_mas == NULL)
    {
        return ECALL_ERR_PARAM;
    }
    if ((deg < -360.0) || (deg > 360.0))
    {
        return ECALL_ERR_PARAM;
    }

    scaled = deg * 3600000.0; /* 1 degree = 3 600 000 milliarcseconds */

    /* round half away from zero */
    if (scaled >= 0.0)
    {
        scaled += 0.5;
    }
    else
    {
        scaled -= 0.5;
    }

    *out_mas = (int32_t)scaled;
    return ECALL_OK;
}

ecall_status_t msd_fill_position(msd_input_t *msd,
                                 bool         have_live_fix,
                                 double       lat_deg,
                                 double       lon_deg,
                                 uint64_t     fix_age_ms,
                                 uint16_t     direction_deg)
{
    if (msd == NULL)
    {
        return ECALL_ERR_PARAM;
    }

    msd->direction_deg = (uint16_t)(direction_deg % 360u);

    /* v1: only a live, fresh fix yields a trusted position. No fallback. */
    if (have_live_fix && (fix_age_ms <= (uint64_t)MSD_MAX_FIX_AGE_MS))
    {
        int32_t lat_mas = 0;
        int32_t lon_mas = 0;

        if (msd_deg_to_mas(lat_deg, &lat_mas) != ECALL_OK)
        {
            return ECALL_ERR_PARAM;
        }
        if (msd_deg_to_mas(lon_deg, &lon_mas) != ECALL_OK)
        {
            return ECALL_ERR_PARAM;
        }

        msd->lat_mas          = lat_mas;
        msd->lon_mas          = lon_mas;
        msd->position_trusted = true;
    }
    else
    {
        msd->lat_mas          = (int32_t)MSD_COORD_NOT_AVAILABLE;
        msd->lon_mas          = (int32_t)MSD_COORD_NOT_AVAILABLE;
        msd->position_trusted = false;
    }

    return ECALL_OK;
}

ecall_status_t msd_encode(const msd_input_t *msd,
                          uint8_t           *out,
                          size_t             out_cap,
                          size_t            *out_len)
{
    uint8_t flags = 0u;
    size_t  vinlen;
    size_t  i;

    if ((msd == NULL) || (out == NULL) || (out_len == NULL))
    {
        return ECALL_ERR_PARAM;
    }
    if (out_cap < (size_t)MSD_USED_BYTES)
    {
        return ECALL_ERR_OVERFLOW;
    }
    if ((size_t)MSD_USED_BYTES > (size_t)MSD_MAX_BYTES)
    {
        return ECALL_ERR_OVERFLOW; /* compile-time truth; defensive         */
    }

    (void)memset(out, 0, (size_t)MSD_USED_BYTES);

    out[OFF_VERSION] = msd->version;
    put_u32(&out[OFF_MESSAGE_ID], msd->message_id);
    out[OFF_ACTIVATION] = (uint8_t)msd->activation;

    if (msd->test_call)
    {
        flags |= FLAG_TEST_BIT;
    }
    if (msd->position_trusted)
    {
        flags |= FLAG_POS_TRUSTED;
    }
    out[OFF_FLAGS] = flags;

    /* VIN: space-padded ASCII, exactly 17 bytes */
    (void)memset(&out[OFF_VIN], (int)' ', MSD_VIN_LEN);
    vinlen = strlen(msd->vin);
    if (vinlen > (size_t)MSD_VIN_LEN)
    {
        vinlen = (size_t)MSD_VIN_LEN;
    }
    for (i = 0u; i < vinlen; ++i)
    {
        out[OFF_VIN + i] = (uint8_t)msd->vin[i];
    }

    put_u64(&out[OFF_TIMESTAMP], msd->timestamp_utc);
    put_u32(&out[OFF_LAT], (uint32_t)msd->lat_mas);
    put_u32(&out[OFF_LON], (uint32_t)msd->lon_mas);
    put_u16(&out[OFF_DIRECTION], msd->direction_deg);
    out[OFF_PASSENGERS] = msd->passengers;
    out[OFF_RESERVED]   = 0u;

    *out_len = (size_t)MSD_USED_BYTES;
    return ECALL_OK;
}
