/*
 * test_gnss.c - NMEA parsing tests (requirement ECALL-02).
 */
#include "unity.h"
#include "gnss_if.h"

void setUp(void)    { }
void tearDown(void) { }

/* Valid GGA yields a fix with correct decimal-degree conversion. */
static void test_ECALL_GNSS_01_parse_gga_valid(void)
{
    gnss_fix_t fix;
    ecall_status_t st;
    const char *gga =
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";

    (void)memset(&fix, 0, sizeof(fix));
    st = gnss_if_parse_sentence(gga, 1000u, &fix);

    TEST_ASSERT_EQUAL_INT(ECALL_OK, st);
    TEST_ASSERT_TRUE(fix.valid);
    /* 4807.038 N -> 48 + 07.038/60 = 48.1173 deg */
    TEST_ASSERT_INT_WITHIN(1, 48117, (long)(fix.lat_deg * 1000.0));
    /* 01131.000 E -> 11 + 31/60 = 11.5167 deg */
    TEST_ASSERT_INT_WITHIN(1, 11516, (long)(fix.lon_deg * 1000.0));
    TEST_ASSERT_EQUAL_INT(8, (long)fix.satellites);
    TEST_ASSERT_EQUAL_INT(1000, (long)fix.rx_mono_ms);
}

/* Valid RMC (status A) yields a fix. */
static void test_ECALL_GNSS_02_parse_rmc_active(void)
{
    gnss_fix_t fix;
    ecall_status_t st;
    const char *rmc =
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,,*11";

    (void)memset(&fix, 0, sizeof(fix));
    st = gnss_if_parse_sentence(rmc, 2000u, &fix);

    TEST_ASSERT_EQUAL_INT(ECALL_OK, st);
    TEST_ASSERT_TRUE(fix.valid);
    TEST_ASSERT_INT_WITHIN(1, 48117, (long)(fix.lat_deg * 1000.0));
}

/* RMC with status V (void) must not produce a valid fix. */
static void test_ECALL_GNSS_03_rmc_void_rejected(void)
{
    gnss_fix_t fix;
    ecall_status_t st;
    const char *rmc =
        "$GPRMC,123519,V,4807.038,N,01131.000,E,0,0,230394,,*0A";

    (void)memset(&fix, 0, sizeof(fix));
    st = gnss_if_parse_sentence(rmc, 2000u, &fix);

    TEST_ASSERT_EQUAL_INT(ECALL_ERR_NODATA, st);
    TEST_ASSERT_FALSE(fix.valid);
}

/* A bad checksum is rejected. */
static void test_ECALL_GNSS_04_bad_checksum(void)
{
    gnss_fix_t fix;
    ecall_status_t st;
    const char *gga =
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*00";

    (void)memset(&fix, 0, sizeof(fix));
    st = gnss_if_parse_sentence(gga, 1000u, &fix);

    TEST_ASSERT_EQUAL_INT(ECALL_ERR_PARSE, st);
}

/* Non GGA/RMC sentence is reported unsupported, not a crash. */
static void test_ECALL_GNSS_05_other_sentence_unsupported(void)
{
    gnss_fix_t fix;
    ecall_status_t st;
    const char *gsv = "$GPGSV,3,1,11,03,03,111,00*4A";

    (void)memset(&fix, 0, sizeof(fix));
    st = gnss_if_parse_sentence(gsv, 1000u, &fix);

    TEST_ASSERT_EQUAL_INT(ECALL_ERR_UNSUPPORTED, st);
}

/* NULL arguments are rejected. */
static void test_ECALL_GNSS_06_null_params(void)
{
    gnss_fix_t fix;
    TEST_ASSERT_EQUAL_INT(ECALL_ERR_PARAM,
                          gnss_if_parse_sentence(NULL, 0u, &fix));
    TEST_ASSERT_EQUAL_INT(ECALL_ERR_PARAM,
                          gnss_if_parse_sentence("$GPGGA", 0u, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ECALL_GNSS_01_parse_gga_valid);
    RUN_TEST(test_ECALL_GNSS_02_parse_rmc_active);
    RUN_TEST(test_ECALL_GNSS_03_rmc_void_rejected);
    RUN_TEST(test_ECALL_GNSS_04_bad_checksum);
    RUN_TEST(test_ECALL_GNSS_05_other_sentence_unsupported);
    RUN_TEST(test_ECALL_GNSS_06_null_params);
    return UNITY_END();
}
