#ifndef CRON_AGENT_H
#define CRON_AGENT_H

/*
 * Cron Side Agent
 *
 * Manages scheduled tasks (cron + at). Implements side_agent_t callbacks
 * and exposes user-facing commands (add, remove, list).
 *
 * Dependencies injected via cron_agent_deps_t for testability.
 */

#include "side_agent.h"

typedef char *(*cron_api_chat_fn)(const char *system_prompt,
                                  const char *user_message,
                                  int enable_thinking,
                                  const char *log_caller);

typedef struct {
    /* LLM API (for pre_query search and post_query extraction) */
    cron_api_chat_fn api_chat;

    /* System commands (crontab, at) — NULL to skip in tests */
    int (*crontab_sync)(void *agent_state);
    int (*at_create)(const char *at_time, const char *command);
    int (*at_remove)(int job_id);
} cron_agent_deps_t;

/*
 * Initialize with explicit dependencies.
 * storage_dir: path to job store (e.g., ~/.aibash_cron)
 * deps: function pointers (api_chat required for LLM, others optional)
 */
int cron_agent_init_with_deps(const char *storage_dir, const cron_agent_deps_t *deps);

/* Side agent callbacks */
int   cron_agent_init(void *config);
void  cron_agent_cleanup(void);
char *cron_agent_pre_query(const char *query, const char *cwd);
void  cron_agent_post_query(const char *query, const char *response, const char *cwd);

/* User commands */
int   cron_agent_add(const char *type, const char *schedule,
                     const char *command, const char *description);
int   cron_agent_remove(int id);
char *cron_agent_list(void);
int   cron_agent_count(void);
int   cron_agent_ready(void);

#endif /* CRON_AGENT_H */
