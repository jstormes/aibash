#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>
#include <string.h>

extern int g_tests_run;
extern int g_tests_passed;
extern int g_tests_failed;

#define TEST_ASSERT(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "    FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
        g_tests_failed++; \
        return 1; \
    } \
    g_tests_passed++; \
} while(0)

#define TEST_ASSERT_INT_EQ(a, b) do { \
    int _a = (a), _b = (b); \
    g_tests_run++; \
    if (_a != _b) { \
        fprintf(stderr, "    FAIL %s:%d: %s == %d, expected %s == %d\n", \
                __FILE__, __LINE__, #a, _a, #b, _b); \
        g_tests_failed++; \
        return 1; \
    } \
    g_tests_passed++; \
} while(0)

#define TEST_ASSERT_NOT_NULL(ptr) \
    TEST_ASSERT((ptr) != NULL, #ptr " should not be NULL")

#define TEST_ASSERT_NULL(ptr) \
    TEST_ASSERT((ptr) == NULL, #ptr " should be NULL")

#define TEST_ASSERT_STR_CONTAINS(hay, needle) do { \
    const char *_h = (hay), *_n = (needle); \
    g_tests_run++; \
    if (!_h || !_n || !strstr(_h, _n)) { \
        fprintf(stderr, "    FAIL %s:%d: \"%s\" not found in result\n", \
                __FILE__, __LINE__, _n ? _n : "(null)"); \
        g_tests_failed++; \
        return 1; \
    } \
    g_tests_passed++; \
} while(0)

#define RUN_TEST(fn) do { \
    fprintf(stderr, "  %-40s ", #fn); \
    if (fn() == 0) { fprintf(stderr, "ok\n"); } \
    else { fprintf(stderr, "FAILED\n"); } \
} while(0)

#define TEST_SUMMARY() \
    (fprintf(stderr, "\n  %d assertions: %d passed, %d failed\n", \
             g_tests_run, g_tests_passed, g_tests_failed), \
     g_tests_failed > 0 ? 1 : 0)

#endif /* TEST_HARNESS_H */
