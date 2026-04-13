#ifndef BASH_LLM_GLOBAL_MEM_AGENT_H
#define BASH_LLM_GLOBAL_MEM_AGENT_H

/*
 * Global Memory Agent
 *
 * Sole owner of the user's long-term memory store. All memory access
 * goes through this module -- no other code should include llm_memory.h
 * or call llm_memory_* functions directly.
 *
 * Provides:
 *   - High-level API for user commands (remember, forget, list, cleanup)
 *   - Side-agent callbacks for pre-query search and post-query extraction
 *   - Memory store initialization and lifecycle management
 */

/*
 * Initialize the global memory agent.
 *   memory_dir: path to memory store directory (e.g., ~/.aibash_memories)
 *   memory_max: max entries
 *   api_url/model/api_key: LLM connection for the agent (NULL if no agent LLM)
 *
 * This initializes both the memory store and the agent's LLM connection.
 * Returns number of memories loaded, or -1 on error.
 */
int  llm_global_mem_agent_init(const char *memory_dir, int memory_max,
                               const char *api_url, const char *model,
                               const char *api_key);
void llm_global_mem_agent_cleanup(void);

/* Returns 1 if agent is initialized and LLM is available */
int llm_global_mem_agent_ready(void);

/* Returns number of stored memories */
int llm_global_mem_agent_count(void);

/*
 * User-facing memory commands (called from llm builtin).
 * These are synchronous -- return immediately with results.
 */
int   llm_global_mem_remember(const char *text);        /* save a fact; returns 0 on success */
int   llm_global_mem_forget(int id);                    /* forget by ID; returns 0 on success */
int   llm_global_mem_forget_match(const char *text);    /* forget by text match; returns 0 */
char *llm_global_mem_list(void);                        /* list all; returns malloc'd text */

/*
 * Run memory cleanup (split compound entries, deduplicate, resolve conflicts).
 * Requires agent LLM. Called synchronously from "llm cleanup" command.
 */
void llm_global_mem_agent_cleanup_run(int memory_max);

/*
 * Extract facts from a conversation and save to memory.
 * Designed to be called in a forked child process (reinitializes store from disk).
 */
void llm_global_mem_agent_extract(const char *conversation);

/*
 * Side agent callbacks (registered by side_agent_init).
 *
 * pre_query:  searches memories via LLM, returns relevant context.
 *             Called in forked child by the side agent framework.
 *
 * post_query: extracts facts from conversation, saves to memory.
 *             Called in double-forked grandchild by the side agent framework.
 */
char *global_mem_agent_pre_query_cb(const char *query, const char *cwd);
void  global_mem_agent_post_query_cb(const char *query, const char *response, const char *cwd);

#endif /* BASH_LLM_GLOBAL_MEM_AGENT_H */
