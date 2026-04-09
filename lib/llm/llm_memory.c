#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>
#include <ctype.h>

#include "llm_memory.h"
#include "llm_streams.h"
#include "cJSON.h"

static mem_entry_t *g_memories = NULL;
static int g_mem_count = 0;
static int g_mem_cap = 0;
static int g_mem_max = 200;
static int g_next_id = 1;
static char *g_memory_file = NULL;

/* ---- Persistence ---- */

static void memory_save_to_disk(void)
{
    if (!g_memory_file) return;

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < g_mem_count; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "id", g_memories[i].id);
        cJSON_AddStringToObject(entry, "content", g_memories[i].content);
        if (g_memories[i].keywords)
            cJSON_AddStringToObject(entry, "keywords", g_memories[i].keywords);
        if (g_memories[i].created)
            cJSON_AddStringToObject(entry, "created", g_memories[i].created);
        cJSON_AddItemToArray(arr, entry);
    }

    char *json = cJSON_Print(arr);
    cJSON_Delete(arr);
    if (!json) return;

    FILE *f = fopen(g_memory_file, "w");
    if (f) {
        fputs(json, f);
        fclose(f);
    }
    free(json);
}

static void memory_load_from_disk(void)
{
    if (!g_memory_file) return;

    FILE *f = fopen(g_memory_file, "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len <= 0) { fclose(f); return; }
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return; }
    size_t nread = fread(buf, 1, len, f);
    fclose(f);
    buf[nread] = '\0';

    cJSON *arr = cJSON_Parse(buf);
    free(buf);
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return;
    }

    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n && g_mem_count < g_mem_max; i++) {
        cJSON *entry = cJSON_GetArrayItem(arr, i);
        if (!entry) continue;

        cJSON *j_id = cJSON_GetObjectItem(entry, "id");
        cJSON *j_content = cJSON_GetObjectItem(entry, "content");
        cJSON *j_keywords = cJSON_GetObjectItem(entry, "keywords");
        cJSON *j_created = cJSON_GetObjectItem(entry, "created");

        if (!j_content || !cJSON_IsString(j_content)) continue;

        if (g_mem_count >= g_mem_cap) {
            g_mem_cap = (g_mem_cap == 0) ? 64 : g_mem_cap * 2;
            g_memories = realloc(g_memories, g_mem_cap * sizeof(mem_entry_t));
        }

        int id = (j_id && cJSON_IsNumber(j_id)) ? j_id->valueint : g_next_id;
        g_memories[g_mem_count].id = id;
        g_memories[g_mem_count].content = strdup(j_content->valuestring);
        g_memories[g_mem_count].keywords =
            (j_keywords && cJSON_IsString(j_keywords)) ? strdup(j_keywords->valuestring) : NULL;
        g_memories[g_mem_count].created =
            (j_created && cJSON_IsString(j_created)) ? strdup(j_created->valuestring) : NULL;
        g_mem_count++;

        if (id >= g_next_id)
            g_next_id = id + 1;
    }

    cJSON_Delete(arr);
}

/* ---- Helpers ---- */

static char *now_iso(void)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char *buf = malloc(32);
    strftime(buf, 32, "%Y-%m-%dT%H:%M:%SZ", tm);
    return buf;
}

/*
 * Auto-generate keywords from content.
 * Extracts words >= 3 chars, lowercased, comma-separated.
 */
static char *auto_keywords(const char *content)
{
    char *result = NULL;
    size_t rlen = 0, rcap = 0;
    const char *p = content;

    while (*p) {
        /* Skip non-alpha */
        while (*p && !isalpha((unsigned char)*p)) p++;
        if (!*p) break;

        /* Extract word */
        const char *start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '-' || *p == '_')) p++;
        size_t wlen = p - start;

        if (wlen >= 3) {
            /* Add comma separator */
            if (rlen > 0) {
                if (rlen + 1 >= rcap) { rcap = (rcap == 0) ? 128 : rcap * 2; result = realloc(result, rcap); }
                result[rlen++] = ',';
            }
            /* Add lowercased word */
            if (rlen + wlen + 1 >= rcap) { rcap = (rcap == 0) ? 128 : rcap * 2; while (rlen + wlen + 1 >= rcap) rcap *= 2; result = realloc(result, rcap); }
            for (size_t i = 0; i < wlen; i++)
                result[rlen++] = tolower((unsigned char)start[i]);
        }
    }

    if (result) result[rlen] = '\0';
    return result;
}

/*
 * Score how well a memory matches a query.
 * Returns number of query words that match in content or keywords.
 */
static int match_score(const mem_entry_t *mem, const char *query)
{
    int score = 0;
    char *qcopy = strdup(query);

    /* Tokenize query into words */
    char *saveptr = NULL;
    char *word = strtok_r(qcopy, " \t,", &saveptr);
    while (word) {
        if (strlen(word) >= 2) {
            if (mem->content && strcasestr(mem->content, word))
                score++;
            if (mem->keywords && strcasestr(mem->keywords, word))
                score++;
        }
        word = strtok_r(NULL, " \t,", &saveptr);
    }

    free(qcopy);
    return score;
}

