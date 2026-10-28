/*
 * unity.h - Minimal Unity-style test harness (vendored for the eCall
 * workshop). This is a small, self-contained subset of the Unity API
 * (RUN_TEST, TEST_ASSERT_*). It is sufficient for the baseline tests and
 * keeps the project buildable offline.
 */
#ifndef UNITY_H
#define UNITY_H

#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned unity_tests_run;
extern unsigned unity_tests_failed;
extern const char *unity_current_test;

void unity_begin(const char *file);
int  unity_end(void);
void unity_run_test(void (*fn)(void), const char *name);
void unity_fail(const char *file, int line, const char *msg);

#define UNITY_BEGIN() unity_begin(__FILE__)
#define UNITY_END()   unity_end()

#define RUN_TEST(fn)  unity_run_test((fn), #fn)

#define TEST_FAIL_MESSAGE(msg) \
    do { unity_fail(__FILE__, __LINE__, (msg)); return; } while (0)

#define TEST_ASSERT_TRUE(cond) \
    do { if (!(cond)) { unity_fail(__FILE__, __LINE__, "expected TRUE: " #cond); return; } } while (0)

#define TEST_ASSERT_FALSE(cond) \
    do { if ((cond)) { unity_fail(__FILE__, __LINE__, "expected FALSE: " #cond); return; } } while (0)

#define TEST_ASSERT_NULL(ptr) \
    do { if ((ptr) != NULL) { unity_fail(__FILE__, __LINE__, "expected NULL: " #ptr); return; } } while (0)

#define TEST_ASSERT_NOT_NULL(ptr) \
    do { if ((ptr) == NULL) { unity_fail(__FILE__, __LINE__, "expected non-NULL: " #ptr); return; } } while (0)

#define TEST_ASSERT_EQUAL_INT(expected, actual) \
    do { if ((long)(expected) != (long)(actual)) { \
        char _m[128]; (void)snprintf(_m, sizeof(_m), \
        "expected %ld got %ld", (long)(expected), (long)(actual)); \
        unity_fail(__FILE__, __LINE__, _m); return; } } while (0)

#define TEST_ASSERT_EQUAL_UINT(expected, actual) \
    TEST_ASSERT_EQUAL_INT((long)(expected), (long)(actual))

#define TEST_ASSERT_EQUAL(expected, actual) \
    TEST_ASSERT_EQUAL_INT((long)(expected), (long)(actual))

#define TEST_ASSERT_EQUAL_size_t(expected, actual) \
    TEST_ASSERT_EQUAL_INT((long)(expected), (long)(actual))

#define TEST_ASSERT_EQUAL_HEX8(expected, actual) \
    do { if ((unsigned)(expected & 0xFFu) != (unsigned)(actual & 0xFFu)) { \
        char _m[128]; (void)snprintf(_m, sizeof(_m), \
        "expected 0x%02X got 0x%02X", (unsigned)(expected) & 0xFFu, \
        (unsigned)(actual) & 0xFFu); \
        unity_fail(__FILE__, __LINE__, _m); return; } } while (0)

#define TEST_ASSERT_EQUAL_STRING(expected, actual) \
    do { if (strcmp((expected), (actual)) != 0) { \
        unity_fail(__FILE__, __LINE__, "string mismatch"); return; } } while (0)

#define TEST_ASSERT_INT_WITHIN(delta, expected, actual) \
    do { long _d = (long)(actual) - (long)(expected); \
        if (_d < 0) { _d = -_d; } \
        if (_d > (long)(delta)) { \
        unity_fail(__FILE__, __LINE__, "value out of tolerance"); return; } \
    } while (0)

/* setUp / tearDown are optional; weak defaults provided in unity.c. */
void setUp(void);
void tearDown(void);

#ifdef __cplusplus
}
#endif

#endif /* UNITY_H */
