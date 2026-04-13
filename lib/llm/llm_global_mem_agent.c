#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "llm_global_mem_agent.h"
#include "llm_global_mem_api.h"
#include "llm_memory.h"
#include "llm_streams.h"
#include "llm_log.h"
#include "llm_serverconf.h"
#include "cJSON.h"

/* Forward declaration */
void llm_global_mem_agent_extract(const char *conversation);

static int g_agent_ready = 0;    /* LLM available for search/extract */
static int g_store_ready = 0;    /* memory store initialized */
static char g_memory_dir[4096];  /* path to memory directory */
static int g_memory_max = 200;   /* max entries */

/* ---- Memory search prompt ---- */

static const char *SEARCH_PROMPT =
    "Which memories below could be useful for answering the user's query?\n"
    "Include anything related -- preferences, facts, context.\n"
    "When in doubt, include it.\n\n"
    "Copy matching memories as bullet points. If nothing relevant: NONE\n\n"
    "Example:\n"
    "Query: tell me about my setup\n"
    "Memories: [1] User prefers Python [2] User likes cats [3] User deploys to AWS\n"
    "Answer:\n- User prefers Python\n- User deploys to AWS";

/* ---- Pass 1: Fast extraction (thinking OFF) ---- */

static const char *EXTRACT_PROMPT =
    "You are a memory extraction agent. Analyze the conversation and extract "
    "facts worth remembering. Output ONLY a valid JSON array.\n\n"
    "RULES:\n"
    "1. Use third person: \"User prefers...\" not \"I prefer...\"\n"
    "2. If nothing worth saving, return: []\n"
    "3. Output RAW JSON only. No markdown, no code fences, no explanation.\n"
    "4. If the user asks to forget something, use {\"forget\": ID}\n\n"
    "FORMAT: [{\"content\": \"...\", \"keywords\": \"comma,separated\"}]\n\n"
    "DO NOT save: command outputs, file contents, directory listings, errors.\n"
    "DO save: name, role, preferences, habits, project details, tech choices.";

/* ---- Pass 2: Cleanup (thinking ON) ---- */

static const char *CLEANUP_PROMPT =
    "You are a memory cleanup agent. Review the current memories and fix "
    "any issues. Output ONLY a valid JSON array of operations.\n\n"
    "TASKS:\n"
    "1. SPLIT compound entries into individual facts\n"
    "2. REPLACE outdated memories when newer info exists\n"
    "3. REMOVE duplicates\n\n"
    "OPERATIONS:\n"
    "  {\"add\": \"new fact text\", \"keywords\": \"...\"}\n"
    "  {\"forget\": ID}\n\n"
    "To split a compound memory [5] into parts: forget 5, then add each part.\n"
    "To replace outdated [3] with new info: forget 3, then add the correction.\n"
    "If memories are clean and consistent, return: []\n\n"
    "EXAMPLE - Given memories:\n"
    "  [1] User's name is James\n"
    "  [2] User prefers Python, uses neovim, and deploys to AWS\n"
    "  [3] User switched from neovim to helix editor\n\n"
    "Output:\n"
    "[\n"
    "  {\"forget\": 2},\n"
    "  {\"forget\": 3},\n"
    "  {\"add\": \"User prefers Python for scripting\", \"keywords\": \"python,scripting\"},\n"
    "  {\"add\": \"User uses helix as editor\", \"keywords\": \"helix,editor\"},\n"
    "  {\"add\": \"User deploys to AWS\", \"keywords\": \"aws,deploy,cloud\"}\n"
    "]\n\n"
    "Output RAW JSON only. No markdown, no code fences, no explanation.";

/* ---- Init/cleanup ---- */

int global_mem_agent_init(server_config_t *config)
{
    if (!config || !config->memory_enabled) return -1;

    const char *home = getenv("HOME");
    char memdir[4096];
    snprintf(memdir, sizeof(memdir), "%s/.aibash_memories", home ? home : ".");

    snprintf(g_memory_dir, sizeof(g_memory_dir), "%s", memdir);
    g_memory_max = config->memory_max;
    llm_memory_init(memdir, config->memory_max);
    g_store_ready = 1;

    /* Initialize API logging */
    char logdir[4096];
    snprintf(logdir, sizeof(logdir), "%s/logs", memdir);
    llm_log_init(logdir);

    /* Initialize the agent LLM connection (optional) */
    if (config->memory_api_url) {
        llm_global_mem_api_init(config->memory_api_url,
                                config->memory_model,
                                config->memory_api_key);
        g_agent_ready = 1;
    }

    return 0;
}

