#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "history.h"
#include "config.h"
#include "cJSON.h"

/*
 * Each history entry stores a pre-built cJSON object representing
 * the message. This preserves the full structure including tool_calls
 * arrays and tool-role messages with tool_call_id.
 */
typedef struct {
    cJSON *message;  /* owns the JSON object */
} message_t;

static message_t *messages = NULL;
static int msg_count = 0;
static int msg_cap = 0;
static int msg_max = 0;

void history_init(void)
{
    msg_count = 0;
    msg_max = LLMSH_MAX_HISTORY * 3;
    if (!messages) {
        msg_cap = msg_max;
        messages = calloc(msg_cap, sizeof(message_t));
    }
}

void history_cleanup(void)
{
    for (int i = 0; i < msg_count; i++) {
        cJSON_Delete(messages[i].message);
        messages[i].message = NULL;
    }
    msg_count = 0;
}

static void add_json_message(cJSON *msg)
{
    if (!msg) return;

    /* Evict oldest if at capacity */
    if (msg_count >= msg_max) {
        cJSON_Delete(messages[0].message);
        memmove(&messages[0], &messages[1], (msg_count - 1) * sizeof(message_t));
        msg_count--;
    }

    /* Grow if needed */
    if (msg_count >= msg_cap) {
        msg_cap = (msg_cap == 0) ? 64 : msg_cap * 2;
        messages = realloc(messages, msg_cap * sizeof(message_t));
        if (!messages) { cJSON_Delete(msg); return; }
    }

    messages[msg_count].message = msg;
    msg_count++;
}

void history_add_user(const char *text)
{
    if (!text) return;
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", text);
    add_json_message(msg);
}

void history_add_assistant(const char *text)
{
    if (!text) return;
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "assistant");
    cJSON_AddStringToObject(msg, "content", text);
    add_json_message(msg);
}

void history_add_assistant_tool_calls(const char *text,
                                       const tool_call_t *tool_calls,
                                       int num_tool_calls)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "assistant");

    /* Content may be null when only tool calls are present */
    if (text && text[0])
        cJSON_AddStringToObject(msg, "content", text);
    else
        cJSON_AddNullToObject(msg, "content");

    /* Build tool_calls array */
    cJSON *tcs = cJSON_CreateArray();
    for (int i = 0; i < num_tool_calls; i++) {
        cJSON *tc = cJSON_CreateObject();
        cJSON_AddStringToObject(tc, "id",
            tool_calls[i].id ? tool_calls[i].id : "unknown");
        cJSON_AddStringToObject(tc, "type", "function");

        cJSON *fn = cJSON_CreateObject();
        cJSON_AddStringToObject(fn, "name",
            tool_calls[i].name ? tool_calls[i].name : "unknown");
        cJSON_AddStringToObject(fn, "arguments",
            tool_calls[i].arguments ? tool_calls[i].arguments : "{}");
        cJSON_AddItemToObject(tc, "function", fn);

        cJSON_AddItemToArray(tcs, tc);
    }
    cJSON_AddItemToObject(msg, "tool_calls", tcs);

    add_json_message(msg);
}

void history_add_tool_result(const char *tool_call_id,
                              const char *tool_name,
                              const char *result)
{
    if (!result) return;

    /* Truncate large outputs */
    const char *content = result;
    char *truncated = NULL;
    if (strlen(result) >= LLMSH_MAX_OUTPUT_CAPTURE) {
        truncated = malloc(LLMSH_MAX_OUTPUT_CAPTURE);
        if (truncated) {
            memcpy(truncated, result, LLMSH_MAX_OUTPUT_CAPTURE - 20);
            strcpy(truncated + LLMSH_MAX_OUTPUT_CAPTURE - 20, "\n[truncated]");
            content = truncated;
        }
    }

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "tool");
    cJSON_AddStringToObject(msg, "tool_call_id",
        tool_call_id ? tool_call_id : "unknown");
    if (tool_name)
        cJSON_AddStringToObject(msg, "name", tool_name);
    cJSON_AddStringToObject(msg, "content", content);

    add_json_message(msg);
    free(truncated);
}

char *history_build_messages(const char *system_prompt)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    /* System message first */
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", system_prompt);
    cJSON_AddItemToArray(arr, sys);

    /* Add history — deep copy each stored message */
    for (int i = 0; i < msg_count; i++) {
        if (!messages[i].message) continue;
        cJSON *copy = cJSON_Duplicate(messages[i].message, 1);
        if (copy) cJSON_AddItemToArray(arr, copy);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}
