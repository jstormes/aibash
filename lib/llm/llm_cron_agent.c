#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#include "llm_cron_agent.h"
#include "llm_global_mem_api.h"
#include "llm_streams.h"
#include "cJSON.h"

#define CRON_MAX_JOBS 256

typedef struct {
    int id;
    char *type;         /* "cron" or "at" */
    char *description;  /* human-readable description */
    char *schedule;     /* cron: "0 3 * * *", at: "2026-12-15 08:00" */
    char *command;      /* shell command to execute */
    int at_job_id;      /* at job system ID (-1 if cron) */
} cron_job_t;

static cron_job_t g_jobs[CRON_MAX_JOBS];
static int g_job_count = 0;
static int g_next_id = 1;
static int g_ready = 0;
static char g_storage_dir[4096];
static char g_jobs_path[4096];

/* ---- LLM Prompts ---- */

static const char *SEARCH_PROMPT =
    "Which scheduled tasks below could be relevant to the query?\n"
    "Include anything related to the topic, time, or activity mentioned.\n"
    "When in doubt, include it. Copy task lines exactly.\n"
    "If nothing relevant: NONE\n\n"
    "Example 1:\n"
    "Query: what do I have coming up\n"
    "Tasks: [1] daily backup at 3am [2] birthday Dec 15\n"
    "Answer:\n[1] daily backup at 3am\n[2] birthday Dec 15\n\n"
    "Example 2:\n"
    "Query: what is my name\n"
    "Tasks: [1] daily backup at 3am\n"
    "Answer: NONE";

static const char *EXTRACT_PROMPT =
    "You are a scheduling agent. Analyze the conversation and extract any "
    "requests to schedule recurring or one-time tasks.\n\n"
    "RULES:\n"
    "1. Only extract EXPLICIT scheduling requests\n"
    "2. If nothing to schedule, return: []\n"
    "3. Output RAW JSON only. No markdown, no code fences, no explanation.\n\n"
    "For recurring tasks, use type \"cron\" with a 5-field cron schedule:\n"
    "  minute hour day-of-month month day-of-week\n"
    "For one-time tasks, use type \"at\" with a datetime string.\n\n"
    "FORMAT:\n"
    "[{\"type\": \"cron\", \"schedule\": \"0 3 * * *\", "
    "\"command\": \"shell command\", \"description\": \"human description\"}]\n"
    "[{\"type\": \"at\", \"schedule\": \"2026-12-25 08:00\", "
    "\"command\": \"shell command\", \"description\": \"human description\"}]\n\n"
    "DO NOT schedule: questions about schedules, casual mentions of time.\n"
    "DO schedule: explicit requests like 'run X every day at Y', "
    "'remind me on DATE', 'schedule a backup at TIME'.";

/* ---- Storage helpers ---- */

static void free_job(cron_job_t *j)
{
    free(j->type);
    free(j->description);
    free(j->schedule);
    free(j->command);
    j->type = j->description = j->schedule = j->command = NULL;
}

static int load_jobs(void)
{
    FILE *f = fopen(g_jobs_path, "r");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0) { fclose(f); return 0; }

    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    cJSON *arr = cJSON_Parse(buf);
    free(buf);
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return 0;
    }

    int n = cJSON_GetArraySize(arr);
    g_job_count = 0;
    g_next_id = 1;

    for (int i = 0; i < n && g_job_count < CRON_MAX_JOBS; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!item) continue;

        cJSON *j_id   = cJSON_GetObjectItem(item, "id");
        cJSON *j_type = cJSON_GetObjectItem(item, "type");
        cJSON *j_desc = cJSON_GetObjectItem(item, "description");
        cJSON *j_sched = cJSON_GetObjectItem(item, "schedule");
        cJSON *j_cmd  = cJSON_GetObjectItem(item, "command");
        cJSON *j_atid = cJSON_GetObjectItem(item, "at_job_id");

        if (!j_type || !cJSON_IsString(j_type)) continue;
        if (!j_sched || !cJSON_IsString(j_sched)) continue;

        cron_job_t *j = &g_jobs[g_job_count];
        j->id = (j_id && cJSON_IsNumber(j_id)) ? j_id->valueint : g_next_id;
        j->type = strdup(j_type->valuestring);
        j->description = (j_desc && cJSON_IsString(j_desc)) ? strdup(j_desc->valuestring) : strdup("");
        j->schedule = strdup(j_sched->valuestring);
        j->command = (j_cmd && cJSON_IsString(j_cmd)) ? strdup(j_cmd->valuestring) : strdup("");
        j->at_job_id = (j_atid && cJSON_IsNumber(j_atid)) ? j_atid->valueint : -1;

        if (j->id >= g_next_id) g_next_id = j->id + 1;
        g_job_count++;
    }

    cJSON_Delete(arr);
    return g_job_count;
}

