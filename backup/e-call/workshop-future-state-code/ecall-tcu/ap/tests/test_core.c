/*
 * test_core.c - State machine tests (requirement ECALL-03).
 */
#include "unity.h"
#include "ecall_core.h"

void setUp(void)    { }
void tearDown(void) { }

/* Full happy path IDLE -> ... -> CLOSED. */
static void test_ECALL_CORE_01_full_sequence(void)
{
    ecall_core_t  c;
    ecall_state_t s;

    TEST_ASSERT_EQUAL_INT(ECALL_OK, ecall_core_init(&c));
    TEST_ASSERT_EQUAL_INT(ECALL_OK,
        ecall_core_on_trigger(&c, TRIGGER_AUTOMATIC));
    (void)ecall_core_state(&c, &s);
    TEST_ASSERT_EQUAL_INT(ECALL_STATE_TRIGGERED, s);

    TEST_ASSERT_EQUAL_INT(ECALL_OK, ecall_core_on_msd_sent(&c));
    (void)ecall_core_state(&c, &s);
    TEST_ASSERT_EQUAL_INT(ECALL_STATE_MSD_SENT, s);

    TEST_ASSERT_EQUAL_INT(ECALL_OK, ecall_core_on_voice_connected(&c));
    (void)ecall_core_state(&c, &s);
    TEST_ASSERT_EQUAL_INT(ECALL_STATE_VOICE_CONNECTED, s);

    TEST_ASSERT_EQUAL_INT(ECALL_OK, ecall_core_on_close(&c));
    (void)ecall_core_state(&c, &s);
    TEST_ASSERT_EQUAL_INT(ECALL_STATE_CLOSED, s);

    TEST_ASSERT_EQUAL_INT(ECALL_OK, ecall_core_reset(&c));
    (void)ecall_core_state(&c, &s);
    TEST_ASSERT_EQUAL_INT(ECALL_STATE_IDLE, s);
}

/* Duplicate triggers while active are ignored and counted. */
static void test_ECALL_CORE_02_duplicate_trigger_ignored(void)
{
    ecall_core_t c;

    (void)ecall_core_init(&c);
    TEST_ASSERT_EQUAL_INT(ECALL_OK,
        ecall_core_on_trigger(&c, TRIGGER_AUTOMATIC));

    /* second trigger while active must be a no-op */
    TEST_ASSERT_EQUAL_INT(ECALL_ERR_STATE,
        ecall_core_on_trigger(&c, TRIGGER_MANUAL));
    TEST_ASSERT_EQUAL_INT(ECALL_ERR_STATE,
        ecall_core_on_trigger(&c, TRIGGER_AUTOMATIC));

    TEST_ASSERT_EQUAL_UINT(2u, c.ignored_dups);
    TEST_ASSERT_EQUAL_UINT(1u, c.session_id);
    TEST_ASSERT_TRUE(ecall_core_is_active(&c));
}

/* Out-of-order transitions are rejected. */
static void test_ECALL_CORE_03_bad_transition_rejected(void)
{
    ecall_core_t c;

    (void)ecall_core_init(&c);
    /* cannot go MSD_SENT from IDLE */
    TEST_ASSERT_EQUAL_INT(ECALL_ERR_STATE, ecall_core_on_msd_sent(&c));
    /* cannot voice-connect from IDLE */
    TEST_ASSERT_EQUAL_INT(ECALL_ERR_STATE,
        ecall_core_on_voice_connected(&c));
    /* cannot close when not active */
    TEST_ASSERT_EQUAL_INT(ECALL_ERR_STATE, ecall_core_on_close(&c));
}

/* A new call can start after reset; session_id increments. */
static void test_ECALL_CORE_04_second_session(void)
{
    ecall_core_t c;

    (void)ecall_core_init(&c);
    (void)ecall_core_on_trigger(&c, TRIGGER_MANUAL);
    (void)ecall_core_on_msd_sent(&c);
    (void)ecall_core_on_voice_connected(&c);
    (void)ecall_core_on_close(&c);
    (void)ecall_core_reset(&c);

    TEST_ASSERT_EQUAL_INT(ECALL_OK,
        ecall_core_on_trigger(&c, TRIGGER_AUTOMATIC));
    TEST_ASSERT_EQUAL_UINT(2u, c.session_id);
}

/* Invalid trigger kind rejected. */
static void test_ECALL_CORE_05_invalid_trigger(void)
{
    ecall_core_t c;
    (void)ecall_core_init(&c);
    TEST_ASSERT_EQUAL_INT(ECALL_ERR_PARAM,
        ecall_core_on_trigger(&c, TRIGGER_NONE));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ECALL_CORE_01_full_sequence);
    RUN_TEST(test_ECALL_CORE_02_duplicate_trigger_ignored);
    RUN_TEST(test_ECALL_CORE_03_bad_transition_rejected);
    RUN_TEST(test_ECALL_CORE_04_second_session);
    RUN_TEST(test_ECALL_CORE_05_invalid_trigger);
    return UNITY_END();
}
