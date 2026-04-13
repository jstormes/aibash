#ifndef BASH_LLM_CRON_AGENT_H
#define BASH_LLM_CRON_AGENT_H

#include "llm_serverconf.h"

/*
 * Cron Side Agent
 *
 * Manages user's scheduled tasks (cron + at).
 */

#define CRON_AGENT_MAX_JOBS 256

/* Side agent callbacks (registered by the framework) */
int   cron_agent_init(server_config_t *config);
void  cron_agent_cleanup(void);
char *cron_agent_pre_query_cb(const char *query, const char *cwd);
void  cron_agent_post_query_cb(const char *query, const char *response, const char *cwd);

/* User-facing commands (called from llm builtin) */
int   llm_cron_add(const char *type, const char *schedule,
                    const char *command, const char *description);
int   llm_cron_remove(int id);
char *llm_cron_list(void);
int   llm_cron_agent_count(void);
int   llm_cron_agent_ready(void);

#endif /* BASH_LLM_CRON_AGENT_H */
