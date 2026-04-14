#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "test_harness.h"
#include "../cron_agent.h"

/* ---- Mock LLM API ---- */

static char *mock_llm_response = NULL;

static char *mock_api_chat(const char *system_prompt, const char *user_message,
                           int enable_thinking, const char *log_caller)
{
    (void)system_prompt; (void)user_message;
    (void)enable_thinking; (void)log_caller;
    return mock_llm_response ? strdup(mock_llm_response) : NULL;
}

/* ---- Deps (no system cron/at) ---- */

static cron_agent_deps_t mock_deps = {
    .api_chat      = mock_api_chat,
    .crontab_sync  = NULL,
    .at_create     = NULL,
    .at_remove     = NULL,
};

/* ---- Temp directory helper ---- */

static char g_tmpdir[256];

static void make_tmpdir(void)
{
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/aibash_cron_test_%d", getpid());
    mkdir(g_tmpdir, 0755);
}

static void rm_tmpdir(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_tmpdir);
    system(cmd);
}

static void reset(void)
{
    cron_agent_cleanup();
    mock_llm_response = NULL;
    rm_tmpdir();
}

/* ---- Tests: init/cleanup ---- */

static int test_init_creates_dir(void)
{
    reset();
    make_tmpdir();
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/cronstore", g_tmpdir);

    int rc = cron_agent_init_with_deps(dir, &mock_deps);
    TEST_ASSERT_INT_EQ(rc, 0);
    TEST_ASSERT_INT_EQ(cron_agent_ready(), 1);
    TEST_ASSERT_INT_EQ(cron_agent_count(), 0);

    struct stat st;
    TEST_ASSERT(stat(dir, &st) == 0, "storage dir should be created");

    reset();
    return 0;
}

static int test_cleanup_resets(void)
{
    reset();
    make_tmpdir();
    cron_agent_init_with_deps(g_tmpdir, &mock_deps);
    cron_agent_add("cron", "0 3 * * *", "/tmp/test.sh", "test job");
    TEST_ASSERT_INT_EQ(cron_agent_count(), 1);

    cron_agent_cleanup();
    TEST_ASSERT_INT_EQ(cron_agent_count(), 0);
    TEST_ASSERT_INT_EQ(cron_agent_ready(), 0);

    rm_tmpdir();
    return 0;
}

/* ---- Tests: add/remove/list ---- */

static int test_add_and_list(void)
{
    reset();
    make_tmpdir();
    cron_agent_init_with_deps(g_tmpdir, &mock_deps);

    int id = cron_agent_add("cron", "0 3 * * *", "/tmp/cleanup.sh", "Daily cleanup");
    TEST_ASSERT(id > 0, "add should return positive id");
    TEST_ASSERT_INT_EQ(cron_agent_count(), 1);

    char *list = cron_agent_list();
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_STR_CONTAINS(list, "Daily cleanup");
    TEST_ASSERT_STR_CONTAINS(list, "Every day at 3:00");
    free(list);

    reset();
    return 0;
}

static int test_add_multiple(void)
{
    reset();
    make_tmpdir();
    cron_agent_init_with_deps(g_tmpdir, &mock_deps);

    cron_agent_add("cron", "0 3 * * *", "/tmp/a.sh", "job A");
    cron_agent_add("at", "2026-12-25 08:00", "echo hi", "holiday");
    TEST_ASSERT_INT_EQ(cron_agent_count(), 2);

    char *list = cron_agent_list();
    TEST_ASSERT_STR_CONTAINS(list, "job A");
    TEST_ASSERT_STR_CONTAINS(list, "holiday");
    free(list);

    reset();
    return 0;
}

static int test_remove_job(void)
{
    reset();
    make_tmpdir();
    cron_agent_init_with_deps(g_tmpdir, &mock_deps);

    int id1 = cron_agent_add("cron", "0 3 * * *", "/tmp/a.sh", "job A");
    cron_agent_add("cron", "0 6 * * *", "/tmp/b.sh", "job B");

    int rc = cron_agent_remove(id1);
    TEST_ASSERT_INT_EQ(rc, 0);
    TEST_ASSERT_INT_EQ(cron_agent_count(), 1);

    char *list = cron_agent_list();
    TEST_ASSERT_STR_CONTAINS(list, "job B");
    free(list);

    reset();
    return 0;
}

static int test_remove_nonexistent(void)
{
    reset();
    make_tmpdir();
    cron_agent_init_with_deps(g_tmpdir, &mock_deps);

    int rc = cron_agent_remove(999);
    TEST_ASSERT_INT_EQ(rc, -1);

    reset();
    return 0;
}

static int test_empty_list(void)
{
    reset();
    make_tmpdir();
    cron_agent_init_with_deps(g_tmpdir, &mock_deps);

    char *list = cron_agent_list();
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_STR_CONTAINS(list, "no scheduled tasks");
    free(list);

    reset();
    return 0;
}

/* ---- Tests: persistence ---- */

