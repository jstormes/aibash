#ifndef LLMSH_SERVERCONF_H
#define LLMSH_SERVERCONF_H

#define LLMSH_MAX_SERVERS 16

typedef struct {
    char *name;
    char *api_url;
    char *model;
    char *api_key;
} server_entry_t;

typedef struct {
    server_entry_t servers[LLMSH_MAX_SERVERS];
    int count;
    int active;     /* index of active server */

    /* Global settings from [settings] section */
    int max_iterations;
    int man_enrich;      /* 0=off, 1=whatis auto (default) */
    int man_max_bytes;   /* max bytes for detail lookups (default: 4096) */
} server_config_t;

/* Load config from ~/.llmshrc. Falls back to env vars. */
server_config_t *serverconf_load(void);
void serverconf_free(server_config_t *conf);

/* Get active server */
const server_entry_t *serverconf_active(const server_config_t *conf);

/* Switch active server by name. Returns 0 on success, -1 if not found. */
int serverconf_switch(server_config_t *conf, const char *name);

/* List all servers to stdout */
void serverconf_list(const server_config_t *conf);

#endif /* LLMSH_SERVERCONF_H */
