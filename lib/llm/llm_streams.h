#ifndef BASH_LLM_STREAMS_H
#define BASH_LLM_STREAMS_H

#include <stdio.h>

/* Whether tool output (fd 1) should be displayed. 1 = show (default), 0 = hidden */
extern int streams_verbose;

/*
 * Suppress tool output during LLM agentic loops.
 * When set, stream_tool_output() is silenced unless debug mode is on.
 */
extern int streams_llm_active;

/*
 * Label mode:
 *   0 = off (default)
 *   1 = labels: prefix lines with [chat], [stdout], [think], [tool], [api]
 *   2 = debug: labels + raw API request/response data
 */
extern int streams_label_mode;

/* Initialize streams */
void llm_streams_init(void);

/* Cleanup */
void llm_streams_cleanup(void);

/* Write tool output to stdout (only if verbose) */
void stream_tool_output(const char *text);

/* Write LLM chat text to stdout */
void stream_chat_output(const char *text);

/* Write LLM thinking/reasoning text to stderr */
void stream_think_output(const char *text);

/* Write tool call info (name + args) before execution */
void stream_tool_call(const char *name, const char *args);

/* Write man page RAG lookup info */
void stream_man_output(const char *text);

/* Write API debug info (request/response summaries) */
void stream_api_output(const char *direction, const char *text);

/* Write memory keyword search debug info */
void stream_memory_output(const char *text);

/* Write global memory agent debug info */
void stream_global_mem_agent_output(const char *text);

/* Write cron agent debug info */
void stream_cron_agent_output(const char *text);

/* Write side agent framework debug info */
void stream_side_agent_output(const char *name, const char *text);

#endif /* BASH_LLM_STREAMS_H */
