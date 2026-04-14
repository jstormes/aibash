#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "test_harness.h"
#include "../calendar_agent.h"

/* ---- Mock event store ---- */

#define MOCK_CAL_MAX 32

static struct {
    int id;
    char *summary;
    char *dtstart;
    char *dtend;
    char *description;
} mock_events[MOCK_CAL_MAX];
static int mock_event_count = 0;
static int mock_next_id = 1;
static char mock_store_path[256];

static int mock_store_load(const char *path)
{
    /* In tests, we load from a simple text file if it exists */
    snprintf(mock_store_path, sizeof(mock_store_path), "%s", path);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    mock_event_count = 0;
    mock_next_id = 1;
    char line[1024];
    while (fgets(line, sizeof(line), f) && mock_event_count < MOCK_CAL_MAX) {
        char *sum = NULL, *start = NULL, *end = NULL, *desc = NULL;
        int id = 0;
        if (sscanf(line, "%d|", &id) == 1) {
            char *p = strchr(line, '|') + 1;
            sum = strtok(p, "|"); start = strtok(NULL, "|");
            end = strtok(NULL, "|"); desc = strtok(NULL, "|\n");
            if (sum && start) {
                mock_events[mock_event_count].id = id;
                mock_events[mock_event_count].summary = strdup(sum);
                mock_events[mock_event_count].dtstart = strdup(start);
                mock_events[mock_event_count].dtend = end ? strdup(end) : strdup("");
                mock_events[mock_event_count].description = desc ? strdup(desc) : strdup("");
                if (id >= mock_next_id) mock_next_id = id + 1;
                mock_event_count++;
            }
        }
    }
    fclose(f);
    return mock_event_count;
}

static int mock_store_save(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    for (int i = 0; i < mock_event_count; i++) {
        fprintf(f, "%d|%s|%s|%s|%s\n",
                mock_events[i].id, mock_events[i].summary,
                mock_events[i].dtstart, mock_events[i].dtend,
                mock_events[i].description);
    }
    fclose(f);
    return 0;
}

static int mock_store_add(const char *summary, const char *dtstart,
                          const char *dtend, const char *description)
{
    if (mock_event_count >= MOCK_CAL_MAX) return -1;
    mock_events[mock_event_count].id = mock_next_id++;
    mock_events[mock_event_count].summary = strdup(summary);
    mock_events[mock_event_count].dtstart = strdup(dtstart);
    mock_events[mock_event_count].dtend = dtend ? strdup(dtend) : strdup("");
    mock_events[mock_event_count].description = description ? strdup(description) : strdup("");
    mock_event_count++;
    if (mock_store_path[0]) mock_store_save(mock_store_path);
    return mock_events[mock_event_count - 1].id;
}

static int mock_store_remove(int id)
{
    for (int i = 0; i < mock_event_count; i++) {
        if (mock_events[i].id == id) {
            free(mock_events[i].summary);
            free(mock_events[i].dtstart);
            free(mock_events[i].dtend);
            free(mock_events[i].description);
            for (int j = i; j < mock_event_count - 1; j++)
                mock_events[j] = mock_events[j + 1];
            mock_event_count--;
            if (mock_store_path[0]) mock_store_save(mock_store_path);
            return 0;
        }
    }
    return -1;
}

static char *mock_store_list(void)
{
    if (mock_event_count == 0) return strdup("(no calendar events)\n");
    char *buf = malloc(4096);
    size_t off = 0;
    for (int i = 0; i < mock_event_count; i++) {
        off += snprintf(buf + off, 4096 - off, "[%d] %s: %s (%s - %s)\n",
                        mock_events[i].id, mock_events[i].dtstart,
                        mock_events[i].summary,
                        mock_events[i].dtstart, mock_events[i].dtend);
    }
    return buf;
}

static int mock_store_count(void) { return mock_event_count; }

static void mock_store_cleanup(void)
{
    for (int i = 0; i < mock_event_count; i++) {
        free(mock_events[i].summary);
        free(mock_events[i].dtstart);
        free(mock_events[i].dtend);
        free(mock_events[i].description);
    }
    mock_event_count = 0;
    mock_next_id = 1;
    mock_store_path[0] = 0;
}

/* ---- Mock LLM API ---- */

static char *mock_llm_response = NULL;

static char *mock_api_chat(const char *sys, const char *user,
                           int think, const char *caller)
{
    (void)sys; (void)user; (void)think; (void)caller;
    return mock_llm_response ? strdup(mock_llm_response) : NULL;
}

/* ---- Deps ---- */