static int save_jobs(void)
{
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < g_job_count; i++) {
        cron_job_t *j = &g_jobs[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", j->id);
        cJSON_AddStringToObject(item, "type", j->type);
        cJSON_AddStringToObject(item, "description", j->description);
        cJSON_AddStringToObject(item, "schedule", j->schedule);
        cJSON_AddStringToObject(item, "command", j->command);
        if (j->at_job_id >= 0)
            cJSON_AddNumberToObject(item, "at_job_id", j->at_job_id);
        cJSON_AddItemToArray(arr, item);
    }

    char *text = cJSON_Print(arr);
    cJSON_Delete(arr);
    if (!text) return -1;

    FILE *f = fopen(g_jobs_path, "w");
    if (!f) { free(text); return -1; }
    fputs(text, f);
    fclose(f);
    free(text);
    return 0;
}

/* ---- Crontab sync ---- */

static char *crontab_read(void)
{
    FILE *fp = popen("crontab -l 2>/dev/null", "r");
    if (!fp) return NULL;

    char *buf = NULL;
    size_t len = 0, cap = 0;
    char line[1024];

    while (fgets(line, sizeof(line), fp)) {
        size_t llen = strlen(line);
        if (len + llen + 1 > cap) {
            cap = (cap == 0) ? 2048 : cap * 2;
            buf = realloc(buf, cap);
        }
        memcpy(buf + len, line, llen);
        len += llen;
    }
    pclose(fp);

    if (buf) buf[len] = '\0';
    return buf;
}

static int crontab_sync(void)
{
    /* Read existing crontab, preserve non-aibash entries */
    char *existing = crontab_read();
    char *clean = NULL;
    size_t clen = 0, ccap = 0;

    /* Filter out aibash-managed lines */
    if (existing) {
        char *line = existing;
        int skip_next = 0;
        while (*line) {
            char *eol = strchr(line, '\n');
            size_t llen = eol ? (size_t)(eol - line + 1) : strlen(line);

            if (strstr(line, "# aibash:id=") == line) {
                skip_next = 1;  /* skip the comment and the next line (the cron entry) */
            } else if (skip_next) {
                skip_next = 0;  /* skip this cron entry line */
            } else {
                /* Preserve this line */
                if (clen + llen + 1 > ccap) {
                    ccap = (ccap == 0) ? 2048 : ccap * 2;
                    clean = realloc(clean, ccap);
                }
                memcpy(clean + clen, line, llen);
                clen += llen;
            }

            line += llen;
        }
        free(existing);
    }

    /* Append aibash cron entries */
    for (int i = 0; i < g_job_count; i++) {
        if (strcmp(g_jobs[i].type, "cron") != 0) continue;

        char entry[2048];
        int elen = snprintf(entry, sizeof(entry), "# aibash:id=%d %s\n%s %s\n",
                            g_jobs[i].id, g_jobs[i].description,
                            g_jobs[i].schedule, g_jobs[i].command);

        if (clen + elen + 1 > ccap) {
            ccap = (ccap == 0) ? 2048 : ccap * 2;
            if (clen + elen + 1 > ccap) ccap = clen + elen + 256;
            clean = realloc(clean, ccap);
        }
        memcpy(clean + clen, entry, elen);
        clen += elen;
    }

    if (!clean || clen == 0) {
        free(clean);
        return 0;
    }

    clean[clen] = '\0';

    /* Write back via crontab - */
    FILE *fp = popen("crontab -", "w");
    if (!fp) { free(clean); return -1; }
    fputs(clean, fp);
    int ret = pclose(fp);
    free(clean);

    return (ret == 0) ? 0 : -1;
}

