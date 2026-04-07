#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "llm_shell.h"
#include "llm_api.h"
#include "llm_router.h"
#include "llm_history.h"
#include "llm_streams.h"
#include "cJSON.h"

/* Check bash's interrupt_state for Ctrl-C */
extern volatile sig_atomic_t interrupt_state;

/* Helper to update ctx->last_output */
static void set_last_output(llm_shell_ctx_t *ctx, char *output)
{
    free(ctx->last_output);
    ctx->last_output = output;
}

/* ---- Agentic loop ---- */

char *llm_shell_agentic_loop(llm_shell_ctx_t *ctx, llm_response_t *resp)
{
    char cwd[4096];
    int iterations = 0;
    char *final_text = NULL;

    while (resp && resp->num_tool_calls > 0
           && !interrupt_state && iterations < ctx->max_iterations) {
        iterations++;

        /* Stream any assistant text before tool calls */
        if (resp->text && resp->text[0])
            stream_chat_output("\n");

        /* Record assistant message with tool_calls (OpenAI protocol) */
        llm_history_add_assistant_tool_calls(resp->text,
                                              resp->tool_calls,
                                              resp->num_tool_calls);

        set_last_output(ctx, NULL);

        /* Execute each tool call and record results */
        for (int i = 0; i < resp->num_tool_calls && !interrupt_state; i++) {
            const char *name = resp->tool_calls[i].name;
            if (name) {
                char status[512];
                const char *args = resp->tool_calls[i].arguments;
                if (args && args[0]) {
                    cJSON *a = cJSON_Parse(args);
                    const char *hint = NULL;
                    if (a) {
                        cJSON *child = a->child;
                        while (child) {
                            if (cJSON_IsString(child)) { hint = child->valuestring; break; }
                            if (cJSON_IsArray(child)) {
                                cJSON *first = cJSON_GetArrayItem(child, 0);
                                if (first && cJSON_IsString(first)) { hint = first->valuestring; break; }
                            }
                            child = child->next;
                        }
                        snprintf(status, sizeof(status), "  \xe2\x86\x92 %s %s",
                                 name, hint ? hint : "");
                        cJSON_Delete(a);
                    } else {
                        snprintf(status, sizeof(status), "  \xe2\x86\x92 %s", name);
                    }
                } else {
                    snprintf(status, sizeof(status), "  \xe2\x86\x92 %s", name);
                }
                fprintf(stderr, "\033[90m%s\033[0m\n", status);
                fflush(stderr);
            }

            char *result = llm_router_dispatch(&resp->tool_calls[i]);
            if (result) {
                stream_tool_output(result);
                if (result[0] && result[strlen(result)-1] != '\n')
                    stream_tool_output("\n");
                llm_history_add_tool_result(resp->tool_calls[i].id,
                                             resp->tool_calls[i].name,
                                             result);
                set_last_output(ctx, result);
            }
        }

        llm_response_free(resp);
        resp = NULL;

        if (interrupt_state) break;

        if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, ".");
        resp = llm_chat_stream(NULL, cwd, ctx->last_output, NULL, 0, ctx->cbs);
    }

    if (resp && resp->text && resp->text[0]) {
        stream_chat_output("\n");
        llm_history_add_assistant(resp->text);
        final_text = strdup(resp->text);
    }

    if (iterations >= ctx->max_iterations)
        fprintf(stderr, "bash-llm: max iterations reached (%d)\n", ctx->max_iterations);

    llm_response_free(resp);
    return final_text;
}

/* ---- Query ---- */

char *llm_shell_query(llm_shell_ctx_t *ctx, const char *query, const char *context)
{
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, ".");

    char *full_query;
    if (context) {
        size_t len = strlen(query) + strlen(context) + 64;
        full_query = malloc(len);
        snprintf(full_query, len,
                 "[context]:\n%s\n\n[user request]: %s",
                 context, query);
    } else {
        full_query = strdup(query);
    }

    llm_history_add_user(full_query);
    streams_llm_active = 1;
    llm_response_t *resp = llm_chat_stream(full_query, cwd, ctx->last_output,
                                            NULL, 0, ctx->cbs);
    free(full_query);

    if (!resp) {
        streams_llm_active = 0;
        fprintf(stderr, "bash-llm: LLM request failed\n");
        return NULL;
    }

    char *result = llm_shell_agentic_loop(ctx, resp);
    streams_llm_active = 0;
    return result;
}
