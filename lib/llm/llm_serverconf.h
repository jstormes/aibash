#ifndef BASH_LLM_SERVERCONF_H
#define BASH_LLM_SERVERCONF_H

#include "llm_config.h"

typedef struct {
    char *name;
    char *api_url;
    char *model;
    char *api_key;
} server_entry_t;

typedef struct {
    server_entry_t servers[BASH_LLM_MAX_SERVERS];
    int count;
    int active;     /* index of active server */

    /* Global settings from [settings] section */
    int max_iterations;
    int man_enrich;          /* 0=off, 1=whatis auto (default) */
    int man_max_bytes;       /* max bytes for detail lookups (default: 4096) */
    int command_not_found;   /* 0=off, 1=on (default) -- route unknown commands to LLM */
} server_config_t;

/* Load config from ~/.bashllmrc. Falls back to env vars. */
server_config_t *llm_serverconf_load(void);
void llm_serverconf_free(server_config_t *conf);

/* Get active server */
const server_entry_t *llm_serverconf_active(const server_config_t *conf);

/* Switch active server by name. Returns 0 on success, -1 if not found. */
int llm_serverconf_switch(server_config_t *conf, const char *name);

/* List all servers to stdout */
void llm_serverconf_list(const server_config_t *conf);

#endif /* BASH_LLM_SERVERCONF_H */
