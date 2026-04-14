#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#include "cron_agent.h"

/* ---- Internal types ---- */

#define CRON_MAX_JOBS 256

typedef struct {
    int id;
    char *type;         /* "cron" or "at" */
    char *description;
    char *schedule;     /* cron: "0 3 * * *", at: "2026-12-15 08:00" */
    char *command;
    int at_job_id;      /* at system job ID, -1 if cron */
} cron_job_t;

/* ---- State ---- */

static cron_job_t g_jobs[CRON_MAX_JOBS];
static int g_job_count = 0;
static int g_next_id = 1;
static int g_ready = 0;
static char g_storage_dir[4096];
static char g_jobs_path[4096];
static cron_agent_deps_t g_deps;
static int g_has_api = 0;

/* ---- LLM Prompts ---- */

static const char *CLASSIFY_PROMPT =
    "Is this query about schedules, reminders, tasks, time, or events?\n"
    "Answer only YES or NO.";

static const char *EXTRACT_PROMPT =
    "You are a scheduling agent. Analyze the conversation and extract any "
    "NEW requests from the USER to schedule tasks.\n\n"
    "RULES:\n"
    "1. Only extract EXPLICIT NEW scheduling requests from the USER\n"
    "2. If nothing new to schedule, return: []\n"
    "3. Output RAW JSON only. No markdown, no code fences, no explanation.\n"
    "4. Do NOT re-create tasks the assistant mentioned from existing schedules\n"
    "5. Only the USER can request scheduling, not the assistant\n\n"
    "FORMAT:\n"
    "[{\"type\": \"cron\", \"schedule\": \"0 3 * * *\", "
    "\"command\": \"shell command\", \"description\": \"human description\"}]\n\n"
    "DO NOT schedule: questions about schedules, assistant responses,\n"
    "casual mentions of time, or tasks already scheduled.\n"
    "DO schedule: explicit NEW requests like 'run X every day at Y', "
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

/* Minimal JSON parser for jobs.json — avoids cJSON dependency in tests.
 * Parses a JSON array of objects with string and number fields. */

static char *json_extract_string(const char *obj, const char *key)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    char *pos = strstr(obj, search);
    if (!pos) return NULL;
    char *colon = strchr(pos + strlen(search), ':');
    if (!colon) return NULL;
    char *q1 = strchr(colon, '"');
    if (!q1) return NULL;
    q1++;
    char *q2 = q1;
    while (*q2 && !(*q2 == '"' && *(q2 - 1) != '\\')) q2++;
    if (!*q2) return NULL;
    size_t len = q2 - q1;
    char *val = malloc(len + 1);
    memcpy(val, q1, len);
    val[len] = '\0';
    return val;
}

static int json_extract_int(const char *obj, const char *key, int def)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    char *pos = strstr(obj, search);
    if (!pos) return def;
    char *colon = strchr(pos + strlen(search), ':');
    if (!colon) return def;
    return atoi(colon + 1);
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

    g_job_count = 0;
    g_next_id = 1;

    /* Walk through JSON array objects */
    const char *p = buf;
    while (*p && *p != '[') p++;
    if (!*p) { free(buf); return 0; }
    p++;

    while (*p && g_job_count < CRON_MAX_JOBS) {
        while (*p && *p != '{') {
            if (*p == ']') goto done;
            p++;
        }
        if (!*p) break;

        /* Find end of object */
        int depth = 0;
        const char *end = NULL;
        for (const char *q = p; *q; q++) {
            if (*q == '{') depth++;
            else if (*q == '}') { depth--; if (depth == 0) { end = q; break; } }
        }
        if (!end) break;

        size_t olen = end - p + 1;
        char *obj = malloc(olen + 1);
        memcpy(obj, p, olen);
        obj[olen] = '\0';

        char *type = json_extract_string(obj, "type");
        char *sched = json_extract_string(obj, "schedule");

        if (type && sched) {
            cron_job_t *j = &g_jobs[g_job_count];
            j->id = json_extract_int(obj, "id", g_next_id);
            j->type = type;
            j->schedule = sched;
            j->description = json_extract_string(obj, "description");
            if (!j->description) j->description = strdup("");
            j->command = json_extract_string(obj, "command");
            if (!j->command) j->command = strdup("");
            j->at_job_id = json_extract_int(obj, "at_job_id", -1);
            if (j->id >= g_next_id) g_next_id = j->id + 1;
            g_job_count++;
        } else {
            free(type);
            free(sched);
        }

        free(obj);
        p = end + 1;
    }

done:
    free(buf);
    return g_job_count;
}

