#ifndef BASH_LLM_SIDE_AGENT_H
#define BASH_LLM_SIDE_AGENT_H

/*
 * Side Agent Framework
 *
 * Side agents run alongside the main LLM agent to provide context or
 * perform background work. Each agent registers callbacks:
 *
 *   pre_query  — runs before the main LLM call (forked child, pipe back results)
 *   post_query — runs after the main LLM responds (double-forked, fire-and-forget)
 *
 * The framework handles all fork/pipe/select/reap plumbing.
 * Adding a new agent: write callback(s), register in side_agent_init().
 */

#define SIDE_AGENT_MAX             8
#define SIDE_AGENT_DEFAULT_TIMEOUT 5
#define SIDE_AGENT_BUF_SIZE        4096

typedef struct {
    const char *name;       /* "global_memory", "local_memory", "cron" */
    int timeout_sec;        /* pre_query timeout; 0 = use default (5s) */
    int enabled;            /* runtime toggle; 1 = active */

    /*
     * Pre-query callback: called in a forked child process before the
     * main LLM call. Returns malloc'd context string to inject into the
     * system prompt, or NULL if nothing relevant.
     */
    char *(*pre_query)(const char *query, const char *cwd);

    /*
     * Post-query callback: called in a double-forked grandchild after
     * the main LLM responds. Fire-and-forget — no return value.
     * Results are persisted to disk by the callback.
     */
    void (*post_query)(const char *query, const char *response, const char *cwd);
} side_agent_t;

/*
 * Register a side agent. Returns 0 on success, -1 if registry full.
 * Called during initialization (side_agent_init).
 */
int side_agent_register(const side_agent_t *agent);

/*
 * Initialize the side-agent subsystem. Registers built-in agents
 * (global memory). Call after memory agent init.
 */
void side_agent_init(void);

/*
 * Run all enabled pre-query agents in parallel.
 * Forks one child per agent, pipes results back, select() with timeout.
 * Returns malloc'd combined context string with each result wrapped:
 *   [name context]\n...\n[end name context]\n
 * Returns NULL if no agent produced output. Caller frees.
 */
char *side_agents_pre_query(const char *query, const char *cwd);

/*
 * Run all enabled post-query agents in background.
 * Each gets its own double-fork (fire-and-forget).
 * Parent returns immediately.
 */
void side_agents_post_query(const char *query, const char *response, const char *cwd);

#endif /* BASH_LLM_SIDE_AGENT_H */
