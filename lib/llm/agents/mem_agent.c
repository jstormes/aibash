#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mem_agent.h"

/* ---- State ---- */

static int g_store_ready = 0;
static int g_api_ready = 0;
static mem_agent_deps_t g_deps;
static char g_memory_dir[4096];
static int g_memory_max = 200;

/* ---- LLM Prompts ---- */

static const char *SEARCH_PROMPT =
    "Which memories below could be useful for answering the user's query?\n"
    "Include anything related -- preferences, facts, context.\n"
    "When in doubt, include it.\n\n"
    "Copy matching memories as bullet points. If nothing relevant: NONE\n\n"
    "Example:\n"
    "Query: tell me about my setup\n"
    "Memories: [1] User prefers Python [2] User likes cats [3] User deploys to AWS\n"
    "Answer:\n- User prefers Python\n- User deploys to AWS";

static const char *EXTRACT_PROMPT =
    "You are a memory extraction agent. Analyze the conversation and extract "
    "NEW facts the user shared. Output ONLY a valid JSON array.\n\n"
    "RULES:\n"
    "1. Use third person: \"User prefers...\" not \"I prefer...\"\n"
    "2. If nothing NEW worth saving, return: []\n"
    "3. Output RAW JSON only. No markdown, no code fences, no explanation.\n"
    "4. Do NOT extract facts the assistant already stated — those are known.\n"
    "5. Only extract facts the USER explicitly shared or requested.\n\n"
    "FORMAT: [{\"content\": \"...\", \"keywords\": \"comma,separated\"}]\n\n"
    "DO NOT save: command outputs, file contents, directory listings, errors,\n"
    "assistant responses, or facts the assistant mentioned from memory.\n"
    "DO save: NEW name, role, preferences, habits, project details, tech choices.";

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
    "If memories are clean and consistent, return: []\n\n"
    "Output RAW JSON only. No markdown, no code fences, no explanation.";

/* ---- Helpers ---- */

static int is_empty_response(const char *text)
{
    if (!text || !text[0]) return 1;
    const char *p = text;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '\0') return 1;
    if (strncasecmp(p, "NONE", 4) == 0) return 1;
    return 0;
}