/* ---- At helpers ---- */

static int at_create(const char *at_time, const char *command)
{
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "echo '%s' | at '%s' 2>&1", command, at_time);

    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    char buf[512];
    int job_id = -1;
    while (fgets(buf, sizeof(buf), fp)) {
        /* Parse: "job 42 at ..." */
        if (strncmp(buf, "job ", 4) == 0) {
            job_id = atoi(buf + 4);
        }
        /* Some systems: "warning: commands will be executed using /bin/sh" then "job N at ..." */
        char *p = strstr(buf, "job ");
        if (p) job_id = atoi(p + 4);
    }
    pclose(fp);
    return job_id;
}

static int at_remove(int job_id)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "atrm %d 2>/dev/null", job_id);
    return system(cmd) == 0 ? 0 : -1;
}

/* ---- Init / cleanup ---- */

int cron_agent_init(server_config_t *config)
{
    (void)config;

    /* Only compute paths on first init (not re-init in forked child) */
    if (!g_storage_dir[0]) {
        const char *home = getenv("HOME");
        snprintf(g_storage_dir, sizeof(g_storage_dir),
                 "%s/.aibash_cron", home ? home : ".");
        snprintf(g_jobs_path, sizeof(g_jobs_path),
                 "%s/jobs.json", g_storage_dir);
        mkdir(g_storage_dir, 0755);
    }

    load_jobs();
    g_ready = 1;
    return 0;
}

void cron_agent_cleanup(void)
{
    for (int i = 0; i < g_job_count; i++)
        free_job(&g_jobs[i]);
    g_job_count = 0;
    g_next_id = 1;
    g_ready = 0;
}

int llm_cron_agent_ready(void)
{
    return g_ready;
}

int llm_cron_agent_count(void)
{
    return g_ready ? g_job_count : 0;
}

/* ---- User commands ---- */

int llm_cron_add(const char *type, const char *schedule,
                  const char *command, const char *description)
{
    if (!g_ready || g_job_count >= CRON_MAX_JOBS) return -1;
    if (!type || !schedule || !command) return -1;

    cron_job_t *j = &g_jobs[g_job_count];
    j->id = g_next_id++;
    j->type = strdup(type);
    j->schedule = strdup(schedule);
    j->command = strdup(command);
    j->description = description ? strdup(description) : strdup("");
    j->at_job_id = -1;

    /* Create the actual system job */
    if (strcmp(type, "at") == 0) {
        int at_id = at_create(schedule, command);
        if (at_id < 0) {
            free_job(j);
            return -1;
        }
        j->at_job_id = at_id;
    }

    g_job_count++;
    save_jobs();

    /* Sync crontab for cron jobs */
    if (strcmp(type, "cron") == 0)
        crontab_sync();

    return j->id;
}

int llm_cron_remove(int id)
{
    if (!g_ready) return -1;

    for (int i = 0; i < g_job_count; i++) {
        if (g_jobs[i].id != id) continue;

        /* Remove system job */
        if (strcmp(g_jobs[i].type, "at") == 0 && g_jobs[i].at_job_id >= 0)
            at_remove(g_jobs[i].at_job_id);

        free_job(&g_jobs[i]);

        /* Compact array */
        for (int k = i; k < g_job_count - 1; k++)
            g_jobs[k] = g_jobs[k + 1];
        g_job_count--;

        save_jobs();
        crontab_sync();
        return 0;
    }
    return -1;
}

