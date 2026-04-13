#ifndef BASH_LLM_GLOBAL_MEM_AGENT_H
#define BASH_LLM_GLOBAL_MEM_AGENT_H

#include "llm_serverconf.h"

/*
 * Global Memory Agent
 *
 * Sole owner of the user's long-term memory store. All memory access
 * goes through this module.
 */

/* Side agent callbacks (registered by the framework) */
int   global_mem_agent_init(server_config_t *config);
void  global_mem_agent_cleanup(void);
char *global_mem_agent_pre_query_cb(const char *query, const char *cwd);
void  global_mem_agent_post_query_cb(const char *query, const char *response, const char *cwd);

/* User-facing commands (called from llm builtin) */
int   llm_global_mem_remember(const char *text);
int   llm_global_mem_forget(int id);
int   llm_global_mem_forget_match(const char *text);
char *llm_global_mem_list(void);
int   llm_global_mem_agent_count(void);
int   llm_global_mem_agent_ready(void);
void  llm_global_mem_agent_cleanup_run(int memory_max);

#endif /* BASH_LLM_GLOBAL_MEM_AGENT_H */
