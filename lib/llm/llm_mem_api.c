#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "llm_mem_api.h"
#include "cJSON.h"

/* Own connection state -- independent of main agent's llm_api.c */
static char *g_mem_api_url = NULL;
static char *g_mem_model   = NULL;
static char *g_mem_api_key = NULL;

int llm_mem_api_init(const char *api_url, const char *model, const char *api_key)
{
    free(g_mem_api_url);
    free(g_mem_model);
    free(g_mem_api_key);
    g_mem_api_url = api_url ? strdup(api_url) : NULL;
    g_mem_model   = model ? strdup(model) : NULL;
    g_mem_api_key = api_key ? strdup(api_key) : NULL;
    return 0;
}

void llm_mem_api_cleanup(void)
{
    free(g_mem_api_url);  g_mem_api_url = NULL;
    free(g_mem_model);    g_mem_model = NULL;
    free(g_mem_api_key);  g_mem_api_key = NULL;
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

char *llm_mem_api_chat(const char *system_prompt, const char *user_message)
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

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return NULL;

    /* Set up curl */
    CURL *curl = curl_easy_init();
    if (!curl) { free(body); return NULL; }

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

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);

    if (res != CURLE_OK) {
        free(response.data);
        return NULL;
    }

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
