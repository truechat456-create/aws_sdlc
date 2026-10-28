/*
 * test_can.c - CAN crash frame + SOS decoding tests (requirement ECALL-01).
 */
#include "unity.h"
#include "can_if.h"

void setUp(void)    { }
void tearDown(void) { }

/* Crash frame: ID 0x0A0, byte0 0x01 => AUTOMATIC. */
static void test_ECALL_CAN_01_airbag_fired_automatic(void)
{
    trigger_kind_t out = TRIGGER_NONE;
    uint8_t        data[8] = { 0x01u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };

    TEST_ASSERT_EQUAL_INT(ECALL_OK,
        can_if_decode_frame(0x0A0u, data, 8u, &out));
    TEST_ASSERT_EQUAL_INT(TRIGGER_AUTOMATIC, out);
}

/* Correct ID but byte0 != 0x01 => no trigger. */
static void test_ECALL_CAN_02_airbag_not_fired(void)
{
    trigger_kind_t out = TRIGGER_AUTOMATIC;
    uint8_t        data[8] = { 0x00u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };

    TEST_ASSERT_EQUAL_INT(ECALL_OK,
        can_if_decode_frame(0x0A0u, data, 8u, &out));
    TEST_ASSERT_EQUAL_INT(TRIGGER_NONE, out);
}

/* Wrong CAN ID is ignored (not our frame). */
static void test_ECALL_CAN_03_wrong_id_ignored(void)
{
    trigger_kind_t out = TRIGGER_AUTOMATIC;
    uint8_t        data[8] = { 0x01u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };

    TEST_ASSERT_EQUAL_INT(ECALL_OK,
        can_if_decode_frame(0x123u, data, 8u, &out));
    TEST_ASSERT_EQUAL_INT(TRIGGER_NONE, out);
}

/* Zero DLC on our frame is a parse error. */
static void test_ECALL_CAN_04_zero_dlc(void)
{
    trigger_kind_t out = TRIGGER_NONE;
    uint8_t        data[8] = { 0u };

    TEST_ASSERT_EQUAL_INT(ECALL_ERR_PARSE,
        can_if_decode_frame(0x0A0u, data, 0u, &out));
}

/* UDP "SOS" => MANUAL. */
static void test_ECALL_CAN_05_sos_manual(void)
{
    trigger_kind_t out = TRIGGER_NONE;
    const char    *p   = "SOS";

    TEST_ASSERT_EQUAL_INT(ECALL_OK,
        can_if_decode_sos(p, 3u, &out));
    TEST_ASSERT_EQUAL_INT(TRIGGER_MANUAL, out);
}

/* Non-SOS payload is rejected. */
static void test_ECALL_CAN_06_non_sos_rejected(void)
{
    trigger_kind_t out = TRIGGER_MANUAL;
    const char    *p   = "PING";

    TEST_ASSERT_EQUAL_INT(ECALL_ERR_PARSE,
        can_if_decode_sos(p, 4u, &out));
    TEST_ASSERT_EQUAL_INT(TRIGGER_NONE, out);
}

/* NULL args rejected. */
static void test_ECALL_CAN_07_null_params(void)
{
    uint8_t data[1] = { 0x01u };
    TEST_ASSERT_EQUAL_INT(ECALL_ERR_PARAM,
        can_if_decode_frame(0x0A0u, data, 1u, NULL));
    TEST_ASSERT_EQUAL_INT(ECALL_ERR_PARAM,
        can_if_decode_sos(NULL, 3u, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ECALL_CAN_01_airbag_fired_automatic);
    RUN_TEST(test_ECALL_CAN_02_airbag_not_fired);
    RUN_TEST(test_ECALL_CAN_03_wrong_id_ignored);
    RUN_TEST(test_ECALL_CAN_04_zero_dlc);
    RUN_TEST(test_ECALL_CAN_05_sos_manual);
    RUN_TEST(test_ECALL_CAN_06_non_sos_rejected);
    RUN_TEST(test_ECALL_CAN_07_null_params);
    return UNITY_END();
}
