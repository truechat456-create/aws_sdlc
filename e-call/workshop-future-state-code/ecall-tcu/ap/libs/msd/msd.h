/*
 * msd.h - Minimum Set of Data (MSD) encoder.
 *
 * Baseline v1. Binary MSD, fixed layout, total size <= 140 bytes.
 * Position rule (v1): if the newest live fix is <= 2 s old the position is
 * set and position_trusted = true; otherwise lat/lon are NOT_AVAILABLE and
 * position_trusted = false. There is NO fallback in v1.
 *
 * Full byte layout is documented in docs/msd_layout.md.
 */
#ifndef MSD_H
#define MSD_H

#include "ecall_status.h"

#define MSD_MAX_BYTES           (140)
#define MSD_VERSION             (1u)
#define MSD_VIN_LEN             (17u)

/* Sentinel written into lat/lon when no trusted position is available. */
#define MSD_COORD_NOT_AVAILABLE (0x7FFFFFFF)

/* Maximum fix age that still counts as a live position, in milliseconds. */
#define MSD_MAX_FIX_AGE_MS      (2000u)

typedef enum
{
    MSD_ACT_AUTOMATIC = 1,   /* crash-triggered                             */
    MSD_ACT_MANUAL    = 2    /* manual SOS                                  */
} msd_activation_t;

/* Inputs the caller assembles before encoding. */
typedef struct
{
    uint8_t          version;          /* = MSD_VERSION                     */
    uint32_t         message_id;       /* incrementing per eCall session    */
    msd_activation_t activation;       /* automatic / manual                */
    bool             test_call;        /* true = test, not a real emergency */
    char             vin[MSD_VIN_LEN + 1u]; /* NUL-terminated VIN           */

    uint64_t         timestamp_utc;    /* seconds since Unix epoch          */

    bool             position_trusted; /* true if lat/lon are live & fresh  */
    int32_t          lat_mas;          /* latitude  in milliarcseconds      */
    int32_t          lon_mas;          /* longitude in milliarcseconds      */
    uint16_t         direction_deg;    /* travel direction, 0..359          */
    uint8_t          passengers;       /* occupants (best estimate)         */
} msd_input_t;

/*
 * Build an MSD input from raw fields. Converts decimal-degree coordinates to
 * milliarcseconds. If !have_live_fix OR fix_age_ms > MSD_MAX_FIX_AGE_MS the
 * position is marked NOT_AVAILABLE (position_trusted = false).
 */
ecall_status_t msd_fill_position(msd_input_t *msd,
                                 bool         have_live_fix,
                                 double       lat_deg,
                                 double       lon_deg,
                                 uint64_t     fix_age_ms,
                                 uint16_t     direction_deg);

/*
 * Encode msd into out[] (big-endian, fixed layout). Writes the number of
 * bytes used to *out_len. Guaranteed <= MSD_MAX_BYTES or ECALL_ERR_OVERFLOW.
 */
ecall_status_t msd_encode(const msd_input_t *msd,
                          uint8_t           *out,
                          size_t             out_cap,
                          size_t            *out_len);

/* Convert decimal degrees to milliarcseconds (1 deg = 3 600 000 mas). */
ecall_status_t msd_deg_to_mas(double deg, int32_t *out_mas);

#endif /* MSD_H */
