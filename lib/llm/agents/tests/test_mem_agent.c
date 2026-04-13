#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_harness.h"
#include "../mem_agent.h"

/* ---- Mock memory store ---- */

#define MOCK_MEM_MAX 64

static struct {
    int id;
    char *content;
    char *keywords;
} mock_memories[MOCK_MEM_MAX];
static int mock_mem_count = 0;
static int mock_mem_next_id = 1;

static int mock_mem_init(const char *dir, int max)
{
    (void)dir; (void)max;
    mock_mem_count = 0;
    mock_mem_next_id = 1;
    return 0;
}

static void mock_mem_cleanup(void)
{
    for (int i = 0; i < mock_mem_count; i++) {
        free(mock_memories[i].content);
        free(mock_memories[i].keywords);
    }
    mock_mem_count = 0;
    mock_mem_next_id = 1;
}

static int mock_mem_save(const char *content, const char *keywords)
{
    if (mock_mem_count >= MOCK_MEM_MAX) return -1;
    mock_memories[mock_mem_count].id = mock_mem_next_id++;
    mock_memories[mock_mem_count].content = strdup(content);
    mock_memories[mock_mem_count].keywords = keywords ? strdup(keywords) : NULL;
    mock_mem_count++;
    return 0;
}

static int mock_mem_forget(int id)
{
    for (int i = 0; i < mock_mem_count; i++) {
        if (mock_memories[i].id == id) {
            free(mock_memories[i].content);
            free(mock_memories[i].keywords);
            for (int j = i; j < mock_mem_count - 1; j++)
                mock_memories[j] = mock_memories[j + 1];
            mock_mem_count--;
            return 0;
        }
    }
    return -1;
}

static int mock_mem_forget_match(const char *text)
{
    int found = 0;
    for (int i = mock_mem_count - 1; i >= 0; i--) {
        if (strstr(mock_memories[i].content, text)) {
            free(mock_memories[i].content);
            free(mock_memories[i].keywords);
            for (int j = i; j < mock_mem_count - 1; j++)
                mock_memories[j] = mock_memories[j + 1];
            mock_mem_count--;
            found = 1;
        }
    }
    return found ? 0 : -1;
}

static char *mock_mem_list(void)
{
    if (mock_mem_count == 0) return strdup("(no memories saved)");

    char *buf = malloc(4096);
    size_t off = 0;
    for (int i = 0; i < mock_mem_count; i++) {
        off += snprintf(buf + off, 4096 - off, "[%d] %s\n",
                        mock_memories[i].id, mock_memories[i].content);
    }
    return buf;
}

static int mock_mem_count_fn(void)
{
    return mock_mem_count;
}

/* ---- Mock LLM API ---- */

static char *mock_llm_response = NULL;
static int mock_api_init_called = 0;

static int mock_api_init(const char *url, const char *model, const char *key)
{
    (void)url; (void)model; (void)key;
    mock_api_init_called = 1;
    return 0;
}

static void mock_api_cleanup(void) { }

static char *mock_api_chat(const char *system_prompt, const char *user_message,
                           int enable_thinking, const char *log_caller)
{
    (void)system_prompt; (void)user_message;
    (void)enable_thinking; (void)log_caller;
    return mock_llm_response ? strdup(mock_llm_response) : NULL;
}

/* ---- Mock logging ---- */

static void mock_log_init(const char *dir) { (void)dir; }

/* ---- Deps struct ---- */

static mem_agent_deps_t mock_deps = {
    .mem_init      = mock_mem_init,
    .mem_cleanup   = mock_mem_cleanup,
    .mem_save      = mock_mem_save,
    .mem_forget    = mock_mem_forget,
    .mem_forget_match = mock_mem_forget_match,
    .mem_list      = mock_mem_list,
    .mem_count     = mock_mem_count_fn,
    .api_init      = mock_api_init,
    .api_cleanup   = mock_api_cleanup,
    .api_chat      = mock_api_chat,
    .log_init      = mock_log_init,
};

