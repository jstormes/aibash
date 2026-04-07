#ifndef LLMSH_SHELL_H
#define LLMSH_SHELL_H

#include <signal.h>
#include "llm.h"
#include "serverconf.h"

/*
 * Shell context — holds all state previously stored in globals.
 * Created by main(), passed to all shell_* functions.
 */
typedef struct {
    server_config_t *servers;
    llm_stream_cbs *cbs;
    char *last_output;      /* last tool output for LLM context */
    int max_iterations;
} shell_ctx_t;

/*
 * Run the agentic loop: execute tool calls, feed results back to the LLM,
 * repeat until the LLM responds with only text or max iterations hit.
 *
 * resp is consumed/freed by this function.
 * Returns: final text response (malloced), or NULL
 */
char *shell_agentic_loop(shell_ctx_t *ctx, llm_response_t *resp);

/*
 * Send a query to the LLM with optional piped context and run the
 * full agentic loop. Returns final text (malloced), or NULL.
 */
char *shell_query(shell_ctx_t *ctx, const char *query, const char *context);

/*
 * Execute a command line: split on |, run command segments directly,
 * hand off to LLM at first non-command segment.
 *
 * Returns: output string (malloced) — either command output or LLM answer
 */
char *shell_execute(shell_ctx_t *ctx, const char *cmdline);

/*
 * Handle a built-in slash command (/server, /clear, /verbose, etc.)
 * Returns 1 if handled, 0 if not a slash command.
 */
int shell_handle_slash(shell_ctx_t *ctx, const char *line);

/*
 * Check if input is an exit command.
 */
int shell_is_exit(const char *line);

/*
 * Parse output redirection from end of input.
 * Looks for " > file" or " >> file" at the end.
 * If found, sets *outfile (malloced) and *append flag,
 * returns a malloced copy of the input with the redirect stripped.
 * If not found, returns NULL and leaves outfile/append unchanged.
 */
char *shell_parse_redirect(const char *input, char **outfile, int *append);

#endif /* LLMSH_SHELL_H */
