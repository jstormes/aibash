#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>

#include "test_harness.h"
#include "../side_agent.h"

/* ---- Mock callbacks ---- */

static int mock_init_ok(void *config)
{
    (void)config;
    return 0;
}

static int mock_init_fail(void *config)
{
    (void)config;
    return -1;
}

static void mock_cleanup(void) { }

static char *mock_pre_query_hello(const char *query, const char *cwd)
{
    (void)query; (void)cwd;
    return strdup("hello from mock");
}

static char *mock_pre_query_a(const char *query, const char *cwd)
{
    (void)query; (void)cwd;
    return strdup("context A");
}

static char *mock_pre_query_b(const char *query, const char *cwd)
{
    (void)query; (void)cwd;
    return strdup("context B");
}

static char *mock_pre_query_null(const char *query, const char *cwd)
{
    (void)query; (void)cwd;
    return NULL;
}

static int g_pre_query_called = 0;
static char *mock_pre_query_tracking(const char *query, const char *cwd)
{
    (void)query; (void)cwd;
    g_pre_query_called = 1;
    return strdup("tracked");
}

static char *mock_pre_query_slow(const char *query, const char *cwd)
{
    (void)query; (void)cwd;
    sleep(10);
    return strdup("too late");
}

static void mock_post_query_sentinel(const char *query, const char *response, const char *cwd)
{
    (void)query; (void)response; (void)cwd;
    /* Write a sentinel file to prove we ran */
    FILE *f = fopen("/tmp/aibash_test_sentinel", "w");
    if (f) { fputs("post_query_ran", f); fclose(f); }
}

static char *mock_pre_query_check_sigpipe(const char *query, const char *cwd)
{
    (void)query; (void)cwd;
    /*
     * Check if SIGPIPE has been reset to SIG_DFL.
     * signal() returns the previous handler. If we set SIG_DFL and get
     * SIG_DFL back, it was already SIG_DFL (correct).
     * If we get SIG_IGN back, the framework did NOT reset it (bug).
     */
    void (*prev)(int) = signal(SIGPIPE, SIG_DFL);
    if (prev == SIG_DFL)
        return strdup("SIGPIPE_OK");
    else
        return strdup("SIGPIPE_BAD");
}

/* ---- Helper ---- */

static void reset_framework(void)
{
    side_agent_cleanup();
}

/* ---- Tests ---- */

static int test_register_and_count(void)
{
    reset_framework();

    side_agent_register(&(side_agent_t){ .name = "a" });
    side_agent_register(&(side_agent_t){ .name = "b" });
    side_agent_register(&(side_agent_t){ .name = "c" });

    TEST_ASSERT_INT_EQ(side_agent_count(), 3);

    reset_framework();
    return 0;
}

static int test_cleanup_resets(void)
{
    reset_framework();

    side_agent_register(&(side_agent_t){ .name = "x" });
    side_agent_register(&(side_agent_t){ .name = "y" });
    TEST_ASSERT_INT_EQ(side_agent_count(), 2);

    side_agent_cleanup();
    TEST_ASSERT_INT_EQ(side_agent_count(), 0);

    return 0;
}

static int test_init_calls_init_cb(void)
{
    reset_framework();

    side_agent_register(&(side_agent_t){
        .name = "good",
        .init = mock_init_ok,
        .cleanup = mock_cleanup,
    });

    side_agent_init(NULL);

    /* After init with success, the agent should be enabled.
     * We verify indirectly: register a pre_query and see if it runs. */
    /* For now just verify count is still 1 (init didn't crash) */
    TEST_ASSERT_INT_EQ(side_agent_count(), 1);

    reset_framework();
    return 0;
}

static int test_init_failed_stays_disabled(void)
{
    reset_framework();

    side_agent_register(&(side_agent_t){
        .name = "bad",
        .init = mock_init_fail,
        .pre_query = mock_pre_query_hello,
    });

    side_agent_init(NULL);

    /* Agent init failed, so pre_query should not run (disabled) */
    char *result = side_agents_pre_query("test", "/tmp");
    TEST_ASSERT_NULL(result);

    reset_framework();
    return 0;
}

static int test_pre_query_returns_result(void)
{
    reset_framework();

    side_agent_register(&(side_agent_t){
        .name = "mock",
        .timeout_sec = 5,
        .enabled = 1,
        .pre_query = mock_pre_query_hello,
    });

    char *result = side_agents_pre_query("test query", "/tmp");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_STR_CONTAINS(result, "## mock");
    TEST_ASSERT_STR_CONTAINS(result, "hello from mock");
    free(result);

    reset_framework();
    return 0;
}