/* ---- Fake config ---- */

/* The agent casts void* to its config type. For tests we provide
 * a minimal struct with the fields the agent reads. */
typedef struct {
    int memory_enabled;
    int memory_max;
    char *memory_api_url;
    char *memory_model;
    char *memory_api_key;
} test_config_t;

static test_config_t test_config = {
    .memory_enabled = 1,
    .memory_max = 100,
    .memory_api_url = "http://test:8080/v1/chat/completions",
    .memory_model = "test-model",
    .memory_api_key = NULL,
};

/* ---- Helper ---- */

static void reset(void)
{
    mem_agent_cleanup();
    mock_mem_cleanup();
    mock_llm_response = NULL;
    mock_api_init_called = 0;
}

/* ---- Tests: init/cleanup ---- */

static int test_init_success(void)
{
    reset();
    int rc = mem_agent_init_with_deps(&test_config, &mock_deps);
    TEST_ASSERT_INT_EQ(rc, 0);
    TEST_ASSERT_INT_EQ(mem_agent_ready(), 1);
    TEST_ASSERT(mock_api_init_called, "API init should be called");
    reset();
    return 0;
}

static int test_init_disabled(void)
{
    reset();
    test_config_t disabled = test_config;
    disabled.memory_enabled = 0;
    int rc = mem_agent_init_with_deps(&disabled, &mock_deps);
    TEST_ASSERT_INT_EQ(rc, -1);
    TEST_ASSERT_INT_EQ(mem_agent_ready(), 0);
    reset();
    return 0;
}

static int test_init_no_api_url(void)
{
    reset();
    test_config_t no_url = test_config;
    no_url.memory_api_url = NULL;
    int rc = mem_agent_init_with_deps(&no_url, &mock_deps);
    /* Should still init storage, just not the LLM */
    TEST_ASSERT_INT_EQ(rc, 0);
    TEST_ASSERT_INT_EQ(mem_agent_ready(), 0);
    reset();
    return 0;
}

/* ---- Tests: user commands ---- */

static int test_remember_and_list(void)
{
    reset();
    mem_agent_init_with_deps(&test_config, &mock_deps);

    mem_agent_remember("User prefers Python");
    mem_agent_remember("User deploys to AWS");

    char *list = mem_agent_list();
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_STR_CONTAINS(list, "Python");
    TEST_ASSERT_STR_CONTAINS(list, "AWS");
    TEST_ASSERT_INT_EQ(mem_agent_count(), 2);
    free(list);

    reset();
    return 0;
}

static int test_forget_by_id(void)
{
    reset();
    mem_agent_init_with_deps(&test_config, &mock_deps);

    mem_agent_remember("first");
    mem_agent_remember("second");

    int rc = mem_agent_forget(1);
    TEST_ASSERT_INT_EQ(rc, 0);
    TEST_ASSERT_INT_EQ(mem_agent_count(), 1);

    char *list = mem_agent_list();
    TEST_ASSERT_STR_CONTAINS(list, "second");
    free(list);

    reset();
    return 0;
}

static int test_forget_by_match(void)
{
    reset();
    mem_agent_init_with_deps(&test_config, &mock_deps);

    mem_agent_remember("User likes Python");
    mem_agent_remember("User likes Rust");

    int rc = mem_agent_forget_match("Python");
    TEST_ASSERT_INT_EQ(rc, 0);
    TEST_ASSERT_INT_EQ(mem_agent_count(), 1);

    char *list = mem_agent_list();
    TEST_ASSERT_STR_CONTAINS(list, "Rust");
    free(list);

    reset();
    return 0;
}

static int test_forget_nonexistent(void)
{
    reset();
    mem_agent_init_with_deps(&test_config, &mock_deps);

    int rc = mem_agent_forget(999);
    TEST_ASSERT_INT_EQ(rc, -1);

    reset();
    return 0;
}