char *llm_cron_list(void)
{
    if (!g_ready || g_job_count == 0)
        return strdup("(no scheduled tasks)\n");

    char *buf = NULL;
    size_t len = 0, cap = 0;

    for (int i = 0; i < g_job_count; i++) {
        char line[1024];
        snprintf(line, sizeof(line), "[%d] (%s) %s — %s — %s\n",
                 g_jobs[i].id, g_jobs[i].type,
                 g_jobs[i].schedule, g_jobs[i].description,
                 g_jobs[i].command);
        size_t llen = strlen(line);
        if (len + llen + 1 > cap) {
            cap = (cap == 0) ? 1024 : cap * 2;
            buf = realloc(buf, cap);
        }
        memcpy(buf + len, line, llen);
        len += llen;
    }

    if (buf) buf[len] = '\0';
    return buf ? buf : strdup("");
}

/* ---- Side agent callbacks ---- */

static char *build_search_message(const char *query)
{
    if (g_job_count == 0) return NULL;

    char *job_list = llm_cron_list();
    if (!job_list) return NULL;

    /* Add current time for context */
    time_t now = time(NULL);
    char timestr[64];
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M %Z (%A)", localtime(&now));

    size_t len = strlen(query) + strlen(job_list) + strlen(timestr) + 128;
    char *msg = malloc(len);
    snprintf(msg, len,
             "Current time: %s\n\nQuery: %s\n\nScheduled tasks:\n%s",
             timestr, query, job_list);
    free(job_list);
    return msg;
}

static int is_empty_response(const char *text)
{
    if (!text || !text[0]) return 1;
    const char *p = text;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '\0') return 1;
    if (strncasecmp(p, "NONE", 4) == 0) return 1;
    return 0;
}

char *cron_agent_pre_query_cb(const char *query, const char *cwd)
{
    (void)cwd;
    if (!query || !query[0]) return NULL;
    if (g_job_count == 0) return NULL;

    char *msg = build_search_message(query);
    if (!msg) return NULL;

    stream_cron_agent_output("checking schedule...\n");

    /* LLM search — agents run serially to avoid curl fork issues */
    char *result = llm_global_mem_api_chat(SEARCH_PROMPT, msg, 0, "cron-search");
    free(msg);

    if (is_empty_response(result)) {
        free(result);
        return NULL;
    }

    stream_cron_agent_output(result);
    return result;
}

void cron_agent_post_query_cb(const char *query, const char *response, const char *cwd)
{
    (void)cwd;
    if (!g_ready) return;

    /* Reinitialize from disk in the forked child */
    cron_agent_cleanup();
    cron_agent_init(NULL);  /* re-init from disk */

    size_t conv_len = strlen(query) + strlen(response) + 64;
    char *conversation = malloc(conv_len);
    snprintf(conversation, conv_len, "User: %s\nAssistant: %s", query, response);

    char *result = llm_global_mem_api_chat(EXTRACT_PROMPT, conversation, 0, "cron-extract");
    free(conversation);

    if (!result || !result[0]) { free(result); return; }

    /* Parse JSON response */
    const char *start = result;
    while (*start && *start != '[') start++;
    if (!*start) { free(result); return; }

    cJSON *arr = cJSON_Parse(start);
    free(result);
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return; }

    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!item) continue;

        cJSON *j_type = cJSON_GetObjectItem(item, "type");
        cJSON *j_sched = cJSON_GetObjectItem(item, "schedule");
        cJSON *j_cmd = cJSON_GetObjectItem(item, "command");
        cJSON *j_desc = cJSON_GetObjectItem(item, "description");

        if (!j_type || !cJSON_IsString(j_type)) continue;
        if (!j_sched || !cJSON_IsString(j_sched)) continue;
        if (!j_cmd || !cJSON_IsString(j_cmd)) continue;

        const char *desc = (j_desc && cJSON_IsString(j_desc)) ? j_desc->valuestring : "";

        int id = llm_cron_add(j_type->valuestring, j_sched->valuestring,
                               j_cmd->valuestring, desc);
        if (id > 0) {
            char dbg[512];
            snprintf(dbg, sizeof(dbg), "scheduled: [%d] %s %s — %s\n",
                     id, j_type->valuestring, j_sched->valuestring, desc);
            stream_cron_agent_output(dbg);
        }
    }

    cJSON_Delete(arr);
}
