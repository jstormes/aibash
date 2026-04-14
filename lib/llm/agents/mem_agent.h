#ifndef MEM_AGENT_H
#define MEM_AGENT_H

/*
 * Global Memory Agent
 *
 * Manages the user's long-term memory. Implements side_agent_t callbacks
 * and exposes user-facing commands (remember, forget, list, cleanup).
 *
 * Dependencies (injected via function pointers for testability):
 *   - Memory store: save, forget, list, count, init, cleanup
 *   - LLM API: chat function for search/extract/cleanup passes
 */

#include "side_agent.h"

/*
 * Function pointer types for dependencies.
 * Production code sets these to real implementations.
 * Tests set them to mocks.
 */
typedef char *(*mem_api_chat_fn)(const char *system_prompt,
                                 const char *user_message,
                                 int enable_thinking,
                                 const char *log_caller);

typedef struct {
    /* Memory store */
    int   (*mem_init)(const char *dir, int max);
    void  (*mem_cleanup)(void);
    int   (*mem_save)(const char *content, const char *keywords);
    int   (*mem_forget)(int id);
    int   (*mem_forget_match)(const char *text);
    char *(*mem_list)(void);
    int   (*mem_count)(void);

    /* LLM API */
    int   (*api_init)(const char *url, const char *model, const char *key);
    void  (*api_cleanup)(void);
    mem_api_chat_fn api_chat;

    /* Logging (optional, can be NULL) */
    void  (*log_init)(const char *dir);
} mem_agent_deps_t;

/*
 * Initialize the memory agent with explicit dependencies.
 * storage_dir: path to memory storage (e.g., ~/.aibash_memories)
 * memory_max: max entries
 * deps: function pointers for storage, LLM, logging
 * Returns 0 on success, -1 on failure.
 */
int mem_agent_init_with_deps(const char *storage_dir, int memory_max,
                             const mem_agent_deps_t *deps);

/* Side agent callbacks */
int   mem_agent_init(void *config);      /* uses production deps */
void  mem_agent_cleanup(void);
char *mem_agent_pre_query(const char *query, const char *cwd);
void  mem_agent_post_query(const char *query, const char *response, const char *cwd);

/* User commands */
int   mem_agent_remember(const char *text);
int   mem_agent_forget(int id);
int   mem_agent_forget_match(const char *text);
char *mem_agent_list(void);
int   mem_agent_count(void);
int   mem_agent_ready(void);
void  mem_agent_run_cleanup(void);

#endif /* MEM_AGENT_H */
