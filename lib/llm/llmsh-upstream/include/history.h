#ifndef LLMSH_HISTORY_H
#define LLMSH_HISTORY_H

#include "llm.h"

/* Manage conversation history for LLM context */

void history_init(void);
void history_cleanup(void);

void history_add_user(const char *text);
void history_add_assistant(const char *text);

/*
 * Add an assistant message that includes tool calls.
 * This records the tool_calls array so the API protocol is correct.
 */
void history_add_assistant_tool_calls(const char *text,
                                       const tool_call_t *tool_calls,
                                       int num_tool_calls);

/*
 * Add a tool result message (role: "tool" with tool_call_id).
 */
void history_add_tool_result(const char *tool_call_id,
                              const char *tool_name,
                              const char *result);

/* Build the messages JSON array for the API call. Caller frees. */
char *history_build_messages(const char *system_prompt);

#endif /* LLMSH_HISTORY_H */