static calendar_agent_deps_t mock_deps = {
    .api_chat       = mock_api_chat,
    .store_load     = mock_store_load,
    .store_save     = mock_store_save,
    .store_add      = mock_store_add,
    .store_remove   = mock_store_remove,
    .store_list     = mock_store_list,
    .store_count    = mock_store_count,
    .store_cleanup  = mock_store_cleanup,
};

/* ---- Temp dir helper ---- */

static char g_tmpdir[256];

static void make_tmpdir(void)
{
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/aibash_cal_test_%d", getpid());
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
    calendar_agent_cleanup();
    mock_store_cleanup();
    mock_llm_response = NULL;
    rm_tmpdir();
}

/* ---- Tests: init/cleanup ---- */

static int test_init_creates_dir(void)
{
    reset();
    make_tmpdir();
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/calstore", g_tmpdir);

    int rc = calendar_agent_init_with_deps(dir, &mock_deps);
    TEST_ASSERT_INT_EQ(rc, 0);
    TEST_ASSERT_INT_EQ(calendar_agent_ready(), 1);
    TEST_ASSERT_INT_EQ(calendar_agent_count(), 0);

    struct stat st;
    TEST_ASSERT(stat(dir, &st) == 0, "storage dir should be created");

    reset();
    return 0;
}

static int test_cleanup_resets(void)
{
    reset();
    make_tmpdir();
    calendar_agent_init_with_deps(g_tmpdir, &mock_deps);
    calendar_agent_add("Standup", "2026-04-15 10:00", "2026-04-15 10:30", "Daily standup");
    TEST_ASSERT_INT_EQ(calendar_agent_count(), 1);

    calendar_agent_cleanup();
    TEST_ASSERT_INT_EQ(calendar_agent_count(), 0);
    TEST_ASSERT_INT_EQ(calendar_agent_ready(), 0);

    rm_tmpdir();
    return 0;
}

/* ---- Tests: CRUD ---- */

static int test_add_and_list(void)
{
    reset();
    make_tmpdir();
    calendar_agent_init_with_deps(g_tmpdir, &mock_deps);

    int id = calendar_agent_add("Team standup", "2026-04-15 10:00", "2026-04-15 10:30", "Daily sync");
    TEST_ASSERT(id > 0, "add should return positive id");
    TEST_ASSERT_INT_EQ(calendar_agent_count(), 1);

    char *list = calendar_agent_list();
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_STR_CONTAINS(list, "Team standup");
    free(list);

    reset();
    return 0;
}

static int test_add_multiple(void)
{
    reset();
    make_tmpdir();
    calendar_agent_init_with_deps(g_tmpdir, &mock_deps);

    calendar_agent_add("Standup", "2026-04-15 10:00", "2026-04-15 10:30", "");
    calendar_agent_add("Lunch", "2026-04-15 12:00", "2026-04-15 13:00", "");
    TEST_ASSERT_INT_EQ(calendar_agent_count(), 2);

    char *list = calendar_agent_list();
    TEST_ASSERT_STR_CONTAINS(list, "Standup");
    TEST_ASSERT_STR_CONTAINS(list, "Lunch");
    free(list);

    reset();
    return 0;
}

static int test_remove_event(void)
{
    reset();
    make_tmpdir();
    calendar_agent_init_with_deps(g_tmpdir, &mock_deps);

    int id1 = calendar_agent_add("A", "2026-04-15 09:00", "2026-04-15 10:00", "");
    calendar_agent_add("B", "2026-04-15 11:00", "2026-04-15 12:00", "");

    int rc = calendar_agent_remove(id1);
    TEST_ASSERT_INT_EQ(rc, 0);
    TEST_ASSERT_INT_EQ(calendar_agent_count(), 1);

    char *list = calendar_agent_list();
    TEST_ASSERT_STR_CONTAINS(list, "B");
    free(list);

    reset();
    return 0;
}

static int test_remove_nonexistent(void)
{
    reset();
    make_tmpdir();
    calendar_agent_init_with_deps(g_tmpdir, &mock_deps);

    int rc = calendar_agent_remove(999);
    TEST_ASSERT_INT_EQ(rc, -1);

    reset();
    return 0;
}

static int test_empty_list(void)
{
    reset();
    make_tmpdir();
    calendar_agent_init_with_deps(g_tmpdir, &mock_deps);

    char *list = calendar_agent_list();
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_STR_CONTAINS(list, "no calendar events");
    free(list);

    reset();
    return 0;
}

/* ---- Tests: persistence ---- */

