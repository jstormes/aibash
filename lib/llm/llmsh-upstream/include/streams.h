#ifndef LLMSH_STREAMS_H
#define LLMSH_STREAMS_H

#include <stdio.h>

#define LLMSH_FD_CHAT 3

/* Global FILE* for chat output (fd 3) */
extern FILE *stdchat;

/* Whether tool output (fd 1) should be displayed. 1 = show (default), 0 = hidden */
extern int streams_verbose;

/*
 * Suppress tool output during LLM agentic loops.
 * When set, stream_tool_output() is silenced unless debug mode is on.
 * Direct shell execution should clear this flag.
 */
extern int streams_llm_active;

/*
 * Label mode:
 *   0 = off (default)
 *   1 = labels: prefix lines with [chat], [stdout], [think], [tool], [api]
 *   2 = debug: labels + raw API request/response data
 */
extern int streams_label_mode;

/* Initialize streams: set up fd 3 for chat output */
void streams_init(void);

/* Cleanup */
void streams_cleanup(void);

/* Write tool output to stdout (only if verbose) */
void stream_tool_output(const char *text);

/* Write LLM chat text to stdchat (fd 3) */
void stream_chat_output(const char *text);

/* Write LLM thinking/reasoning text to stdchat (fd 3) */
void stream_think_output(const char *text);

/* Write tool call info (name + args) before execution */
void stream_tool_call(const char *name, const char *args);

/* Write man page RAG lookup info */
void stream_man_output(const char *text);

/* Write API debug info (request/response summaries) */
void stream_api_output(const char *direction, const char *text);

#endif /* LLMSH_STREAMS_H */
