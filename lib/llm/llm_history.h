#ifndef BASH_LLM_HISTORY_H
#define BASH_LLM_HISTORY_H

#include "llm_api.h"

/* Manage conversation history for LLM context */

/*
 * Initialize history. If history_file is not NULL, loads persisted
 * messages from that file (JSON array). Pass NULL to disable persistence.
 */
void llm_history_init(const char *history_file);
void llm_history_cleanup(void);

void llm_history_add_user(const char *text);
void llm_history_add_assistant(const char *text);

/*
 * Add an assistant message that includes tool calls.
 * This records the tool_calls array so the API protocol is correct.
 */
void llm_history_add_assistant_tool_calls(const char *text,
                                           const tool_call_t *tool_calls,
                                           int num_tool_calls);

/*
 * Add a tool result message (role: "tool" with tool_call_id).
 */
void llm_history_add_tool_result(const char *tool_call_id,
                                  const char *tool_name,
                                  const char *result);

/* Build the messages JSON array for the API call. Caller frees. */
char *llm_history_build_messages(const char *system_prompt);

#endif /* BASH_LLM_HISTORY_H */