static char *build_search_message(const char *query)
{
    char *mem_list = g_deps.mem_list();
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

static char *build_prompt_with_memories(const char *prefix, const char *body)
{
    char *mem_list = g_deps.mem_list();
    int has = mem_list && strcmp(mem_list, "(no memories saved)") != 0;

    size_t len = (prefix ? strlen(prefix) : 0)
               + (body ? strlen(body) : 0)
               + (has ? strlen(mem_list) : 0) + 256;
    char *prompt = malloc(len);

    if (has) {
        snprintf(prompt, len, "Existing memories:\n%s\n%s%s",
                 mem_list, prefix ? prefix : "", body ? body : "");
    } else {
        snprintf(prompt, len, "Existing memories: (none)\n\n%s%s",
                 prefix ? prefix : "", body ? body : "");
    }

    free(mem_list);
    return prompt;
}

/*
 * Parse JSON response and apply operations.
 * Handles: {content, keywords}, {forget}, {add, keywords}
 * Returns number of operations applied.
 *
 * Note: cJSON is a build dependency. For the test binary we need
 * a minimal JSON parser. To keep tests simple, we use a basic
 * hand-parser here instead of requiring cJSON linkage.
 */

/* Minimal JSON array item parser — finds key:value pairs in objects.
 * Not a full parser — handles the specific formats our LLM returns. */
static int apply_operations(const char *json_text)
{
    if (!json_text || !json_text[0]) return 0;

    /* Find the array start */
    const char *p = json_text;
    while (*p && *p != '[') p++;
    if (!*p) return 0;
    p++; /* skip '[' */

    int ops = 0;

    /* Walk through objects in the array */
    while (*p) {
        while (*p && *p != '{') {
            if (*p == ']') return ops;
            p++;
        }
        if (!*p) break;

        /* Find the end of this object */
        const char *obj_start = p;
        int depth = 0;
        const char *obj_end = NULL;
        for (const char *q = p; *q; q++) {
            if (*q == '{') depth++;
            else if (*q == '}') { depth--; if (depth == 0) { obj_end = q; break; } }
        }
        if (!obj_end) break;

        /* Extract key-value pairs from this object */
        size_t obj_len = obj_end - obj_start + 1;
        char *obj = malloc(obj_len + 1);
        memcpy(obj, obj_start, obj_len);
        obj[obj_len] = '\0';

        /* Check for "forget": N */
        char *forget_key = strstr(obj, "\"forget\"");
        if (forget_key) {
            char *colon = strchr(forget_key + 8, ':');
            if (colon) {
                int id = atoi(colon + 1);
                if (id > 0) {
                    g_deps.mem_forget(id);
                    ops++;
                }
            }
            free(obj);
            p = obj_end + 1;
            continue;
        }

        /* Check for "content": "..." or "add": "..." */
        const char *content_key = strstr(obj, "\"content\"");
        const char *add_key = strstr(obj, "\"add\"");
        const char *text_key = content_key ? content_key : add_key;

        if (text_key) {
            /* Find the value string */
            char *colon = strchr(text_key + (content_key ? 9 : 5), ':');
            if (colon) {
                char *quote1 = strchr(colon, '"');
                if (quote1) {
                    quote1++;
                    /* Find closing quote (handle escaped quotes) */
                    char *quote2 = quote1;
                    while (*quote2 && !(*quote2 == '"' && *(quote2 - 1) != '\\'))
                        quote2++;
                    if (*quote2) {
                        size_t vlen = quote2 - quote1;
                        char *value = malloc(vlen + 1);
                        memcpy(value, quote1, vlen);
                        value[vlen] = '\0';

                        /* Look for keywords */
                        char *kw_key = strstr(obj, "\"keywords\"");
                        char *keywords = NULL;
                        if (kw_key) {
                            char *kc = strchr(kw_key + 10, ':');
                            if (kc) {
                                char *kq1 = strchr(kc, '"');
                                if (kq1) {
                                    kq1++;
                                    char *kq2 = strchr(kq1, '"');
                                    if (kq2) {
                                        keywords = malloc(kq2 - kq1 + 1);
                                        memcpy(keywords, kq1, kq2 - kq1);
                                        keywords[kq2 - kq1] = '\0';
                                    }
                                }
                            }
                        }

                        /* Handle replaces */
                        char *rep_key = strstr(obj, "\"replaces\"");
                        if (rep_key) {
                            char *rc = strchr(rep_key + 10, ':');
                            if (rc) {
                                int rid = atoi(rc + 1);
                                if (rid > 0) g_deps.mem_forget(rid);
                            }
                        }

                        g_deps.mem_save(value, keywords);
                        ops++;
                        free(value);
                        free(keywords);
                    }
                }
            }
        }

        free(obj);
        p = obj_end + 1;
    }

    return ops;
}

/* ---- Init / cleanup ---- */

int mem_agent_init_with_deps(void *config, const mem_agent_deps_t *deps)
{
    if (!deps) return -1;
    g_deps = *deps;

    /*
     * The config is opaque to the framework. The agent knows its structure.
     * We access memory_enabled, memory_max, memory_api_url, etc.
     * In production this is server_config_t. In tests it's test_config_t.
     * Both have the same field layout for the fields we access.
     */
    typedef struct {
        int memory_enabled;
        int memory_max;
        char *memory_api_url;
        char *memory_model;
        char *memory_api_key;
    } mem_config_t;

    if (!config) return -1;
    mem_config_t *cfg = (mem_config_t *)config;

    if (!cfg->memory_enabled) return -1;

    /* Initialize memory store */
    const char *home = getenv("HOME");
    snprintf(g_memory_dir, sizeof(g_memory_dir),
             "%s/.aibash_memories", home ? home : ".");
    g_memory_max = cfg->memory_max;

    g_deps.mem_init(g_memory_dir, g_memory_max);
    g_store_ready = 1;

    /* Initialize logging if available */
    if (g_deps.log_init) {
        char logdir[4200];
        snprintf(logdir, sizeof(logdir), "%s/logs", g_memory_dir);
        g_deps.log_init(logdir);
    }

    /* Initialize LLM API if configured */
    if (cfg->memory_api_url && g_deps.api_init) {
        g_deps.api_init(cfg->memory_api_url, cfg->memory_model, cfg->memory_api_key);
        g_api_ready = 1;
    }

    return 0;
}

/* Production dependencies — linked from libllm.a in production,
 * absent in test binary (test uses mem_agent_init_with_deps instead) */
#ifndef AGENT_TESTING
extern int   llm_memory_init(const char *, int);
extern void  llm_memory_cleanup(void);
extern int   llm_memory_save(const char *, const char *);
extern int   llm_memory_forget(int);
extern int   llm_memory_forget_match(const char *);
extern char *llm_memory_list(void);
extern int   llm_memory_count(void);
extern int   llm_global_mem_api_init(const char *, const char *, const char *);
extern void  llm_global_mem_api_cleanup(void);
extern char *llm_global_mem_api_chat(const char *, const char *, int, const char *);
extern void  llm_log_init(const char *);
#endif

int mem_agent_init(void *config)
{
#ifdef AGENT_TESTING
    return -1;  /* tests use mem_agent_init_with_deps() */
#else
    mem_agent_deps_t deps = {
        .mem_init         = llm_memory_init,
        .mem_cleanup      = llm_memory_cleanup,
        .mem_save         = llm_memory_save,
        .mem_forget       = llm_memory_forget,
        .mem_forget_match = llm_memory_forget_match,
        .mem_list         = llm_memory_list,
        .mem_count        = llm_memory_count,
        .api_init         = llm_global_mem_api_init,
        .api_cleanup      = llm_global_mem_api_cleanup,
        .api_chat         = llm_global_mem_api_chat,
        .log_init         = llm_log_init,
    };

    return mem_agent_init_with_deps(config, &deps);
#endif
}

void mem_agent_cleanup(void)
{
    if (g_store_ready && g_deps.mem_cleanup)
        g_deps.mem_cleanup();
    if (g_api_ready && g_deps.api_cleanup)
        g_deps.api_cleanup();
    g_store_ready = 0;
    g_api_ready = 0;
}

/* ---- User commands ---- */

int mem_agent_remember(const char *text)
{
    if (!g_store_ready || !text) return -1;
    return g_deps.mem_save(text, NULL);
}

int mem_agent_forget(int id)
{
    if (!g_store_ready) return -1;
    return g_deps.mem_forget(id);
}

int mem_agent_forget_match(const char *text)
{
    if (!g_store_ready || !text) return -1;
    return g_deps.mem_forget_match(text);
}

char *mem_agent_list(void)
{
    if (!g_store_ready) return strdup("(memory not initialized)");
    return g_deps.mem_list();
}

int mem_agent_count(void)
{
    return g_store_ready ? g_deps.mem_count() : 0;
}

int mem_agent_ready(void)
{
    return g_api_ready;
}

/* ---- Side agent callbacks ---- */

char *mem_agent_pre_query(const char *query, const char *cwd)
{
    (void)cwd;
    if (!g_api_ready || !query || !query[0]) return NULL;

    char *msg = build_search_message(query);
    if (!msg) return NULL;

    char *result = g_deps.api_chat(SEARCH_PROMPT, msg, 0, "mem-search");
    free(msg);

    if (is_empty_response(result)) {
        free(result);
        return NULL;
    }

    return result;
}

void mem_agent_post_query(const char *query, const char *response, const char *cwd)
{
    (void)cwd;
    if (!g_api_ready || !g_store_ready) return;

    size_t conv_len = strlen(query) + strlen(response) + 64;
    char *conversation = malloc(conv_len);
    snprintf(conversation, conv_len, "User: %s\nAssistant: %s", query, response);

    /* Pass 1: Fast extraction (thinking OFF)
     * Only send the conversation — NOT existing memories.
     * Including memories causes the model to re-extract known facts
     * from the assistant's response (which echoes memory context). */
    {
        char *resp = g_deps.api_chat(EXTRACT_PROMPT, conversation, 0, "extract");

        if (resp) {
            apply_operations(resp);
            free(resp);
        }
    }

    /* Pass 2: Cleanup (thinking ON) — only if memories exist */
    if (g_deps.mem_count() > 1) {
        /* Reload from disk in forked child for fresh state */
        if (g_deps.mem_cleanup && g_deps.mem_init) {
            g_deps.mem_cleanup();
            g_deps.mem_init(g_memory_dir, g_memory_max);
        }

        char *prompt = build_prompt_with_memories(NULL, NULL);
        char *resp = g_deps.api_chat(CLEANUP_PROMPT, prompt, 1, "cleanup");
        free(prompt);

        if (resp) {
            apply_operations(resp);
            free(resp);
        }
    }

    free(conversation);
}

void mem_agent_run_cleanup(void)
{
    if (!g_api_ready || !g_store_ready) return;
    mem_agent_post_query("(cleanup requested by user)", "", "/tmp");
}
