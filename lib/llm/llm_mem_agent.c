#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm_mem_agent.h"
#include "llm_mem_api.h"
#include "llm_memory.h"
#include "llm_streams.h"
#include "cJSON.h"

static int g_agent_ready = 0;

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

int llm_mem_agent_init(const char *api_url, const char *model, const char *api_key)
{
    if (!api_url) return -1;
    llm_mem_api_init(api_url, model, api_key);
    g_agent_ready = 1;
    return 0;
}

void llm_mem_agent_cleanup(void)
{
    llm_mem_api_cleanup();
    g_agent_ready = 0;
}

int llm_mem_agent_ready(void)
{
    return g_agent_ready;
}

/* ---- Helpers ---- */

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
            stream_mem_agent_output(dbg);
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
            stream_mem_agent_output(dbg);
            continue;
        }
    }

    cJSON_Delete(arr);
    return ops;
}

/* ---- Main extraction entry point ---- */

void llm_mem_agent_extract(const char *conversation, const char *memory_dir, int memory_max)
{
    if (!g_agent_ready || !conversation || !conversation[0]) return;

    /*
     * We're in a forked child process. Reinitialize the memory store
     * from disk since we have a copy-on-write snapshot of the parent.
     */
    llm_memory_init(memory_dir, memory_max);

    /*
     * PASS 1: Fast extraction (thinking OFF)
     * Quick scan of the conversation, extract obvious facts.
     */
    {
        char *prompt = build_prompt_with_memories("Conversation:\n", conversation);
        char *response = llm_mem_api_chat(EXTRACT_PROMPT, prompt, 0);
        free(prompt);

        if (response) {
            apply_operations(response);
            free(response);
        }
    }

    /*
     * PASS 2: Cleanup (thinking ON)
     * Reload memories (pass 1 may have added new ones), then ask the
     * model to split compound entries, resolve conflicts, remove dupes.
     * Thinking mode lets it reason through the logic step by step.
     */
    {
        /* Reload from disk to see pass 1 results */
        llm_memory_cleanup();
        llm_memory_init(memory_dir, memory_max);

        /* Only run cleanup if there are memories to clean */
        if (llm_memory_count() > 1) {
            char *prompt = build_prompt_with_memories(NULL, NULL);
            char *response = llm_mem_api_chat(CLEANUP_PROMPT, prompt, 1);
            free(prompt);

            if (response) {
                apply_operations(response);
                free(response);
            }
        }
    }
}
