#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <errno.h>

#include "llm_whisper.h"
#include "llm_mem_api.h"
#include "llm_memory.h"
#include "llm_streams.h"

#define WHISPER_TIMEOUT_SEC 5
#define WHISPER_BUF_SIZE    4096
#define WHISPER_CACHE_TTL   60  /* seconds */

/* Cache: avoid redundant LLM calls for similar queries */
static char *g_cache_query = NULL;
static char *g_cache_result = NULL;
static time_t g_cache_time = 0;

static const char *WHISPER_PROMPT =
    "Given the user's memories below, which ones DIRECTLY answer or relate "
    "to their specific query? Be strict -- only include memories that the "
    "user would need to answer this exact question. Do not include loosely "
    "related or tangential memories.\n\n"
    "Return ONLY the relevant memories as bullet points (- memory text).\n"
    "If none are directly relevant, return exactly: NONE\n"
    "No explanation, no markdown, just bullet points or NONE.";

/*
 * Build the user message: query + all memories.
 */
static char *build_agent_message(const char *query)
{
    char *mem_list = llm_memory_list();
    if (!mem_list || strcmp(mem_list, "(no memories saved)") == 0) {
        free(mem_list);
        return NULL;
    }

    size_t len = strlen(query) + strlen(mem_list) + 64;
    char *msg = malloc(len);
    snprintf(msg, len, "Query: %s\n\nMemories:\n%s", query, mem_list);
    free(mem_list);
    return msg;
}

/*
 * Check if a response is "NONE" or empty.
 */
static int is_empty_response(const char *text)
{
    if (!text || !text[0]) return 1;

    /* Skip whitespace */
    const char *p = text;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    if (*p == '\0') return 1;
    if (strncasecmp(p, "NONE", 4) == 0) return 1;

    return 0;
}

char *llm_whisper_agents(const char *query)
{
    if (!query || !query[0]) return NULL;

    /* Check cache: reuse recent result if query matches within TTL */
    time_t now = time(NULL);
    if (g_cache_result && g_cache_query
        && strcmp(g_cache_query, query) == 0
        && (now - g_cache_time) < WHISPER_CACHE_TTL) {
        stream_whisper_output("(cached)\n");
        return strdup(g_cache_result);
    }

    /* Build the message with all memories */
    char *agent_msg = build_agent_message(query);
    if (!agent_msg) return NULL;

    stream_whisper_output("searching memories...\n");

    /* Create pipe for the agent */
    int pipefd[2];
    if (pipe(pipefd) < 0) { free(agent_msg); return NULL; }

    /* Fork whisper agent */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child */
        close(pipefd[0]);
        close(STDIN_FILENO);

        char *result = llm_mem_api_chat(WHISPER_PROMPT, agent_msg, 0, "whisper");
        if (result) {
            write(pipefd[1], result, strlen(result));
            free(result);
        }
        close(pipefd[1]);
        free(agent_msg);
        _exit(0);
    }

    free(agent_msg);
    close(pipefd[1]);

    /* Parent: read with timeout */
    fd_set readfds;
    struct timeval tv;
    FD_ZERO(&readfds);
    FD_SET(pipefd[0], &readfds);
    tv.tv_sec = WHISPER_TIMEOUT_SEC;
    tv.tv_usec = 0;

    char *result = NULL;
    int ready = select(pipefd[0] + 1, &readfds, NULL, NULL, &tv);
    if (ready > 0) {
        char *buf = malloc(WHISPER_BUF_SIZE);
        size_t total = 0;
        ssize_t n;
        while ((n = read(pipefd[0], buf + total, WHISPER_BUF_SIZE - total - 1)) > 0) {
            total += n;
            if (total >= WHISPER_BUF_SIZE - 1) break;
        }
        if (total > 0) {
            buf[total] = '\0';
            result = buf;
        } else {
            free(buf);
        }
    }

    close(pipefd[0]);

    /* Reap child */
    if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, WNOHANG);
        waitpid(pid, NULL, WNOHANG);
    }

    if (is_empty_response(result)) {
        free(result);
        return NULL;
    }

    /* Update cache */
    free(g_cache_query);
    free(g_cache_result);
    g_cache_query = strdup(query);
    g_cache_result = strdup(result);
    g_cache_time = now;

    stream_whisper_output(result);
    return result;
}
