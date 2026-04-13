#ifndef SIDE_AGENT_H
#define SIDE_AGENT_H

/*
 * Side Agent Framework
 *
 * Manages a registry of side agents that run alongside the main LLM.
 * Each agent provides optional callbacks:
 *
 *   init       — called once at startup with opaque config
 *   cleanup    — free resources
 *   pre_query  — fork a child, run callback, pipe result back (with timeout)
 *   post_query — double-fork, fire-and-forget background work
 *
 * The framework handles all fork/pipe/select/reap/SIGPIPE plumbing.
 * It knows nothing about what specific agents do.
 */

#define SIDE_AGENT_MAX             8
#define SIDE_AGENT_DEFAULT_TIMEOUT 5
#define SIDE_AGENT_BUF_SIZE        4096

typedef struct {
    const char *name;       /* agent identifier */
    int timeout_sec;        /* pre_query timeout; 0 = default (5s) */
    int enabled;            /* set by init; 1 = active */

    /* Called once at startup. config is opaque — agent casts as needed.
     * Return 0 on success (framework sets enabled=1), -1 on failure. */
    int (*init)(void *config);

    /* Free resources. */
    void (*cleanup)(void);

    /* Run in forked child before main LLM call.
     * Return malloc'd string to inject, or NULL if nothing relevant. */
    char *(*pre_query)(const char *query, const char *cwd);

    /* Run in double-forked grandchild after main LLM responds.
     * Fire-and-forget — no return value. */
    void (*post_query)(const char *query, const char *response, const char *cwd);
} side_agent_t;

/* Register an agent. Returns 0 on success, -1 if full. */
int side_agent_register(const side_agent_t *agent);

/* Call init() on all registered agents. Sets enabled=1 for those that succeed. */
void side_agent_init(void *config);

/* Call cleanup() on all agents and reset the registry. */
void side_agent_cleanup(void);

/* Number of registered agents. */
int side_agent_count(void);

/* Run pre_query on all enabled agents (serial fork/pipe/select).
 * Returns malloc'd combined result, or NULL. Caller frees. */
char *side_agents_pre_query(const char *query, const char *cwd);

/* Run post_query on all enabled agents (double-fork, fire-and-forget). */
void side_agents_post_query(const char *query, const char *response, const char *cwd);

#endif /* SIDE_AGENT_H */