static int test_jobs_persist(void)
{
    reset();
    make_tmpdir();
    cron_agent_init_with_deps(g_tmpdir, &mock_deps);

    cron_agent_add("cron", "0 3 * * *", "/tmp/cleanup.sh", "Daily cleanup");
    cron_agent_add("at", "2026-12-25 08:00", "echo hi", "holiday reminder");
    TEST_ASSERT_INT_EQ(cron_agent_count(), 2);

    /* Cleanup and re-init — should reload from disk */
    cron_agent_cleanup();
    cron_agent_init_with_deps(g_tmpdir, &mock_deps);

    TEST_ASSERT_INT_EQ(cron_agent_count(), 2);
    char *list = cron_agent_list();
    TEST_ASSERT_STR_CONTAINS(list, "Daily cleanup");
    TEST_ASSERT_STR_CONTAINS(list, "holiday reminder");
    free(list);

    reset();
    return 0;
}

/* ---- Tests: pre_query ---- */

/* pre_query returns all jobs as English — no LLM filtering */
static int test_pre_query_returns_all_jobs(void)
{
    reset();
    make_tmpdir();
    cron_agent_init_with_deps(g_tmpdir, &mock_deps);
    cron_agent_add("cron", "0 3 * * *", "/tmp/cleanup.sh", "Daily cleanup");
    cron_agent_add("at", "2026-12-25 08:00", "echo hi", "Holiday reminder");

    char *result = cron_agent_pre_query("anything", "/tmp");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_STR_CONTAINS(result, "Daily cleanup");
    TEST_ASSERT_STR_CONTAINS(result, "Holiday reminder");
    TEST_ASSERT_STR_CONTAINS(result, "Every day at 3:00");
    free(result);

    reset();
    return 0;
}

static int test_pre_query_no_jobs_null(void)
{
    reset();
    make_tmpdir();
    cron_agent_init_with_deps(g_tmpdir, &mock_deps);

    char *result = cron_agent_pre_query("anything", "/tmp");
    TEST_ASSERT_NULL(result);

    reset();
    return 0;
}

/* pre_query always returns jobs regardless of query content */
static int test_pre_query_always_injects(void)
{
    reset();
    make_tmpdir();
    cron_agent_init_with_deps(g_tmpdir, &mock_deps);
    cron_agent_add("cron", "0 3 * * *", "/tmp/a.sh", "job");

    char *result = cron_agent_pre_query("what is my name", "/tmp");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_STR_CONTAINS(result, "job");
    free(result);

    reset();
    return 0;
}

/* ---- Tests: post_query ---- */

static int test_post_query_extracts_job(void)
{
    reset();
    make_tmpdir();
    cron_agent_init_with_deps(g_tmpdir, &mock_deps);

    mock_llm_response =
        "[{\"type\":\"cron\",\"schedule\":\"0 3 * * *\","
        "\"command\":\"/tmp/cleanup.sh\",\"description\":\"daily cleanup\"}]";

    cron_agent_post_query("run cleanup every day at 3am", "Sure!", "/tmp");

    TEST_ASSERT_INT_EQ(cron_agent_count(), 1);
    char *list = cron_agent_list();
    TEST_ASSERT_STR_CONTAINS(list, "daily cleanup");
    free(list);

    reset();
    return 0;
}

static int test_post_query_empty_json(void)
{
    reset();
    make_tmpdir();
    cron_agent_init_with_deps(g_tmpdir, &mock_deps);

    mock_llm_response = "[]";
    cron_agent_post_query("hello", "hi", "/tmp");
    TEST_ASSERT_INT_EQ(cron_agent_count(), 0);

    reset();
    return 0;
}

static int test_post_query_llm_null(void)
{
    reset();
    make_tmpdir();
    cron_agent_init_with_deps(g_tmpdir, &mock_deps);

    mock_llm_response = NULL;
    cron_agent_post_query("hello", "hi", "/tmp");
    TEST_ASSERT_INT_EQ(cron_agent_count(), 0);

    reset();
    return 0;
}

/* ---- Test runner ---- */

void run_cron_agent_tests(void)
{
    fprintf(stderr, "\ncron_agent tests:\n");

    /* Init/cleanup */
    RUN_TEST(test_init_creates_dir);
    RUN_TEST(test_cleanup_resets);

    /* CRUD */
    RUN_TEST(test_add_and_list);
    RUN_TEST(test_add_multiple);
    RUN_TEST(test_remove_job);
    RUN_TEST(test_remove_nonexistent);
    RUN_TEST(test_empty_list);

    /* Persistence */
    RUN_TEST(test_jobs_persist);

    /* Pre-query */
    RUN_TEST(test_pre_query_returns_all_jobs);
    RUN_TEST(test_pre_query_no_jobs_null);
    RUN_TEST(test_pre_query_always_injects);

    /* Post-query */
    RUN_TEST(test_post_query_extracts_job);
    RUN_TEST(test_post_query_empty_json);
    RUN_TEST(test_post_query_llm_null);
}
