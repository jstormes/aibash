#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm_serverconf.h"
#include "llm_config.h"
#include "llm_streams.h"

/* Strip leading/trailing whitespace in place */
static char *strip(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
        *end-- = '\0';
    return s;
}

server_config_t *llm_serverconf_load(void)
{
    server_config_t *conf = calloc(1, sizeof(*conf));

    char path[4096];
    const char *home = getenv("HOME");
    if (home)
        snprintf(path, sizeof(path), "%s/.bashllmrc", home);
    else
        snprintf(path, sizeof(path), ".bashllmrc");

    /* Defaults for global settings */
    conf->max_iterations = BASH_LLM_MAX_ITERATIONS;
    conf->man_enrich = 1;
    conf->man_max_bytes = 4096;
    conf->command_not_found = 0;  /* off by default */
    conf->memory_enabled = 1;    /* on by default */
    conf->memory_max = BASH_LLM_MEMORY_MAX;  /* 1500 default, fits 262K context */

    FILE *f = fopen(path, "r");
    if (f) {
        char line[1024];
        int cur = -1;
        int in_settings = 0;

        while (fgets(line, sizeof(line), f)) {
            char *s = strip(line);
            if (s[0] == '#' || s[0] == '\0')
                continue;

            if (s[0] == '[') {
                char *end = strchr(s, ']');
                if (end) {
                    *end = '\0';
                    if (strcmp(s + 1, "settings") == 0) {
                        in_settings = 1;
                        cur = -1;
                        continue;
                    }
                    if (strcmp(s + 1, "memory") == 0) {
                        in_settings = 2;  /* 2 = memory section */
                        cur = -1;
                        continue;
                    }
                    in_settings = 0;
                    if (conf->count < BASH_LLM_MAX_SERVERS) {
                        cur = conf->count++;
                        conf->servers[cur].name = strdup(s + 1);
                    }
                }
                continue;
            }

            char *eq = strchr(s, '=');
            if (!eq) continue;

            *eq = '\0';
            char *key = strip(s);
            char *val = strip(eq + 1);

            if (in_settings == 2) {
                /* [memory] section */
                if (strcmp(key, "url") == 0)
                    conf->memory_api_url = strdup(val);
                else if (strcmp(key, "model") == 0)
                    conf->memory_model = strdup(val);
                else if (strcmp(key, "key") == 0)
                    conf->memory_api_key = strdup(val);
                continue;
            }

            if (in_settings == 1) {
                /* [settings] section */
                if (strcmp(key, "max_iterations") == 0)
                    conf->max_iterations = atoi(val);
                else if (strcmp(key, "man_enrich") == 0)
                    conf->man_enrich = atoi(val);
                else if (strcmp(key, "man_max_bytes") == 0)
                    conf->man_max_bytes = atoi(val);
                else if (strcmp(key, "command_not_found") == 0)
                    conf->command_not_found = atoi(val);
                else if (strcmp(key, "memory") == 0)
                    conf->memory_enabled = atoi(val);
                else if (strcmp(key, "memory_max") == 0)
                    conf->memory_max = atoi(val);
                continue;
            }

            if (cur < 0) continue;

            if (strcmp(key, "url") == 0)
                conf->servers[cur].api_url = strdup(val);
            else if (strcmp(key, "model") == 0)
                conf->servers[cur].model = strdup(val);
            else if (strcmp(key, "key") == 0)
                conf->servers[cur].api_key = strdup(val);
        }
        fclose(f);
    }

    /* If no servers loaded, create one from env vars / defaults */
    if (conf->count == 0) {
        conf->count = 1;
        conf->servers[0].name = strdup("default");

        const char *url = getenv("BASH_LLM_API_URL");
        const char *model = getenv("BASH_LLM_MODEL");
        const char *key = getenv("BASH_LLM_API_KEY");

        conf->servers[0].api_url = strdup(url ? url : BASH_LLM_DEFAULT_API_URL);
        conf->servers[0].model   = strdup(model ? model : BASH_LLM_DEFAULT_MODEL);
        conf->servers[0].api_key = key ? strdup(key) : NULL;
    }

    conf->active = 0;
    return conf;
}

void llm_serverconf_free(server_config_t *conf)
{
    if (!conf) return;
    for (int i = 0; i < conf->count; i++) {
        free(conf->servers[i].name);
        free(conf->servers[i].api_url);
        free(conf->servers[i].model);
        free(conf->servers[i].api_key);
    }
    free(conf->memory_api_url);
    free(conf->memory_model);
    free(conf->memory_api_key);
    free(conf);
}

const server_entry_t *llm_serverconf_active(const server_config_t *conf)
{
    return &conf->servers[conf->active];
}

int llm_serverconf_switch(server_config_t *conf, const char *name)
{
    for (int i = 0; i < conf->count; i++) {
        if (strcmp(conf->servers[i].name, name) == 0) {
            conf->active = i;
            return 0;
        }
    }
    return -1;
}

void llm_serverconf_list(const server_config_t *conf)
{
    for (int i = 0; i < conf->count; i++) {
        char line[512];
        snprintf(line, sizeof(line), "%s %s (%s @ %s)\n",
                 i == conf->active ? "*" : " ",
                 conf->servers[i].name,
                 conf->servers[i].model ? conf->servers[i].model : "default",
                 conf->servers[i].api_url ? conf->servers[i].api_url : "(none)");
        stream_chat_output(line);
    }
}