/* ---- Public API ---- */

int llm_memory_init(const char *memory_dir, int max_entries)
{
    g_mem_max = max_entries > 0 ? max_entries : 200;
    g_mem_count = 0;
    g_next_id = 1;

    /* Create directory if needed */
    mkdir(memory_dir, 0755);

    /* Build file path */
    free(g_memory_file);
    size_t len = strlen(memory_dir) + 32;
    g_memory_file = malloc(len);
    snprintf(g_memory_file, len, "%s/memories.json", memory_dir);

    /* Load existing memories */
    memory_load_from_disk();
    return g_mem_count;
}

void llm_memory_cleanup(void)
{
    for (int i = 0; i < g_mem_count; i++) {
        free(g_memories[i].content);
        free(g_memories[i].keywords);
        free(g_memories[i].created);
    }
    g_mem_count = 0;
    free(g_memory_file);
    g_memory_file = NULL;
}

int llm_memory_save(const char *content, const char *keywords)
{
    if (!content || !content[0]) return -1;

    /* Evict oldest if at capacity */
    if (g_mem_count >= g_mem_max) {
        free(g_memories[0].content);
        free(g_memories[0].keywords);
        free(g_memories[0].created);
        memmove(&g_memories[0], &g_memories[1], (g_mem_count - 1) * sizeof(mem_entry_t));
        g_mem_count--;
    }

    /* Grow if needed */
    if (g_mem_count >= g_mem_cap) {
        g_mem_cap = (g_mem_cap == 0) ? 64 : g_mem_cap * 2;
        g_memories = realloc(g_memories, g_mem_cap * sizeof(mem_entry_t));
    }

    g_memories[g_mem_count].id = g_next_id++;
    g_memories[g_mem_count].content = strdup(content);
    g_memories[g_mem_count].keywords = keywords ? strdup(keywords) : auto_keywords(content);
    g_memories[g_mem_count].created = now_iso();
    g_mem_count++;

    memory_save_to_disk();
    return 0;
}

int llm_memory_forget(int id)
{
    for (int i = 0; i < g_mem_count; i++) {
        if (g_memories[i].id == id) {
            free(g_memories[i].content);
            free(g_memories[i].keywords);
            free(g_memories[i].created);
            memmove(&g_memories[i], &g_memories[i + 1],
                    (g_mem_count - i - 1) * sizeof(mem_entry_t));
            g_mem_count--;
            memory_save_to_disk();
            return 0;
        }
    }
    return -1;
}

int llm_memory_forget_match(const char *text)
{
    if (!text) return -1;
    int removed = 0;

    for (int i = g_mem_count - 1; i >= 0; i--) {
        if (strcasestr(g_memories[i].content, text)) {
            free(g_memories[i].content);
            free(g_memories[i].keywords);
            free(g_memories[i].created);
            memmove(&g_memories[i], &g_memories[i + 1],
                    (g_mem_count - i - 1) * sizeof(mem_entry_t));
            g_mem_count--;
            removed++;
        }
    }

    if (removed > 0) memory_save_to_disk();
    return removed > 0 ? 0 : -1;
}

char *llm_memory_search(const char *query)
{
    if (!query || !query[0] || g_mem_count == 0)
        return strdup("(no memories found)");

    /* Score all memories */
    typedef struct { int idx; int score; } scored_t;
    scored_t *scored = malloc(g_mem_count * sizeof(scored_t));
    int nscored = 0;

    for (int i = 0; i < g_mem_count; i++) {
        int s = match_score(&g_memories[i], query);
        if (s > 0) {
            scored[nscored].idx = i;
            scored[nscored].score = s;
            nscored++;
        }
    }

    if (nscored == 0) {
        free(scored);
        return strdup("(no matching memories)");
    }

    /* Sort by score descending (simple bubble sort, small N) */
    for (int i = 0; i < nscored - 1; i++)
        for (int j = i + 1; j < nscored; j++)
            if (scored[j].score > scored[i].score) {
                scored_t tmp = scored[i];
                scored[i] = scored[j];
                scored[j] = tmp;
            }

    /* Format results (top 20) */
    char *buf = NULL;
    size_t blen = 0, bcap = 0;
    int limit = nscored < 20 ? nscored : 20;

    for (int i = 0; i < limit; i++) {
        mem_entry_t *m = &g_memories[scored[i].idx];
        char line[1024];
        snprintf(line, sizeof(line), "[%d] %s\n", m->id, m->content);
        size_t llen = strlen(line);
        if (blen + llen + 1 > bcap) {
            bcap = (bcap == 0) ? 1024 : bcap * 2;
            buf = realloc(buf, bcap);
        }
        memcpy(buf + blen, line, llen);
        blen += llen;
    }

    free(scored);
    if (buf) buf[blen] = '\0';
    return buf ? buf : strdup("(no matching memories)");
}