static int test_events_persist(void)
{
    reset();
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/aibash_cal_persist_%d", getpid());
    mkdir(tmpdir, 0755);

    calendar_agent_init_with_deps(tmpdir, &mock_deps);
    calendar_agent_add("Persist test", "2026-04-15 14:00", "2026-04-15 15:00", "test");
    TEST_ASSERT_INT_EQ(calendar_agent_count(), 1);

    calendar_agent_cleanup();
    mock_store_cleanup();
    calendar_agent_init_with_deps(tmpdir, &mock_deps);
    TEST_ASSERT_INT_EQ(calendar_agent_count(), 1);

    char *list = calendar_agent_list();
    TEST_ASSERT_STR_CONTAINS(list, "Persist test");
    free(list);

    calendar_agent_cleanup();
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    system(cmd);
    return 0;
}

/* ---- Tests: pre_query ---- */

static int test_pre_query_yes_injects(void)
{
    reset();
    make_tmpdir();
    calendar_agent_init_with_deps(g_tmpdir, &mock_deps);
    calendar_agent_add("Standup", "2026-04-15 10:00", "2026-04-15 10:30", "Daily");
    calendar_agent_add("Dentist", "2026-04-16 14:00", "2026-04-16 15:00", "Checkup");

    mock_llm_response = "YES";
    char *result = calendar_agent_pre_query("what meetings do I have", "/tmp");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_STR_CONTAINS(result, "Standup");
    TEST_ASSERT_STR_CONTAINS(result, "Dentist");
    free(result);

    reset();
    return 0;
}

static int test_pre_query_no_skips(void)
{
    reset();
    make_tmpdir();
    calendar_agent_init_with_deps(g_tmpdir, &mock_deps);
    calendar_agent_add("Standup", "2026-04-15 10:00", "2026-04-15 10:30", "");

    mock_llm_response = "NO";
    char *result = calendar_agent_pre_query("list files in tmp", "/tmp");
    TEST_ASSERT_NULL(result);

    reset();
    return 0;
}

static int test_pre_query_no_events_null(void)
{
    reset();
    make_tmpdir();
    calendar_agent_init_with_deps(g_tmpdir, &mock_deps);

    char *result = calendar_agent_pre_query("any meetings", "/tmp");
    TEST_ASSERT_NULL(result);

    reset();
    return 0;
}

/* ---- Tests: post_query ---- */

static int test_post_query_creates_event(void)
{
    reset();
    make_tmpdir();
    calendar_agent_init_with_deps(g_tmpdir, &mock_deps);

    mock_llm_response =
        "[{\"summary\":\"Team lunch\",\"start\":\"2026-04-15 12:00\","
        "\"end\":\"2026-04-15 13:00\",\"description\":\"Pizza\"}]";
    calendar_agent_post_query("schedule a team lunch tomorrow at noon", "Done!", "/tmp");

    TEST_ASSERT_INT_EQ(calendar_agent_count(), 1);
    char *list = calendar_agent_list();
    TEST_ASSERT_STR_CONTAINS(list, "Team lunch");
    free(list);

    reset();
    return 0;
}

static int test_post_query_empty_json(void)
{
    reset();
    make_tmpdir();
    calendar_agent_init_with_deps(g_tmpdir, &mock_deps);

    mock_llm_response = "[]";
    calendar_agent_post_query("hello", "hi", "/tmp");
    TEST_ASSERT_INT_EQ(calendar_agent_count(), 0);

    reset();
    return 0;
}

static int test_post_query_null(void)
{
    reset();
    make_tmpdir();
    calendar_agent_init_with_deps(g_tmpdir, &mock_deps);

    mock_llm_response = NULL;
    calendar_agent_post_query("hello", "hi", "/tmp");
    TEST_ASSERT_INT_EQ(calendar_agent_count(), 0);

    reset();
    return 0;
}

/* ---- Test runner ---- */

void run_calendar_agent_tests(void)
{
    fprintf(stderr, "\ncalendar_agent tests:\n");

    RUN_TEST(test_init_creates_dir);
    RUN_TEST(test_cleanup_resets);

    RUN_TEST(test_add_and_list);
    RUN_TEST(test_add_multiple);
    RUN_TEST(test_remove_event);
    RUN_TEST(test_remove_nonexistent);
    RUN_TEST(test_empty_list);

    RUN_TEST(test_events_persist);

    RUN_TEST(test_pre_query_yes_injects);
    RUN_TEST(test_pre_query_no_skips);
    RUN_TEST(test_pre_query_no_events_null);

    RUN_TEST(test_post_query_creates_event);
    RUN_TEST(test_post_query_empty_json);
    RUN_TEST(test_post_query_null);
}
