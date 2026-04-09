#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm_mem_agent.h"
#include "llm_mem_api.h"
#include "llm_memory.h"
#include "llm_streams.h"
#include "cJSON.h"

static int g_agent_ready = 0;

static const char *EXTRACT_SYSTEM_PROMPT =
    "You are a memory extraction agent. Analyze the conversation and extract "
    "facts worth remembering. Output ONLY a valid JSON array.\n\n"
    "RULES:\n"
    "1. Split multiple facts into SEPARATE entries (one fact per object)\n"
    "2. Use third person: \"User prefers...\" not \"I prefer...\"\n"
    "3. If a fact contradicts an existing memory, use \"replaces\" with the memory ID\n"
    "4. If the user asks to forget something, use \"forget\" with the memory ID\n"
    "5. If nothing worth saving, return: []\n"
    "6. Output RAW JSON only. No markdown, no code fences, no explanation.\n\n"
    "ENTRY FORMAT:\n"
    "  {\"content\": \"...\", \"keywords\": \"comma,separated\"}\n"
    "  {\"content\": \"...\", \"keywords\": \"...\", \"replaces\": 2}\n"
    "  {\"forget\": 5}\n\n"
    "EXAMPLE - Given existing memories:\n"
    "  [1] User's name is James\n"
    "  [2] User prefers Python for scripting\n"
    "  [3] User uses vim as editor\n\n"
    "Conversation: \"I switched to Rust for scripting and started using helix\"\n\n"
    "Output:\n"
    "[\n"
    "  {\"content\": \"User prefers Rust for scripting\", \"keywords\": \"rust,scripting,language\", \"replaces\": 2},\n"
    "  {\"content\": \"User uses helix as editor\", \"keywords\": \"helix,editor\", \"replaces\": 3}\n"
    "]\n\n"
    "EXAMPLE - User says \"forget that I use vim\":\n"
    "[\n"
    "  {\"forget\": 3}\n"
    "]\n\n"
    "DO NOT save: command outputs, file contents, directory listings, errors, "
    "or things already in existing memories.\n\n"
    "DO save: name, role, preferences, habits, project details, tech choices, "
    "corrections to existing facts.";

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

/*
 * Build the user message containing existing memories + conversation.
 */
static char *build_extraction_prompt(const char *conversation)
{
    /* Get current memories for context */
    char *mem_list = llm_memory_list();

    size_t len = strlen(conversation) + (mem_list ? strlen(mem_list) : 0) + 256;
    char *prompt = malloc(len);

    if (mem_list && strcmp(mem_list, "(no memories saved)") != 0) {
        snprintf(prompt, len,
                 "Existing memories:\n%s\n"
                 "Conversation:\n%s",
                 mem_list, conversation);
    } else {
        snprintf(prompt, len,
                 "Existing memories: (none)\n\n"
                 "Conversation:\n%s",
                 conversation);
    }

    free(mem_list);
    return prompt;
}

/*
 * Parse the JSON response and apply memory operations.
 * Handles: save (content+keywords), replaces (delete old + save new), forget (delete).
 */
static void apply_extractions(const char *json_text)
{
    if (!json_text || !json_text[0]) return;

    /* Find the JSON array -- skip any leading text/whitespace */
    const char *start = json_text;
    while (*start && *start != '[') start++;
    if (!*start) return;

    cJSON *arr = cJSON_Parse(start);
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return;
    }

    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!item) continue;

        /* Handle forget requests */
        cJSON *j_forget = cJSON_GetObjectItem(item, "forget");
        if (j_forget && cJSON_IsNumber(j_forget)) {
            int id = j_forget->valueint;
            llm_memory_forget(id);
            stream_mem_agent_output("forgot memory");
            continue;
        }

        cJSON *j_content = cJSON_GetObjectItem(item, "content");
        if (!j_content || !cJSON_IsString(j_content)) continue;

        /* Handle replacements -- delete old memory first */
        cJSON *j_replaces = cJSON_GetObjectItem(item, "replaces");
        if (j_replaces && cJSON_IsNumber(j_replaces)) {
            llm_memory_forget(j_replaces->valueint);
        }

        /* Save new memory */
        cJSON *j_keywords = cJSON_GetObjectItem(item, "keywords");
        const char *kw = (j_keywords && cJSON_IsString(j_keywords))
                         ? j_keywords->valuestring : NULL;

        llm_memory_save(j_content->valuestring, kw);

        /* Debug output */
        char dbg[512];
        snprintf(dbg, sizeof(dbg), "extracted: %s\n", j_content->valuestring);
        stream_mem_agent_output(dbg);
    }

    cJSON_Delete(arr);
}

void llm_mem_agent_extract(const char *conversation, const char *memory_dir, int memory_max)
{
    if (!g_agent_ready || !conversation || !conversation[0]) return;

    /*
     * We're in a forked child process. Reinitialize the memory store
     * from disk since we have a copy-on-write snapshot of the parent.
     */
    llm_memory_init(memory_dir, memory_max);

    /* Build the prompt with existing memories + conversation */
    char *prompt = build_extraction_prompt(conversation);

    /* Call memory agent LLM */
    char *response = llm_mem_api_chat(EXTRACT_SYSTEM_PROMPT, prompt);
    free(prompt);

    if (!response) return;

    /* Parse and apply extractions */
    apply_extractions(response);
    free(response);
}