void global_mem_agent_cleanup(void)
{
    llm_memory_cleanup();
    llm_global_mem_api_cleanup();
    g_agent_ready = 0;
    g_store_ready = 0;
}

int llm_global_mem_agent_ready(void)
{
    return g_agent_ready;
}

int llm_global_mem_agent_count(void)
{
    return g_store_ready ? llm_memory_count() : 0;
}

/* ---- User-facing memory commands ---- */

int llm_global_mem_remember(const char *text)
{
    if (!g_store_ready || !text) return -1;
    return llm_memory_save(text, NULL);
}

int llm_global_mem_forget(int id)
{
    if (!g_store_ready) return -1;
    return llm_memory_forget(id);
}

int llm_global_mem_forget_match(const char *text)
{
    if (!g_store_ready || !text) return -1;
    return llm_memory_forget_match(text);
}

char *llm_global_mem_list(void)
{
    if (!g_store_ready) return strdup("(memory not initialized)");
    return llm_memory_list();
}

/* ---- Internal helpers ---- */

static char *build_prompt_with_memories(const char *prefix, const char *body)
{
    char *mem_list = llm_memory_list();
    int has_memories = mem_list && strcmp(mem_list, "(no memories saved)") != 0;

    size_t len = (prefix ? strlen(prefix) : 0)
               + (body ? strlen(body) : 0)
               + (has_memories ? strlen(mem_list) : 0) + 256;
    char *prompt = malloc(len);

    if (has_memories) {
        snprintf(prompt, len, "Existing memories:\n%s\n%s%s",
                 mem_list,
                 prefix ? prefix : "",
                 body ? body : "");
    } else {
        snprintf(prompt, len, "Existing memories: (none)\n\n%s%s",
                 prefix ? prefix : "",
                 body ? body : "");
    }

    free(mem_list);
    return prompt;
}

/*
 * Parse JSON response and apply operations.
 * Supports: {content, keywords}, {content, keywords, replaces}, {forget}, {add, keywords}
 */
static int apply_operations(const char *json_text)
{
    if (!json_text || !json_text[0]) return 0;

    /* Find the JSON array -- skip any leading text */
    const char *start = json_text;
    while (*start && *start != '[') start++;
    if (!*start) return 0;

    cJSON *arr = cJSON_Parse(start);
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return 0;
    }

    int n = cJSON_GetArraySize(arr);
    int ops = 0;

    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!item) continue;

        /* Handle forget */
        cJSON *j_forget = cJSON_GetObjectItem(item, "forget");
        if (j_forget && cJSON_IsNumber(j_forget)) {
            llm_memory_forget(j_forget->valueint);
            ops++;
            continue;
        }

        /* Handle add (from cleanup pass) */
        cJSON *j_add = cJSON_GetObjectItem(item, "add");
        if (j_add && cJSON_IsString(j_add)) {
            cJSON *j_kw = cJSON_GetObjectItem(item, "keywords");
            const char *kw = (j_kw && cJSON_IsString(j_kw)) ? j_kw->valuestring : NULL;
            llm_memory_save(j_add->valuestring, kw);
            ops++;

            char dbg[512];
            snprintf(dbg, sizeof(dbg), "cleanup: %s\n", j_add->valuestring);
            stream_global_mem_agent_output(dbg);
            continue;
        }

        /* Handle content (from extraction pass) */
        cJSON *j_content = cJSON_GetObjectItem(item, "content");
        if (j_content && cJSON_IsString(j_content)) {
            /* Handle replaces */
            cJSON *j_replaces = cJSON_GetObjectItem(item, "replaces");
            if (j_replaces && cJSON_IsNumber(j_replaces))
                llm_memory_forget(j_replaces->valueint);

            cJSON *j_kw = cJSON_GetObjectItem(item, "keywords");
            const char *kw = (j_kw && cJSON_IsString(j_kw)) ? j_kw->valuestring : NULL;
            llm_memory_save(j_content->valuestring, kw);
            ops++;

            char dbg[512];
            snprintf(dbg, sizeof(dbg), "extracted: %s\n", j_content->valuestring);
            stream_global_mem_agent_output(dbg);
            continue;
        }
    }

    cJSON_Delete(arr);
    return ops;
}