static int save_jobs(void)
{
    FILE *f = fopen(g_jobs_path, "w");
    if (!f) return -1;

    fputs("[\n", f);
    for (int i = 0; i < g_job_count; i++) {
        cron_job_t *j = &g_jobs[i];
        fprintf(f, "  {\"id\":%d,\"type\":\"%s\",\"description\":\"%s\","
                   "\"schedule\":\"%s\",\"command\":\"%s\"",
                j->id, j->type, j->description, j->schedule, j->command);
        if (j->at_job_id >= 0)
            fprintf(f, ",\"at_job_id\":%d", j->at_job_id);
        fprintf(f, "}%s\n", i < g_job_count - 1 ? "," : "");
    }
    fputs("]\n", f);
    fclose(f);
    return 0;
}

/* ---- Helpers ---- */

static int is_empty_response(const char *text)
{
    if (!text || !text[0]) return 1;
    const char *p = text;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '\0') return 1;
    if (strncasecmp(p, "NONE", 4) == 0) return 1;
    return 0;
}

/* ---- Init / cleanup ---- */

int cron_agent_init_with_deps(const char *storage_dir, const cron_agent_deps_t *deps)
{
    if (!storage_dir || !deps) return -1;

    g_deps = *deps;
    g_has_api = (deps->api_chat != NULL);

    snprintf(g_storage_dir, sizeof(g_storage_dir), "%s", storage_dir);
    snprintf(g_jobs_path, sizeof(g_jobs_path), "%s/jobs.json", storage_dir);
    mkdir(storage_dir, 0755);

    load_jobs();
    g_ready = 1;
    return 0;
}

#ifndef AGENT_TESTING
extern char *llm_global_mem_api_chat(const char *, const char *, int, const char *);
#endif

int cron_agent_init(void *config)
{
    (void)config;

#ifdef AGENT_TESTING
    return -1;  /* tests use cron_agent_init_with_deps() */
#else
    const char *home = getenv("HOME");
    char crondir[4096];
    snprintf(crondir, sizeof(crondir), "%s/.aibash_cron", home ? home : ".");

    cron_agent_deps_t deps = {
        .api_chat      = llm_global_mem_api_chat,
        .crontab_sync  = NULL,
        .at_create     = NULL,
        .at_remove     = NULL,
    };

    return cron_agent_init_with_deps(crondir, &deps);
#endif
}

void cron_agent_cleanup(void)
{
    for (int i = 0; i < g_job_count; i++)
        free_job(&g_jobs[i]);
    g_job_count = 0;
    g_next_id = 1;
    g_ready = 0;
    g_has_api = 0;
}

/* ---- User commands ---- */

int cron_agent_add(const char *type, const char *schedule,
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

    /* Create system at job if deps available */
    if (strcmp(type, "at") == 0 && g_deps.at_create) {
        int at_id = g_deps.at_create(schedule, command);
        if (at_id >= 0) j->at_job_id = at_id;
    }

    g_job_count++;
    save_jobs();

    /* Sync crontab if deps available */
    if (strcmp(type, "cron") == 0 && g_deps.crontab_sync)
        g_deps.crontab_sync(NULL);

    return j->id;
}

int cron_agent_remove(int id)
{
    if (!g_ready) return -1;

    for (int i = 0; i < g_job_count; i++) {
        if (g_jobs[i].id != id) continue;

        /* Remove system at job */
        if (strcmp(g_jobs[i].type, "at") == 0 &&
            g_jobs[i].at_job_id >= 0 && g_deps.at_remove)
            g_deps.at_remove(g_jobs[i].at_job_id);

        free_job(&g_jobs[i]);
        for (int k = i; k < g_job_count - 1; k++)
            g_jobs[k] = g_jobs[k + 1];
        g_job_count--;

        save_jobs();
        if (g_deps.crontab_sync) g_deps.crontab_sync(NULL);
        return 0;
    }
    return -1;
}

