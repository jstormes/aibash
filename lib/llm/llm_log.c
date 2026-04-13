#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "llm_log.h"
#include "cJSON.h"

static char *g_log_dir = NULL;

#define LOG_MAX_AGE_SEC (24 * 60 * 60)  /* 1 day */

/*
 * Remove log files older than LOG_MAX_AGE_SEC.
 */
static void cleanup_old_logs(void)
{
    if (!g_log_dir) return;

    DIR *dir = opendir(g_log_dir);
    if (!dir) return;

    time_t now = time(NULL);
    struct dirent *ent;

    while ((ent = readdir(dir))) {
        /* Only clean .json files */
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6 || strcmp(ent->d_name + nlen - 5, ".json") != 0)
            continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", g_log_dir, ent->d_name);

        struct stat st;
        if (stat(path, &st) == 0) {
            if (now - st.st_mtime > LOG_MAX_AGE_SEC)
                unlink(path);
        }
    }

    closedir(dir);
}

void llm_log_init(const char *log_dir)
{
    free(g_log_dir);
    g_log_dir = log_dir ? strdup(log_dir) : NULL;

    if (g_log_dir) {
        mkdir(g_log_dir, 0755);
        cleanup_old_logs();
    }
}

void llm_log_api_call(const char *caller, const char *request_body,
                       const char *response_body, double elapsed_ms)
{
    if (!g_log_dir || !caller) return;

    /* Build filename: YYYYMMDD_HHMMSS_<caller>.json */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char filename[4096];
    snprintf(filename, sizeof(filename),
             "%s/%04d%02d%02d_%02d%02d%02d_%s.json",
             g_log_dir,
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec,
             caller);

    /* Build log entry */
    cJSON *entry = cJSON_CreateObject();

    char timestr[64];
    strftime(timestr, sizeof(timestr), "%Y-%m-%dT%H:%M:%SZ", tm);
    cJSON_AddStringToObject(entry, "time", timestr);
    cJSON_AddStringToObject(entry, "caller", caller);
    cJSON_AddNumberToObject(entry, "elapsed_ms", elapsed_ms);

    /* Parse and re-format request for pretty printing */
    if (request_body) {
        cJSON *req = cJSON_Parse(request_body);
        if (req)
            cJSON_AddItemToObject(entry, "request", req);
        else
            cJSON_AddStringToObject(entry, "request_raw", request_body);
    }

    /* Parse and re-format response */
    if (response_body) {
        cJSON *resp = cJSON_Parse(response_body);
        if (resp)
            cJSON_AddItemToObject(entry, "response", resp);
        else
            cJSON_AddStringToObject(entry, "response_raw", response_body);
    } else {
        cJSON_AddStringToObject(entry, "response", "(null)");
    }

    /* Write pretty-printed JSON to file */
    char *json = cJSON_Print(entry);
    cJSON_Delete(entry);
    if (!json) return;

    FILE *f = fopen(filename, "w");
    if (f) {
        fputs(json, f);
        fputs("\n", f);
        fclose(f);
    }
    free(json);
}
