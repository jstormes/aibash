#ifndef BASH_LLM_SIDE_AGENT_H
#define BASH_LLM_SIDE_AGENT_H

#include "llm_serverconf.h"

/*
 * Side Agent Framework
 *
 * Each side agent has four callbacks:
 *   init       — initialize the agent (called once at startup)
 *   pre_query  — search for context before the main LLM call
 *   post_query — background work after the main LLM responds
 *   cleanup    — free resources on shutdown
 *
 * The framework handles fork/pipe/select plumbing.
 * Agents are defined in a static table — add new ones to llm_side_agent.c.
 */

#define SIDE_AGENT_MAX             8
#define SIDE_AGENT_DEFAULT_TIMEOUT 5
#define SIDE_AGENT_BUF_SIZE        4096

typedef struct {
    const char *name;       /* "global_memory", "cron", etc. */
    int timeout_sec;        /* pre_query timeout; 0 = use default (5s) */
    int enabled;            /* set by init callback; 1 = active */

    /*
     * Init: called once at startup with the server config.
     * The agent reads what it needs from config, sets up storage,
     * and sets enabled=1 if ready. Returns 0 on success.
     */
    int (*init)(server_config_t *config);

    /* Cleanup: free resources. */
    void (*cleanup)(void);

    /* Pre-query: called in forked child. Returns malloc'd string or NULL. */
    char *(*pre_query)(const char *query, const char *cwd);

    /* Post-query: called in double-forked grandchild. Fire-and-forget. */
    void (*post_query)(const char *query, const char *response, const char *cwd);
} side_agent_t;

/*
 * Initialize the side-agent framework. Calls init() on each built-in
 * agent. This is the ONLY init call the builtins need to make.
 */
void side_agent_init(server_config_t *config);

/*
 * Cleanup all agents.
 */
void side_agent_cleanup(void);

/*
 * Run all enabled pre-query agents (serial fork/pipe/select).
 * Returns malloc'd combined context string, or NULL. Caller frees.
 */
char *side_agents_pre_query(const char *query, const char *cwd);

/*
 * Run all enabled post-query agents (double-fork, fire-and-forget).
 */
void side_agents_post_query(const char *query, const char *response, const char *cwd);

/*
 * Number of registered agents.
 */
int side_agent_count(void);

#endif /* BASH_LLM_SIDE_AGENT_H */