char *llm_memory_list(void)
{
    if (g_mem_count == 0)
        return strdup("(no memories saved)");

    char *buf = NULL;
    size_t blen = 0, bcap = 0;

    for (int i = 0; i < g_mem_count; i++) {
        char line[1024];
        snprintf(line, sizeof(line), "[%d] %s\n",
                 g_memories[i].id, g_memories[i].content);
        size_t llen = strlen(line);
        if (blen + llen + 1 > bcap) {
            bcap = (bcap == 0) ? 1024 : bcap * 2;
            buf = realloc(buf, bcap);
        }
        memcpy(buf + blen, line, llen);
        blen += llen;
    }

    if (buf) buf[blen] = '\0';
    return buf ? buf : strdup("(no memories saved)");
}

int llm_memory_count(void)
{
    return g_mem_count;
}

char *llm_memory_whisper(const char *query)
{
    if (!query || !query[0] || g_mem_count == 0)
        return NULL;

    /* Score all memories against query */
    typedef struct { int idx; int score; } scored_t;
    scored_t *scored = malloc(g_mem_count * sizeof(scored_t));
    int nscored = 0;

    for (int i = 0; i < g_mem_count; i++) {
        int s = match_score(&g_memories[i], query);
        if (s > 0) {
            scored[nscored].idx = i;
            scored[nscored].score = s;
            nscored++;
        }
    }

    if (nscored == 0) {
        free(scored);
        return NULL;
    }

    /* Sort by score descending */
    for (int i = 0; i < nscored - 1; i++)
        for (int j = i + 1; j < nscored; j++)
            if (scored[j].score > scored[i].score) {
                scored_t tmp = scored[i];
                scored[i] = scored[j];
                scored[j] = tmp;
            }

    /* Build whisper text (limit ~500 tokens ≈ ~2000 chars) */
    char *buf = NULL;
    size_t blen = 0, bcap = 0;
    int limit = nscored < 10 ? nscored : 10;

    for (int i = 0; i < limit && blen < 2000; i++) {
        mem_entry_t *m = &g_memories[scored[i].idx];
        char line[512];
        snprintf(line, sizeof(line), "- %s\n", m->content);
        size_t llen = strlen(line);
        if (blen + llen + 1 > bcap) {
            bcap = (bcap == 0) ? 512 : bcap * 2;
            buf = realloc(buf, bcap);
        }
        memcpy(buf + blen, line, llen);
        blen += llen;
    }

    free(scored);

    if (buf) {
        buf[blen] = '\0';
        /* Debug output */
        stream_memory_output(buf);
    }
    return buf;
}

/* ---- Tool handlers ---- */

char *llm_memory_tool_save(const char *args_json)
{
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return strdup("error: invalid arguments");

    cJSON *j_content = cJSON_GetObjectItem(args, "content");
    if (!j_content || !cJSON_IsString(j_content)) {
        cJSON_Delete(args);
        return strdup("error: content required");
    }

    cJSON *j_keywords = cJSON_GetObjectItem(args, "keywords");
    const char *kw = (j_keywords && cJSON_IsString(j_keywords)) ? j_keywords->valuestring : NULL;

    if (llm_memory_save(j_content->valuestring, kw) == 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Saved to memory: %s", j_content->valuestring);
        cJSON_Delete(args);
        return strdup(msg);
    }

    cJSON_Delete(args);
    return strdup("error: failed to save memory");
}

char *llm_memory_tool_search(const char *args_json)
{
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return strdup("error: invalid arguments");

    cJSON *j_query = cJSON_GetObjectItem(args, "query");
    if (!j_query || !cJSON_IsString(j_query)) {
        cJSON_Delete(args);
        return strdup("error: query required");
    }

    char *result = llm_memory_search(j_query->valuestring);
    cJSON_Delete(args);
    return result;
}

char *llm_memory_tool_list(const char *args_json)
{
    (void)args_json;
    return llm_memory_list();
}

char *llm_memory_tool_forget(const char *args_json)
{
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return strdup("error: invalid arguments");

    cJSON *j_id = cJSON_GetObjectItem(args, "id");
    if (j_id && cJSON_IsNumber(j_id)) {
        int id = j_id->valueint;
        cJSON_Delete(args);
        if (llm_memory_forget(id) == 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Forgot memory #%d", id);
            return strdup(msg);
        }
        return strdup("error: memory not found");
    }

    cJSON *j_match = cJSON_GetObjectItem(args, "match");
    if (j_match && cJSON_IsString(j_match)) {
        const char *text = j_match->valuestring;
        cJSON_Delete(args);
        if (llm_memory_forget_match(text) == 0)
            return strdup("Forgot matching memories");
        return strdup("error: no matching memories found");
    }

    cJSON_Delete(args);
    return strdup("error: provide id or match text");
}
