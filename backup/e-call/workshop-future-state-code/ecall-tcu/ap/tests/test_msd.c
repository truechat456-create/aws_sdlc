/*
 * test_msd.c - MSD size, fields and position policy tests
 * (requirements ECALL-04, ECALL-05).
 */
#include "unity.h"
#include "msd.h"
#include <stdio.h>
#include <string.h>

void setUp(void)    { }
void tearDown(void) { }

static void base_input(msd_input_t *in)
{
    (void)memset(in, 0, sizeof(*in));
    in->version       = (uint8_t)MSD_VERSION;
    in->message_id    = 0x01020304u;
    in->activation    = MSD_ACT_AUTOMATIC;
    in->test_call     = false;
    (void)snprintf(in->vin, sizeof(in->vin), "%s", "SYNTHVIN000000001");
    in->timestamp_utc = 0x1122334455667788uLL;
    in->passengers    = 3u;
    in->direction_deg = 90u;
}

/* Encoded MSD never exceeds the 140-byte budget. */
static void test_ECALL_MSD_01_size_within_budget(void)
{
    msd_input_t in;
    uint8_t     buf[MSD_MAX_BYTES];
    size_t      len = 0u;

    base_input(&in);
    (void)msd_fill_position(&in, true, 48.0, 11.0, 500u, 90u);

    TEST_ASSERT_EQUAL_INT(ECALL_OK,
        msd_encode(&in, buf, sizeof(buf), &len));
    TEST_ASSERT_TRUE(len <= (size_t)MSD_MAX_BYTES);
    TEST_ASSERT_TRUE(len > 0u);
}

/* Header fields land at the documented offsets, big-endian. */
static void test_ECALL_MSD_02_header_fields(void)
{
    msd_input_t in;
    uint8_t     buf[MSD_MAX_BYTES];
    size_t      len = 0u;

    base_input(&in);
    (void)msd_fill_position(&in, true, 48.0, 11.0, 100u, 90u);
    (void)msd_encode(&in, buf, sizeof(buf), &len);

    TEST_ASSERT_EQUAL_HEX8(MSD_VERSION, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01u, buf[1]); /* message_id MSB */
    TEST_ASSERT_EQUAL_HEX8(0x04u, buf[4]); /* message_id LSB */
    TEST_ASSERT_EQUAL_HEX8((uint8_t)MSD_ACT_AUTOMATIC, buf[5]);
}

/* Live fix <= 2 s old -> position trusted. */
static void test_ECALL_MSD_03_fresh_fix_trusted(void)
{
    msd_input_t in;

    base_input(&in);
    TEST_ASSERT_EQUAL_INT(ECALL_OK,
        msd_fill_position(&in, true, 48.1173, 11.5167, 1500u, 90u));
    TEST_ASSERT_TRUE(in.position_trusted);
    TEST_ASSERT_TRUE(in.lat_mas != (int32_t)MSD_COORD_NOT_AVAILABLE);
}

/* Fix older than 2 s -> position NOT_AVAILABLE (no fallback in v1). */
static void test_ECALL_MSD_04_stale_fix_not_available(void)
{
    msd_input_t in;

    base_input(&in);
    TEST_ASSERT_EQUAL_INT(ECALL_OK,
        msd_fill_position(&in, true, 48.1173, 11.5167, 2001u, 90u));
    TEST_ASSERT_FALSE(in.position_trusted);
    TEST_ASSERT_EQUAL_INT((int32_t)MSD_COORD_NOT_AVAILABLE, in.lat_mas);
    TEST_ASSERT_EQUAL_INT((int32_t)MSD_COORD_NOT_AVAILABLE, in.lon_mas);
}

/* No live fix at all -> NOT_AVAILABLE. */
static void test_ECALL_MSD_05_no_fix_not_available(void)
{
    msd_input_t in;

    base_input(&in);
    TEST_ASSERT_EQUAL_INT(ECALL_OK,
        msd_fill_position(&in, false, 0.0, 0.0, 0u, 0u));
    TEST_ASSERT_FALSE(in.position_trusted);
    TEST_ASSERT_EQUAL_INT((int32_t)MSD_COORD_NOT_AVAILABLE, in.lat_mas);
}

/* Degrees -> milliarcseconds conversion. */
static void test_ECALL_MSD_06_deg_to_mas(void)
{
    int32_t mas = 0;
    TEST_ASSERT_EQUAL_INT(ECALL_OK, msd_deg_to_mas(1.0, &mas));
    TEST_ASSERT_EQUAL_INT(3600000, mas);
    TEST_ASSERT_EQUAL_INT(ECALL_OK, msd_deg_to_mas(-1.0, &mas));
    TEST_ASSERT_EQUAL_INT(-3600000, mas);
}

/* Output buffer too small is reported, never overrun. */
static void test_ECALL_MSD_07_small_buffer(void)
{
    msd_input_t in;
    uint8_t     small[8];
    size_t      len = 0u;

    base_input(&in);
    (void)msd_fill_position(&in, true, 48.0, 11.0, 100u, 90u);
    TEST_ASSERT_EQUAL_INT(ECALL_ERR_OVERFLOW,
        msd_encode(&in, small, sizeof(small), &len));
}

/* position_trusted flag bit is reflected in the flags byte. */
static void test_ECALL_MSD_08_flags_bit(void)
{
    msd_input_t in;
    uint8_t     buf[MSD_MAX_BYTES];
    size_t      len = 0u;

    base_input(&in);
    (void)msd_fill_position(&in, true, 48.0, 11.0, 100u, 90u);
    (void)msd_encode(&in, buf, sizeof(buf), &len);
    /* bit1 = position trusted */
    TEST_ASSERT_TRUE((buf[6] & 0x02u) != 0u);

    (void)msd_fill_position(&in, false, 0.0, 0.0, 0u, 0u);
    (void)msd_encode(&in, buf, sizeof(buf), &len);
    TEST_ASSERT_TRUE((buf[6] & 0x02u) == 0u);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ECALL_MSD_01_size_within_budget);
    RUN_TEST(test_ECALL_MSD_02_header_fields);
    RUN_TEST(test_ECALL_MSD_03_fresh_fix_trusted);
    RUN_TEST(test_ECALL_MSD_04_stale_fix_not_available);
    RUN_TEST(test_ECALL_MSD_05_no_fix_not_available);
    RUN_TEST(test_ECALL_MSD_06_deg_to_mas);
    RUN_TEST(test_ECALL_MSD_07_small_buffer);
    RUN_TEST(test_ECALL_MSD_08_flags_bit);
    return UNITY_END();
}
