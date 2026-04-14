#include "agents_setup.h"
#include "side_agent.h"
#include "mem_agent.h"
#include "cron_agent.h"

void agents_setup(void *config)
{
    /* Register memory agent */
    side_agent_register(&(side_agent_t){
        .name        = "global_memory",
        .timeout_sec = 15,
        .init        = mem_agent_init,
        .cleanup     = mem_agent_cleanup,
        .pre_query   = mem_agent_pre_query,
        .post_query  = mem_agent_post_query,
    });

    /* Register cron agent (pre_query is instant — no LLM call) */
    side_agent_register(&(side_agent_t){
        .name        = "cron",
        .timeout_sec = 2,
        .init        = cron_agent_init,
        .cleanup     = cron_agent_cleanup,
        .pre_query   = cron_agent_pre_query,
        .post_query  = cron_agent_post_query,
    });

    /* Initialize all registered agents */
    side_agent_init(config);
}
