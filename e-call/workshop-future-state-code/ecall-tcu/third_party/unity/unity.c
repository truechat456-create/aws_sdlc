/*
 * unity.c - Minimal Unity-style harness implementation.
 */
#include "unity.h"

unsigned     unity_tests_run    = 0u;
unsigned     unity_tests_failed = 0u;
const char  *unity_current_test = "";

static int unity_current_failed = 0;

void unity_begin(const char *file)
{
    (void)file;
    unity_tests_run    = 0u;
    unity_tests_failed = 0u;
}

int unity_end(void)
{
    (void)printf("\n-----------------------\n");
    (void)printf("%u Tests %u Failures\n",
                 unity_tests_run, unity_tests_failed);
    (void)printf("%s\n", (unity_tests_failed == 0u) ? "OK" : "FAIL");
    return (unity_tests_failed == 0u) ? 0 : 1;
}

void unity_run_test(void (*fn)(void), const char *name)
{
    unity_current_test   = name;
    unity_current_failed = 0;
    unity_tests_run++;

    setUp();
    fn();
    tearDown();

    if (unity_current_failed == 0)
    {
        (void)printf("PASS %s\n", name);
    }
}

void unity_fail(const char *file, int line, const char *msg)
{
    if (unity_current_failed == 0)
    {
        unity_tests_failed++;
        unity_current_failed = 1;
    }
    (void)printf("FAIL %s (%s:%d): %s\n",
                 unity_current_test, file, line, msg);
}

/* Each test translation unit defines its own setUp()/tearDown(). */