/* ---- Memory search helpers ---- */

static char *build_search_message(const char *query)
{
    char *mem_list = llm_memory_list();
    if (!mem_list || strcmp(mem_list, "(no memories saved)") == 0) {
        free(mem_list);
        return NULL;
    }

    size_t len = strlen(query) + strlen(mem_list) + 64;
    char *msg = malloc(len);
    snprintf(msg, len, "Query: %s\n\nMemories:\n%s", query, mem_list);
    free(mem_list);
    return msg;
}

static int is_empty_response(const char *text)
{
    if (!text || !text[0]) return 1;

    const char *p = text;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    if (*p == '\0') return 1;
    if (strncasecmp(p, "NONE", 4) == 0) return 1;

    return 0;
}

/* ---- Side agent callbacks ---- */

char *global_mem_agent_pre_query_cb(const char *query, const char *cwd)
{
    (void)cwd;
    if (!query || !query[0]) return NULL;

    char *agent_msg = build_search_message(query);
    if (!agent_msg) return NULL;

    /* Debug: verify API state in forked child */
    fprintf(stderr, "[mem-debug] agent_ready=%d, msg_len=%zu\n",
            g_agent_ready, strlen(agent_msg));
    fflush(stderr);

    stream_global_mem_agent_output("searching memories...\n");

    char *result = llm_global_mem_api_chat(SEARCH_PROMPT, agent_msg, 0, "mem-search");
    free(agent_msg);

    fprintf(stderr, "[mem-debug] LLM result: %.200s\n", result ? result : "(NULL)");
    fflush(stderr);

    if (is_empty_response(result)) {
        free(result);
        return NULL;
    }

    stream_global_mem_agent_output(result);
    return result;
}

void global_mem_agent_post_query_cb(const char *query, const char *response, const char *cwd)
{
    (void)cwd;
    if (!g_agent_ready) return;

    size_t conv_len = strlen(query) + strlen(response) + 64;
    char *conversation = malloc(conv_len);
    snprintf(conversation, conv_len, "User: %s\nAssistant: %s", query, response);

    llm_global_mem_agent_extract(conversation);
    free(conversation);
}

/* ---- Extraction (runs in forked child) ---- */

static void run_extraction(const char *conversation)
{
    /*
     * PASS 1: Fast extraction (thinking OFF)
     */
    {
        char *prompt = build_prompt_with_memories("Conversation:\n", conversation);
        char *response = llm_global_mem_api_chat(EXTRACT_PROMPT, prompt, 0, "extract");
        free(prompt);

        if (response) {
            apply_operations(response);
            free(response);
        }
    }

    /*
     * PASS 2: Cleanup (thinking ON)
     * Reload memories (pass 1 may have added new ones).
     */
    {
        llm_memory_cleanup();
        llm_memory_init(g_memory_dir, g_memory_max);

        if (llm_memory_count() > 1) {
            char *prompt = build_prompt_with_memories(NULL, NULL);
            char *response = llm_global_mem_api_chat(CLEANUP_PROMPT, prompt, 1, "cleanup");
            free(prompt);

            if (response) {
                apply_operations(response);
                free(response);
            }
        }
    }
}

void llm_global_mem_agent_extract(const char *conversation)
{
    if (!g_agent_ready || !conversation || !conversation[0]) return;

    /*
     * We're in a forked child process. Reinitialize the memory store
     * from disk since we have a copy-on-write snapshot of the parent.
     */
    llm_memory_cleanup();
    llm_memory_init(g_memory_dir, g_memory_max);

    run_extraction(conversation);
}

void llm_global_mem_agent_cleanup_run(int memory_max)
{
    if (!g_agent_ready) return;

    /* Run extraction with a cleanup trigger (no real conversation) */
    llm_global_mem_agent_extract("(cleanup requested by user)");
}
