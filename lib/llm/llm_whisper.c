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

/* Agent 1: User preferences and personal context */
static const char *AGENT1_PROMPT =
    "Given the user's memories below, which ones are relevant to their query?\n"
    "Focus on: user preferences, habits, name, role, personal details.\n\n"
    "Return ONLY the relevant memories as bullet points (- memory text).\n"
    "If none are relevant, return exactly: NONE\n"
    "No explanation, no markdown, just bullet points or NONE.";

/* Agent 2: Project and technical context */
static const char *AGENT2_PROMPT =
    "Given the user's memories below, which ones are relevant to their query?\n"
    "Focus on: project details, tech stack, tools, deployment, infrastructure.\n\n"
    "Return ONLY the relevant memories as bullet points (- memory text).\n"
    "If none are relevant, return exactly: NONE\n"
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
 * Read from a pipe fd with timeout using select().
 * Returns malloced string or NULL on timeout/error.
 */
static char *read_pipe_timeout(int fd, int timeout_sec)
{
    fd_set readfds;
    struct timeval tv;

    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    int ready = select(fd + 1, &readfds, NULL, NULL, &tv);
    if (ready <= 0) return NULL;  /* timeout or error */

    char *buf = malloc(WHISPER_BUF_SIZE);
    if (!buf) return NULL;

    size_t total = 0;
    ssize_t n;
    while ((n = read(fd, buf + total, WHISPER_BUF_SIZE - total - 1)) > 0) {
        total += n;
        if (total >= WHISPER_BUF_SIZE - 1) break;
    }

    if (total == 0) {
        free(buf);
        return NULL;
    }

    buf[total] = '\0';
    return buf;
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

    /* Build the message with all memories */
    char *agent_msg = build_agent_message(query);
    if (!agent_msg) return NULL;

    stream_whisper_output("searching (2 agents)...\n");

    /* Create pipes for both agents */
    int pipe1[2], pipe2[2];
    if (pipe(pipe1) < 0) { free(agent_msg); return NULL; }
    if (pipe(pipe2) < 0) { close(pipe1[0]); close(pipe1[1]); free(agent_msg); return NULL; }

    /* Fork agent 1: user context */
    pid_t pid1 = fork();
    if (pid1 == 0) {
        /* Child 1 */
        close(pipe1[0]);
        close(pipe2[0]); close(pipe2[1]);
        close(STDIN_FILENO);

        char *result = llm_mem_api_chat(AGENT1_PROMPT, agent_msg, 0);
        if (result) {
            write(pipe1[1], result, strlen(result));
            free(result);
        }
        close(pipe1[1]);
        free(agent_msg);
        _exit(0);
    }

    /* Fork agent 2: project context */
    pid_t pid2 = fork();
    if (pid2 == 0) {
        /* Child 2 */
        close(pipe2[0]);
        close(pipe1[0]); close(pipe1[1]);
        close(STDIN_FILENO);

        char *result = llm_mem_api_chat(AGENT2_PROMPT, agent_msg, 0);
        if (result) {
            write(pipe2[1], result, strlen(result));
            free(result);
        }
        close(pipe2[1]);
        free(agent_msg);
        _exit(0);
    }

    free(agent_msg);

    /* Parent: close write ends */
    close(pipe1[1]);
    close(pipe2[1]);

    /* Read results with timeout using select() on both pipes */
    char *result1 = NULL;
    char *result2 = NULL;

    fd_set readfds;
    struct timeval tv;
    int maxfd = (pipe1[0] > pipe2[0] ? pipe1[0] : pipe2[0]) + 1;
    int pipes_open = 2;
    int fd1_open = 1, fd2_open = 1;

    /* Wait for both pipes or timeout */
    time_t start = time(NULL);
    while (pipes_open > 0) {
        int elapsed = (int)(time(NULL) - start);
        if (elapsed >= WHISPER_TIMEOUT_SEC) break;

        FD_ZERO(&readfds);
        if (fd1_open) FD_SET(pipe1[0], &readfds);
        if (fd2_open) FD_SET(pipe2[0], &readfds);
        tv.tv_sec = WHISPER_TIMEOUT_SEC - elapsed;
        tv.tv_usec = 0;

        int ready = select(maxfd, &readfds, NULL, NULL, &tv);
        if (ready <= 0) break;  /* timeout or error */

        if (fd1_open && FD_ISSET(pipe1[0], &readfds)) {
            result1 = read_pipe_timeout(pipe1[0], 1);
            fd1_open = 0;
            pipes_open--;
        }
        if (fd2_open && FD_ISSET(pipe2[0], &readfds)) {
            result2 = read_pipe_timeout(pipe2[0], 1);
            fd2_open = 0;
            pipes_open--;
        }
    }

    close(pipe1[0]);
    close(pipe2[0]);

    /* Kill any still-running children and reap */
    if (pid1 > 0) { kill(pid1, SIGTERM); waitpid(pid1, NULL, WNOHANG); }
    if (pid2 > 0) { kill(pid2, SIGTERM); waitpid(pid2, NULL, WNOHANG); }
    /* Final reap after a moment */
    waitpid(pid1, NULL, WNOHANG);
    waitpid(pid2, NULL, WNOHANG);

    /* Merge results */
    int have1 = !is_empty_response(result1);
    int have2 = !is_empty_response(result2);

    if (!have1 && !have2) {
        free(result1);
        free(result2);
        return NULL;
    }

    /* Build combined whisper text */
    size_t len = (have1 ? strlen(result1) : 0) + (have2 ? strlen(result2) : 0) + 4;
    char *merged = malloc(len);
    merged[0] = '\0';

    if (have1) {
        strcat(merged, result1);
        if (have1 && have2 && merged[strlen(merged) - 1] != '\n')
            strcat(merged, "\n");
        stream_whisper_output(result1);
    }
    if (have2) {
        strcat(merged, result2);
        stream_whisper_output(result2);
    }

    free(result1);
    free(result2);

    return merged;
}
