#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>

#include "llm_mem_api.h"
#include "cJSON.h"

/* Own connection state -- independent of main agent's llm_api.c */
static char *g_mem_api_url = NULL;
static char *g_mem_model   = NULL;
static char *g_mem_api_key = NULL;

/* Logging: set to non-NULL path to enable */
static char *g_mem_log_file = NULL;

int llm_mem_api_init(const char *api_url, const char *model, const char *api_key)
{
    free(g_mem_api_url);
    free(g_mem_model);
    free(g_mem_api_key);
    g_mem_api_url = api_url ? strdup(api_url) : NULL;
    g_mem_model   = model ? strdup(model) : NULL;
    g_mem_api_key = api_key ? strdup(api_key) : NULL;

    /* Enable logging: ~/.aibash_memories/api_log.jsonl */
    free(g_mem_log_file);
    const char *home = getenv("HOME");
    if (home) {
        size_t len = strlen(home) + 64;
        g_mem_log_file = malloc(len);
        snprintf(g_mem_log_file, len, "%s/.aibash_memories/api_log.jsonl", home);
    }

    return 0;
}

void llm_mem_api_cleanup(void)
{
    free(g_mem_api_url);  g_mem_api_url = NULL;
    free(g_mem_model);    g_mem_model = NULL;
    free(g_mem_api_key);  g_mem_api_key = NULL;
    free(g_mem_log_file); g_mem_log_file = NULL;
}

/* Curl write callback */
struct mem_buf {
    char *data;
    size_t size;
};

static size_t mem_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct mem_buf *buf = userdata;
    size_t total = size * nmemb;
    char *tmp = realloc(buf->data, buf->size + total + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

/*
 * Log a request/response pair as a JSON object to the log file.
 * Each line is a complete JSON object (JSONL format).
 */
static void log_api_call(const char *caller, cJSON *request,
                          const char *raw_response, double elapsed_ms)
{
    if (!g_mem_log_file) return;

    FILE *f = fopen(g_mem_log_file, "a");
    if (!f) return;

    cJSON *entry = cJSON_CreateObject();

    /* Timestamp */
    time_t now = time(NULL);
    char timestr[64];
    strftime(timestr, sizeof(timestr), "%Y-%m-%dT%H:%M:%SZ", localtime(&now));
    cJSON_AddStringToObject(entry, "time", timestr);
    cJSON_AddStringToObject(entry, "caller", caller);
    cJSON_AddNumberToObject(entry, "elapsed_ms", elapsed_ms);

    /* Request (pretty-printed) */
    if (request) {
        cJSON *req_copy = cJSON_Duplicate(request, 1);
        cJSON_AddItemToObject(entry, "request", req_copy);
    }

    /* Response (parse to JSON if valid, otherwise raw string) */
    if (raw_response) {
        cJSON *resp_json = cJSON_Parse(raw_response);
        if (resp_json)
            cJSON_AddItemToObject(entry, "response", resp_json);
        else
            cJSON_AddStringToObject(entry, "response_raw", raw_response);
    } else {
        cJSON_AddStringToObject(entry, "response", "(null)");
    }

    char *log_line = cJSON_Print(entry);
    if (log_line) {
        fputs(log_line, f);
        fputs("\n", f);
        free(log_line);
    }

    cJSON_Delete(entry);
    fclose(f);
}

char *llm_mem_api_chat(const char *system_prompt, const char *user_message,
                       int enable_thinking)
{
    if (!g_mem_api_url || !g_mem_model) return NULL;

    /* Build messages array */
    cJSON *messages = cJSON_CreateArray();

    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", system_prompt);
    cJSON_AddItemToArray(messages, sys_msg);

    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", user_message);
    cJSON_AddItemToArray(messages, user_msg);

    /* Build request body */
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", g_mem_model);
    cJSON_AddItemToObject(req, "messages", messages);
    cJSON_AddNumberToObject(req, "max_tokens", 1024);
    cJSON_AddNumberToObject(req, "temperature", 0.1);

    /* Control thinking mode for Qwen3.5 models */
    cJSON *kwargs = cJSON_CreateObject();
    cJSON_AddBoolToObject(kwargs, "enable_thinking", enable_thinking ? 1 : 0);
    cJSON_AddItemToObject(req, "chat_template_kwargs", kwargs);

    /* More tokens needed when thinking is enabled */
    if (enable_thinking)
        cJSON_ReplaceItemInObject(req, "max_tokens",
            cJSON_CreateNumber(4096));

    /* Keep a copy for logging before PrintUnformatted consumes it */
    cJSON *req_for_log = cJSON_Duplicate(req, 1);

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) { cJSON_Delete(req_for_log); return NULL; }

    /* Set up curl */
    CURL *curl = curl_easy_init();
    if (!curl) { free(body); cJSON_Delete(req_for_log); return NULL; }

    struct mem_buf response = {NULL, 0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (g_mem_api_key) {
        char auth[512];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", g_mem_api_key);
        headers = curl_slist_append(headers, auth);
    }

    curl_easy_setopt(curl, CURLOPT_URL, g_mem_api_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    /* Time the request */
    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    CURLcode res = curl_easy_perform(curl);

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0
                      + (t_end.tv_nsec - t_start.tv_nsec) / 1e6;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);

    if (res != CURLE_OK) {
        /* Log failed request */
        log_api_call("mem_api_chat", req_for_log, "(curl error)", elapsed_ms);
        cJSON_Delete(req_for_log);
        free(response.data);
        return NULL;
    }

    /* Log the request/response */
    log_api_call("mem_api_chat", req_for_log, response.data, elapsed_ms);
    cJSON_Delete(req_for_log);

    /* Parse response -- extract content string */
    char *result = NULL;
    cJSON *json = cJSON_Parse(response.data);
    if (json) {
        cJSON *choices = cJSON_GetObjectItem(json, "choices");
        cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
        cJSON *message = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;
        if (message) {
            cJSON *content = cJSON_GetObjectItem(message, "content");
            if (content && cJSON_IsString(content) && content->valuestring[0])
                result = strdup(content->valuestring);
        }
        cJSON_Delete(json);
    }

    free(response.data);
    return result;
}
