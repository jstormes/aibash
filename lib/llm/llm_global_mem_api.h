#ifndef BASH_LLM_GLOBAL_MEM_API_H
#define BASH_LLM_GLOBAL_MEM_API_H

/*
 * Separate LLM API client for the global memory agent.
 * Has its own connection state independent of the main agent's llm_api.c.
 * Non-streaming, no tool calls -- just simple prompt -> text response.
 */

int  llm_global_mem_api_init(const char *api_url, const char *model, const char *api_key);
void llm_global_mem_api_cleanup(void);

/*
 * Send a system+user message to the global memory agent LLM (blocking).
 * enable_thinking: 0 = fast direct output, 1 = allow reasoning chain
 * log_caller: identifier for log files (e.g., "mem-search", "extract", "cleanup")
 * Returns malloced response text, or NULL on error.
 */
char *llm_global_mem_api_chat(const char *system_prompt, const char *user_message,
                              int enable_thinking, const char *log_caller);

#endif /* BASH_LLM_GLOBAL_MEM_API_H */