static int test_pre_query_null_no_output(void)
{
    reset_framework();

    side_agent_register(&(side_agent_t){
        .name = "empty",
        .enabled = 1,
        .pre_query = mock_pre_query_null,
    });

    char *result = side_agents_pre_query("test", "/tmp");
    TEST_ASSERT_NULL(result);

    reset_framework();
    return 0;
}

static int test_pre_query_disabled_skipped(void)
{
    reset_framework();
    g_pre_query_called = 0;

    side_agent_register(&(side_agent_t){
        .name = "disabled",
        .enabled = 0,
        .pre_query = mock_pre_query_tracking,
    });

    char *result = side_agents_pre_query("test", "/tmp");
    TEST_ASSERT_NULL(result);
    /* The tracking flag lives in the parent — if the child ran, it would
     * be set in the child's copy only. But since disabled, no fork at all. */

    reset_framework();
    return 0;
}

static int test_pre_query_multiple_agents(void)
{
    reset_framework();

    side_agent_register(&(side_agent_t){
        .name = "agent_a",
        .timeout_sec = 5,
        .enabled = 1,
        .pre_query = mock_pre_query_a,
    });
    side_agent_register(&(side_agent_t){
        .name = "agent_b",
        .timeout_sec = 5,
        .enabled = 1,
        .pre_query = mock_pre_query_b,
    });

    char *result = side_agents_pre_query("test", "/tmp");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_STR_CONTAINS(result, "## agent_a");
    TEST_ASSERT_STR_CONTAINS(result, "context A");
    TEST_ASSERT_STR_CONTAINS(result, "## agent_b");
    TEST_ASSERT_STR_CONTAINS(result, "context B");
    free(result);

    reset_framework();
    return 0;
}

static int test_pre_query_timeout(void)
{
    reset_framework();

    side_agent_register(&(side_agent_t){
        .name = "slow",
        .timeout_sec = 1,
        .enabled = 1,
        .pre_query = mock_pre_query_slow,
    });

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    char *result = side_agents_pre_query("test", "/tmp");
    /* Should timeout and return NULL (or empty) */

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    free(result);  /* may be NULL, free(NULL) is safe */

    /* Should complete in ~1-2 seconds, not 10 */
    TEST_ASSERT(elapsed < 3.0, "pre_query should timeout within 3 seconds");
    TEST_ASSERT(elapsed >= 0.5, "pre_query should wait at least 0.5 seconds");

    reset_framework();
    return 0;
}

static int test_post_query_fires(void)
{
    reset_framework();
    unlink("/tmp/aibash_test_sentinel");

    side_agent_register(&(side_agent_t){
        .name = "poster",
        .enabled = 1,
        .post_query = mock_post_query_sentinel,
    });

    side_agents_post_query("q", "r", "/tmp");

    /* Wait for grandchild to run */
    usleep(200000);  /* 200ms */

    struct stat st;
    int exists = (stat("/tmp/aibash_test_sentinel", &st) == 0);
    unlink("/tmp/aibash_test_sentinel");

    TEST_ASSERT(exists, "post_query should create sentinel file");

    reset_framework();
    return 0;
}

static int test_sigpipe_reset(void)
{
    reset_framework();

    /* Set SIGPIPE to SIG_IGN in parent (simulates bash behavior) */
    signal(SIGPIPE, SIG_IGN);

    side_agent_register(&(side_agent_t){
        .name = "sigtest",
        .timeout_sec = 5,
        .enabled = 1,
        .pre_query = mock_pre_query_check_sigpipe,
    });

    char *result = side_agents_pre_query("test", "/tmp");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_STR_CONTAINS(result, "SIGPIPE_OK");
    free(result);

    /* Restore */
    signal(SIGPIPE, SIG_DFL);

    reset_framework();
    return 0;
}

/* ---- Test runner ---- */

void run_side_agent_tests(void)
{
    fprintf(stderr, "\nside_agent tests:\n");
    RUN_TEST(test_register_and_count);
    RUN_TEST(test_cleanup_resets);
    RUN_TEST(test_init_calls_init_cb);
    RUN_TEST(test_init_failed_stays_disabled);
    RUN_TEST(test_pre_query_returns_result);
    RUN_TEST(test_pre_query_null_no_output);
    RUN_TEST(test_pre_query_disabled_skipped);
    RUN_TEST(test_pre_query_multiple_agents);
    RUN_TEST(test_pre_query_timeout);
    RUN_TEST(test_post_query_fires);
    RUN_TEST(test_sigpipe_reset);
}
