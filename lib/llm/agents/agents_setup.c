/*
 * agents_setup.c -- production wiring for all side agents.
 *
 * This is the ONLY file that knows about production dependencies
 * (llm_memory_*, llm_global_mem_api_*, llm_ical_*, etc).
 * Individual agents never reach outside their own module.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "agents_setup.h"
#include "side_agent.h"
#include "mem_agent.h"
#include "cron_agent.h"
#include "calendar_agent.h"
#include "../llm_serverconf.h"

/* ---- Production dependencies (linked from lib/llm/) ---- */

/* Memory store */
extern int   llm_memory_init(const char *, int);
extern void  llm_memory_cleanup(void);
extern int   llm_memory_save(const char *, const char *);
extern int   llm_memory_forget(int);
extern int   llm_memory_forget_match(const char *);
extern char *llm_memory_list(void);
extern int   llm_memory_count(void);

/* LLM API client (shared by all agents) */
extern int   llm_global_mem_api_init(const char *, const char *, const char *);
extern void  llm_global_mem_api_cleanup(void);
extern char *llm_global_mem_api_chat(const char *, const char *, int, const char *);

/* Logging */
extern void  llm_log_init(const char *);

/* iCal store */
extern int   llm_ical_load(const char *);
extern int   llm_ical_save(const char *);
extern int   llm_ical_add(const char *, const char *, const char *, const char *);
extern int   llm_ical_remove(int);
extern char *llm_ical_list(void);
extern int   llm_ical_count(void);
extern void  llm_ical_cleanup(void);

/* ---- Setup ---- */

void agents_setup(void *config)
{
    server_config_t *cfg = (server_config_t *)config;
    if (!cfg) return;

    const char *home = getenv("HOME");
    if (!home) home = ".";

    /* ---- Initialize the shared LLM API client ---- */
    if (cfg->memory_enabled && cfg->memory_api_url) {
        llm_global_mem_api_init(cfg->memory_api_url,
                                cfg->memory_model,
                                cfg->memory_api_key);
    }

    /* ---- Memory agent ---- */
    if (cfg->memory_enabled) {
        char memdir[4096];
        snprintf(memdir, sizeof(memdir), "%s/.aibash_memories", home);

        /* Init logging */
        char logdir[4200];
        snprintf(logdir, sizeof(logdir), "%s/logs", memdir);
        llm_log_init(logdir);

        mem_agent_deps_t mem_deps = {
            .mem_init         = llm_memory_init,
            .mem_cleanup      = llm_memory_cleanup,
            .mem_save         = llm_memory_save,
            .mem_forget       = llm_memory_forget,
            .mem_forget_match = llm_memory_forget_match,
            .mem_list         = llm_memory_list,
            .mem_count        = llm_memory_count,
            .api_init         = NULL,  /* already initialized above */
            .api_cleanup      = NULL,
            .api_chat         = cfg->memory_api_url ? llm_global_mem_api_chat : NULL,
            .log_init         = NULL,  /* already initialized above */
        };

        mem_agent_init_with_deps(memdir, cfg->memory_max, &mem_deps);

        side_agent_register(&(side_agent_t){
            .name        = "global_memory",
            .timeout_sec = 15,
            .enabled     = 1,
            .pre_query   = mem_agent_pre_query,
            .post_query  = mem_agent_post_query,
            .cleanup     = mem_agent_cleanup,
        });
    }

    /* ---- Cron agent ---- */
    {
        char crondir[4096];
        snprintf(crondir, sizeof(crondir), "%s/.aibash_cron", home);

        cron_agent_deps_t cron_deps = {
            .api_chat      = cfg->memory_api_url ? llm_global_mem_api_chat : NULL,
            .crontab_sync  = NULL,
            .at_create     = NULL,
            .at_remove     = NULL,
        };

        cron_agent_init_with_deps(crondir, &cron_deps);

        side_agent_register(&(side_agent_t){
            .name        = "cron",
            .timeout_sec = 15,
            .enabled     = 1,
            .pre_query   = cron_agent_pre_query,
            .post_query  = cron_agent_post_query,
            .cleanup     = cron_agent_cleanup,
        });
    }

    /* ---- Calendar agent ---- */
    {
        char caldir[4096];
        snprintf(caldir, sizeof(caldir), "%s/.aibash_calendar", home);

        calendar_agent_deps_t cal_deps = {
            .api_chat       = cfg->memory_api_url ? llm_global_mem_api_chat : NULL,
            .store_load     = llm_ical_load,
            .store_save     = llm_ical_save,
            .store_add      = llm_ical_add,
            .store_remove   = llm_ical_remove,
            .store_list     = llm_ical_list,
            .store_count    = llm_ical_count,
            .store_cleanup  = llm_ical_cleanup,
        };

        calendar_agent_init_with_deps(caldir, &cal_deps);

        side_agent_register(&(side_agent_t){
            .name        = "calendar",
            .timeout_sec = 15,
            .enabled     = 1,
            .pre_query   = calendar_agent_pre_query,
            .post_query  = calendar_agent_post_query,
            .cleanup     = calendar_agent_cleanup,
        });
    }
}
