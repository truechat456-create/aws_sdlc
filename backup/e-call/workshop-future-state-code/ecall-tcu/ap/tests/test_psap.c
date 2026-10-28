/*
 * test_psap.c - PSAP retry logic tests (requirement ECALL-06).
 *
 * Uses the injectable send function so no real sockets are needed.
 */
#include "unity.h"
#include "psap_if.h"

void setUp(void)    { }
void tearDown(void) { }

/* A fake sender that fails a configurable number of times first. */
typedef struct
{
    unsigned calls;
    unsigned fail_first;
} fake_t;

static ecall_status_t fake_send(void *ctx, const uint8_t *buf, size_t len)
{
    fake_t *f = (fake_t *)ctx;
    (void)buf;
    (void)len;
    f->calls++;
    if (f->calls <= f->fail_first)
    {
        return ECALL_ERR_IO;
    }
    return ECALL_OK;
}

static const uint8_t PAYLOAD[4] = { 1u, 2u, 3u, 4u };

/* First attempt succeeds -> 1 attempt used. */
static void test_ECALL_PSAP_01_first_try_ok(void)
{
    fake_t  f = { 0u, 0u };
    uint8_t used = 0u;

    TEST_ASSERT_EQUAL_INT(ECALL_OK,
        psap_if_send_with_retry(fake_send, &f, PAYLOAD, sizeof(PAYLOAD),
                                3u, &used));
    TEST_ASSERT_EQUAL_UINT(1u, used);
    TEST_ASSERT_EQUAL_UINT(1u, f.calls);
}

/* Two failures then success -> 3 attempts, success. */
static void test_ECALL_PSAP_02_succeeds_on_third(void)
{
    fake_t  f = { 0u, 2u };
    uint8_t used = 0u;

    TEST_ASSERT_EQUAL_INT(ECALL_OK,
        psap_if_send_with_retry(fake_send, &f, PAYLOAD, sizeof(PAYLOAD),
                                3u, &used));
    TEST_ASSERT_EQUAL_UINT(3u, used);
}

/* All three attempts fail -> timeout, exactly 3 attempts, no more. */
static void test_ECALL_PSAP_03_all_fail_timeout(void)
{
    fake_t  f = { 0u, 99u };
    uint8_t used = 0u;

    TEST_ASSERT_EQUAL_INT(ECALL_ERR_TIMEOUT,
        psap_if_send_with_retry(fake_send, &f, PAYLOAD, sizeof(PAYLOAD),
                                3u, &used));
    TEST_ASSERT_EQUAL_UINT(3u, used);
    TEST_ASSERT_EQUAL_UINT(3u, f.calls);
}

/* Default PSAP_MAX_ATTEMPTS is 3 (1 + 2 retries). */
static void test_ECALL_PSAP_04_max_attempts_is_three(void)
{
    TEST_ASSERT_EQUAL_UINT(3u, (unsigned)PSAP_MAX_ATTEMPTS);
}

/* NULL params rejected. */
static void test_ECALL_PSAP_05_null_params(void)
{
    uint8_t used = 0u;
    TEST_ASSERT_EQUAL_INT(ECALL_ERR_PARAM,
        psap_if_send_with_retry(NULL, NULL, PAYLOAD, sizeof(PAYLOAD),
                                3u, &used));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ECALL_PSAP_01_first_try_ok);
    RUN_TEST(test_ECALL_PSAP_02_succeeds_on_third);
    RUN_TEST(test_ECALL_PSAP_03_all_fail_timeout);
    RUN_TEST(test_ECALL_PSAP_04_max_attempts_is_three);
    RUN_TEST(test_ECALL_PSAP_05_null_params);
    return UNITY_END();
}
