#ifndef BASH_LLM_CRON_AGENT_H
#define BASH_LLM_CRON_AGENT_H

/*
 * Cron Side Agent
 *
 * Manages user's scheduled tasks (cron + at). Uses LLM to:
 *   - pre_query: search scheduled tasks relevant to the user's question
 *   - post_query: detect scheduling intents and create cron/at jobs
 *
 * The agent owns the job store exclusively. The main agent only sees
 * injected [cron context] blocks.
 */

#define CRON_AGENT_MAX_JOBS 256

typedef struct {
    int id;
    char *type;         /* "cron" or "at" */
    char *description;  /* human-readable description */
    char *schedule;     /* cron: "0 3 * * *", at: "2026-12-15 08:00" */
    char *command;      /* shell command to execute */
    int at_job_id;      /* at job system ID (-1 if cron) */
} cron_job_t;

/*
 * Initialize the cron agent.
 *   storage_dir: path to job store directory (e.g., ~/.aibash_cron)
 * Returns number of jobs loaded, or -1 on error.
 */
int  llm_cron_agent_init(const char *storage_dir);
void llm_cron_agent_cleanup(void);
int  llm_cron_agent_ready(void);

/* Job count */
int llm_cron_agent_count(void);

/* User commands */
int   llm_cron_add(const char *type, const char *schedule,
                    const char *command, const char *description);
int   llm_cron_remove(int id);
char *llm_cron_list(void);

/* Side agent callbacks */
char *cron_agent_pre_query_cb(const char *query, const char *cwd);
void  cron_agent_post_query_cb(const char *query, const char *response, const char *cwd);

#endif /* BASH_LLM_CRON_AGENT_H */
