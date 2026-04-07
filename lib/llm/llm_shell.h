#ifndef BASH_LLM_SHELL_H
#define BASH_LLM_SHELL_H

#include "llm_api.h"
#include "llm_serverconf.h"

/*
 * Shell context -- holds all state for the LLM subsystem.
 */
typedef struct {
    server_config_t *servers;
    llm_stream_cbs *cbs;
    char *last_output;      /* last tool output for LLM context */
    int max_iterations;
} llm_shell_ctx_t;

/*
 * Run the agentic loop: execute tool calls, feed results back to the LLM,
 * repeat until the LLM responds with only text or max iterations hit.
 *
 * resp is consumed/freed by this function.
 * Returns: final text response (malloced), or NULL
 */
char *llm_shell_agentic_loop(llm_shell_ctx_t *ctx, llm_response_t *resp);

/*
 * Send a query to the LLM with optional piped context and run the
 * full agentic loop. Returns final text (malloced), or NULL.
 */
char *llm_shell_query(llm_shell_ctx_t *ctx, const char *query, const char *context);

#endif /* BASH_LLM_SHELL_H */