/* ---- Tests: pre_query ---- */

static int test_pre_query_returns_llm_result(void)
{
    reset();
    mem_agent_init_with_deps(&test_config, &mock_deps);
    mem_agent_remember("User prefers Python");

    mock_llm_response = "- User prefers Python";
    char *result = mem_agent_pre_query("what language", "/tmp");

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_STR_CONTAINS(result, "Python");
    free(result);

    reset();
    return 0;
}

static int test_pre_query_empty_store_null(void)
{
    reset();
    mem_agent_init_with_deps(&test_config, &mock_deps);

    /* No memories saved */
    char *result = mem_agent_pre_query("anything", "/tmp");
    TEST_ASSERT_NULL(result);

    reset();
    return 0;
}

static int test_pre_query_llm_returns_none(void)
{
    reset();
    mem_agent_init_with_deps(&test_config, &mock_deps);
    mem_agent_remember("something");

    mock_llm_response = "NONE";
    char *result = mem_agent_pre_query("unrelated", "/tmp");
    TEST_ASSERT_NULL(result);

    reset();
    return 0;
}

static int test_pre_query_llm_returns_null(void)
{
    reset();
    mem_agent_init_with_deps(&test_config, &mock_deps);
    mem_agent_remember("something");

    mock_llm_response = NULL;
    char *result = mem_agent_pre_query("anything", "/tmp");
    TEST_ASSERT_NULL(result);

    reset();
    return 0;
}

/* ---- Tests: post_query ---- */

static int test_post_query_extracts_memories(void)
{
    reset();
    mem_agent_init_with_deps(&test_config, &mock_deps);

    mock_llm_response = "[{\"content\":\"User likes cats\",\"keywords\":\"cats\"}]";
    mem_agent_post_query("I like cats", "Noted!", "/tmp");

    /* The extraction should have saved the memory */
    TEST_ASSERT_INT_EQ(mem_agent_count(), 1);

    char *list = mem_agent_list();
    TEST_ASSERT_STR_CONTAINS(list, "cats");
    free(list);

    reset();
    return 0;
}

static int test_post_query_empty_json(void)
{
    reset();
    mem_agent_init_with_deps(&test_config, &mock_deps);

    mock_llm_response = "[]";
    mem_agent_post_query("hello", "hi", "/tmp");

    TEST_ASSERT_INT_EQ(mem_agent_count(), 0);

    reset();
    return 0;
}

static int test_post_query_forget_operation(void)
{
    reset();
    mem_agent_init_with_deps(&test_config, &mock_deps);
    mem_agent_remember("old fact");

    mock_llm_response = "[{\"forget\":1}]";
    mem_agent_post_query("forget that", "ok", "/tmp");

    TEST_ASSERT_INT_EQ(mem_agent_count(), 0);

    reset();
    return 0;
}

/* ---- Test runner ---- */

void run_mem_agent_tests(void)
{
    fprintf(stderr, "\nmem_agent tests:\n");

    /* Init/cleanup */
    RUN_TEST(test_init_success);
    RUN_TEST(test_init_disabled);
    RUN_TEST(test_init_no_api_url);

    /* User commands */
    RUN_TEST(test_remember_and_list);
    RUN_TEST(test_forget_by_id);
    RUN_TEST(test_forget_by_match);
    RUN_TEST(test_forget_nonexistent);

    /* Pre-query */
    RUN_TEST(test_pre_query_returns_llm_result);
    RUN_TEST(test_pre_query_empty_store_null);
    RUN_TEST(test_pre_query_llm_returns_none);
    RUN_TEST(test_pre_query_llm_returns_null);

    /* Post-query */
    RUN_TEST(test_post_query_extracts_memories);
    RUN_TEST(test_post_query_empty_json);
    RUN_TEST(test_post_query_forget_operation);
}