/* Convert a 5-field cron schedule to English */
static const char *dow_names[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
static const char *month_names[] = {"","January","February","March","April","May","June",
                                     "July","August","September","October","November","December"};

static char *cron_to_english(const char *schedule)
{
    int minute, hour, dom, mon, dow;
    char buf[256];

    /* Try to parse 5 fields: min hour dom mon dow */
    if (sscanf(schedule, "%d %d %d %d %d", &minute, &hour, &dom, &mon, &dow) == 5) {
        /* Specific date + time */
        snprintf(buf, sizeof(buf), "%s %d at %d:%02d",
                 (mon >= 1 && mon <= 12) ? month_names[mon] : "?", dom, hour, minute);
    } else {
        char s_min[16], s_hour[16], s_dom[16], s_mon[16], s_dow[16];
        if (sscanf(schedule, "%15s %15s %15s %15s %15s", s_min, s_hour, s_dom, s_mon, s_dow) != 5)
            return strdup(schedule);  /* can't parse, return raw */

        int m = atoi(s_min);
        int h = atoi(s_hour);

        if (strcmp(s_dom, "*") == 0 && strcmp(s_mon, "*") == 0 && strcmp(s_dow, "*") == 0) {
            /* Every day */
            snprintf(buf, sizeof(buf), "Every day at %d:%02d", h, m);
        } else if (strcmp(s_dom, "*") == 0 && strcmp(s_mon, "*") == 0 && strcmp(s_dow, "0") != 0 && s_dow[0] != '*') {
            /* Specific day of week */
            int d = atoi(s_dow);
            snprintf(buf, sizeof(buf), "Every %s at %d:%02d",
                     (d >= 0 && d <= 6) ? dow_names[d] : s_dow, h, m);
        } else {
            /* Complex — build a simple description */
            snprintf(buf, sizeof(buf), "Scheduled (%s) at %d:%02d", schedule, h, m);
        }
    }

    return strdup(buf);
}

/* Format a job as a human-readable line */
static int format_job(char *buf, size_t bufsz, cron_job_t *j)
{
    if (strcmp(j->type, "cron") == 0) {
        char *when = cron_to_english(j->schedule);
        int n = snprintf(buf, bufsz, "[%d] %s: %s (runs: %s)\n",
                         j->id, when, j->description, j->command);
        free(when);
        return n;
    } else {
        /* at job — schedule is already a datetime string */
        return snprintf(buf, bufsz, "[%d] %s: %s (runs: %s)\n",
                        j->id, j->schedule, j->description, j->command);
    }
}

char *cron_agent_list(void)
{
    if (!g_ready || g_job_count == 0)
        return strdup("(no scheduled tasks)\n");

    char *buf = malloc(4096);
    size_t off = 0;
    for (int i = 0; i < g_job_count; i++) {
        off += format_job(buf + off, 4096 - off, &g_jobs[i]);
    }
    return buf;
}

int cron_agent_count(void)
{
    return g_ready ? g_job_count : 0;
}

int cron_agent_ready(void)
{
    return g_ready;
}

/* ---- Side agent callbacks ---- */

char *cron_agent_pre_query(const char *query, const char *cwd)
{
    (void)cwd;
    if (!query || !query[0] || g_job_count == 0) return NULL;

    /*
     * Ask the LLM a simple yes/no: is this query schedule-related?
     * If yes, inject the full English job list.
     * If no, return NULL (saves context tokens).
     */
    if (g_has_api) {
        char *answer = g_deps.api_chat(CLASSIFY_PROMPT, query, 0, "cron-classify");
        if (!answer) return NULL;

        /* Check for YES */
        const char *p = answer;
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        int is_yes = (strncasecmp(p, "YES", 3) == 0);
        free(answer);

        if (!is_yes) return NULL;
    }

    /* Build the full English job list */
    char *job_list = cron_agent_list();
    if (!job_list) return NULL;

    time_t now = time(NULL);
    char timestr[80];
    strftime(timestr, sizeof(timestr), "%A, %B %d %Y at %H:%M %Z", localtime(&now));

    size_t len = strlen(timestr) + strlen(job_list) + 128;
    char *result = malloc(len);
    snprintf(result, len,
             "The user's scheduled tasks (current time: %s):\n%s",
             timestr, job_list);
    free(job_list);

    return result;
}

void cron_agent_post_query(const char *query, const char *response, const char *cwd)
{
    (void)cwd;
    if (!g_has_api || !g_ready) return;

    size_t conv_len = strlen(query) + strlen(response) + 64;
    char *conversation = malloc(conv_len);
    snprintf(conversation, conv_len, "User: %s\nAssistant: %s", query, response);

    char *result = g_deps.api_chat(EXTRACT_PROMPT, conversation, 0, "cron-extract");
    free(conversation);

    if (!result || !result[0]) { free(result); return; }

    /* Parse JSON array of scheduling operations */
    const char *p = result;
    while (*p && *p != '[') p++;
    if (!*p) { free(result); return; }
    p++;

    while (*p) {
        while (*p && *p != '{') {
            if (*p == ']') goto done;
            p++;
        }
        if (!*p) break;

        int depth = 0;
        const char *end = NULL;
        for (const char *q = p; *q; q++) {
            if (*q == '{') depth++;
            else if (*q == '}') { depth--; if (depth == 0) { end = q; break; } }
        }
        if (!end) break;

        size_t olen = end - p + 1;
        char *obj = malloc(olen + 1);
        memcpy(obj, p, olen);
        obj[olen] = '\0';

        char *type = json_extract_string(obj, "type");
        char *sched = json_extract_string(obj, "schedule");
        char *cmd = json_extract_string(obj, "command");
        char *desc = json_extract_string(obj, "description");

        if (type && sched && cmd) {
            cron_agent_add(type, sched, cmd, desc ? desc : "");
        }

        free(type); free(sched); free(cmd); free(desc);
        free(obj);
        p = end + 1;
    }

done:
    free(result);
}
