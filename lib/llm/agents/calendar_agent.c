#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#include "calendar_agent.h"

/* ---- State ---- */

static int g_ready = 0;
static int g_has_api = 0;
static char g_storage_dir[4096];
static char g_store_path[4096];
static calendar_agent_deps_t g_deps;

/* ---- LLM Prompts ---- */

static const char *CLASSIFY_PROMPT =
    "Is this query about calendar events, meetings, appointments, or plans?\n"
    "Answer only YES or NO.";

static const char *EXTRACT_PROMPT =
    "Extract NEW calendar events from the USER's message only.\n\n"
    "RULES:\n"
    "1. Only extract EXPLICIT NEW events the USER requested\n"
    "2. If nothing to add, return: []\n"
    "3. Output RAW JSON only\n"
    "4. Do NOT re-create events the assistant mentioned\n\n"
    "FORMAT:\n"
    "[{\"summary\": \"event name\", \"start\": \"YYYY-MM-DD HH:MM\", "
    "\"end\": \"YYYY-MM-DD HH:MM\", \"description\": \"details\"}]\n\n"
    "DO NOT add: questions about calendar, assistant responses.\n"
    "DO add: explicit requests like 'add meeting on Friday at 2pm'.";

/* ---- Helpers ---- */

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

/* ---- Init / cleanup ---- */

int calendar_agent_init_with_deps(const char *storage_dir,
                                  const calendar_agent_deps_t *deps)
{
    if (!storage_dir || !deps) return -1;

    g_deps = *deps;
    g_has_api = (deps->api_chat != NULL);

    snprintf(g_storage_dir, sizeof(g_storage_dir), "%s", storage_dir);
    snprintf(g_store_path, sizeof(g_store_path), "%s/calendar.ics", storage_dir);
    mkdir(storage_dir, 0755);

    if (g_deps.store_load)
        g_deps.store_load(g_store_path);

    g_ready = 1;
    return 0;
}

int calendar_agent_init(void *config)
{
    (void)config;
    return -1;  /* production uses agents_setup.c calling init_with_deps */
}

void calendar_agent_cleanup(void)
{
    if (g_ready && g_deps.store_cleanup)
        g_deps.store_cleanup();
    g_ready = 0;
    g_has_api = 0;
}

/* ---- User commands ---- */

int calendar_agent_add(const char *summary, const char *start,
                       const char *end, const char *description)
{
    if (!g_ready || !g_deps.store_add) return -1;
    return g_deps.store_add(summary, start, end ? end : "", description ? description : "");
}

int calendar_agent_remove(int id)
{
    if (!g_ready || !g_deps.store_remove) return -1;
    return g_deps.store_remove(id);
}

char *calendar_agent_list(void)
{
    if (!g_ready || !g_deps.store_list) return strdup("(calendar not initialized)\n");
    return g_deps.store_list();
}

int calendar_agent_count(void)
{
    if (!g_ready || !g_deps.store_count) return 0;
    return g_deps.store_count();
}

int calendar_agent_ready(void)
{
    return g_ready;
}

/* ---- Side agent callbacks ---- */

char *calendar_agent_pre_query(const char *query, const char *cwd)
{
    (void)cwd;
    if (!query || !query[0]) return NULL;
    if (!g_deps.store_count || g_deps.store_count() == 0) return NULL;

    /* Classify: is this about calendar/meetings/appointments? */
    if (g_has_api) {
        char *answer = g_deps.api_chat(CLASSIFY_PROMPT, query, 0, "cal-classify");
        if (!answer) return NULL;

        const char *p = answer;
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        int is_yes = (strncasecmp(p, "YES", 3) == 0);
        free(answer);

        if (!is_yes) return NULL;
    }

    /* Build English event list */
    char *event_list = g_deps.store_list();
    if (!event_list) return NULL;

    time_t now = time(NULL);
    char timestr[80];
    strftime(timestr, sizeof(timestr), "%A, %B %d %Y at %H:%M %Z", localtime(&now));

    size_t len = strlen(timestr) + strlen(event_list) + 128;
    char *result = malloc(len);
    snprintf(result, len,
             "The user's calendar events (current time: %s):\n%s",
             timestr, event_list);
    free(event_list);

    return result;
}

void calendar_agent_post_query(const char *query, const char *response, const char *cwd)
{
    (void)cwd;
    if (!g_has_api || !g_ready) return;

    size_t conv_len = strlen(query) + strlen(response) + 64;
    char *conversation = malloc(conv_len);
    snprintf(conversation, conv_len, "User: %s\nAssistant: %s", query, response);

    char *result = g_deps.api_chat(EXTRACT_PROMPT, conversation, 0, "cal-extract");
    free(conversation);

    if (!result || !result[0]) { free(result); return; }

    /* Parse JSON array */
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

        char *summary = json_extract_string(obj, "summary");
        char *start = json_extract_string(obj, "start");
        char *endtime = json_extract_string(obj, "end");
        char *desc = json_extract_string(obj, "description");

        if (summary && start) {
            calendar_agent_add(summary, start, endtime, desc);
        }

        free(summary); free(start); free(endtime); free(desc);
        free(obj);
        p = end + 1;
    }

done:
    free(result);
}
