#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>

#include "llm_side_agent.h"
#include "llm_global_mem_agent.h"
#include "llm_streams.h"

/* ---- Registry ---- */

static side_agent_t g_agents[SIDE_AGENT_MAX];
static int g_agent_count = 0;

int side_agent_register(const side_agent_t *agent)
{
    if (!agent || g_agent_count >= SIDE_AGENT_MAX)
        return -1;
    g_agents[g_agent_count++] = *agent;  /* struct copy */
    return 0;
}

/* ---- Pre-query: parallel fork/pipe/select ---- */

typedef struct {
    int fd;         /* pipe read end */
    pid_t pid;      /* child pid */
    int agent_idx;  /* index into g_agents */
    char *result;   /* read result */
    int done;       /* 1 if read complete */
} pre_query_slot_t;

char *side_agents_pre_query(const char *query, const char *cwd)
{
    if (!query || !query[0]) return NULL;

    /* Collect enabled pre_query agents */
    int slots_count = 0;
    pre_query_slot_t slots[SIDE_AGENT_MAX];

    for (int i = 0; i < g_agent_count; i++) {
        if (!g_agents[i].enabled || !g_agents[i].pre_query)
            continue;

        int pipefd[2];
        if (pipe(pipefd) < 0) continue;

        pid_t pid = fork();
        if (pid < 0) {
            close(pipefd[0]);
            close(pipefd[1]);
            continue;
        }

        if (pid == 0) {
            /* Child process */
            close(pipefd[0]);
            close(STDIN_FILENO);

            char *result = g_agents[i].pre_query(query, cwd);
            if (result) {
                size_t len = strlen(result);
                size_t written = 0;
                while (written < len) {
                    ssize_t n = write(pipefd[1], result + written, len - written);
                    if (n <= 0) break;
                    written += n;
                }
                free(result);
            }
            close(pipefd[1]);
            _exit(0);
        }

        /* Parent */
        close(pipefd[1]);
        slots[slots_count].fd = pipefd[0];
        slots[slots_count].pid = pid;
        slots[slots_count].agent_idx = i;
        slots[slots_count].result = NULL;
        slots[slots_count].done = 0;
        slots_count++;
    }

    if (slots_count == 0)
        return NULL;

    /* Compute deadline from max timeout */
    int max_timeout = 0;
    for (int i = 0; i < slots_count; i++) {
        int t = g_agents[slots[i].agent_idx].timeout_sec;
        if (t <= 0) t = SIDE_AGENT_DEFAULT_TIMEOUT;
        if (t > max_timeout) max_timeout = t;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += max_timeout;

    /* Select loop: read from pipes as they become ready */
    int n_pending = slots_count;
    while (n_pending > 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remaining_ms = (deadline.tv_sec - now.tv_sec) * 1000
                          + (deadline.tv_nsec - now.tv_nsec) / 1000000;
        if (remaining_ms <= 0) break;

        fd_set readfds;
        FD_ZERO(&readfds);
        int maxfd = -1;
        for (int i = 0; i < slots_count; i++) {
            if (!slots[i].done) {
                FD_SET(slots[i].fd, &readfds);
                if (slots[i].fd > maxfd) maxfd = slots[i].fd;
            }
        }
        if (maxfd < 0) break;

        struct timeval tv;
        tv.tv_sec = remaining_ms / 1000;
        tv.tv_usec = (remaining_ms % 1000) * 1000;

        int ret = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (ret <= 0) break;

        for (int i = 0; i < slots_count; i++) {
            if (slots[i].done) continue;
            if (!FD_ISSET(slots[i].fd, &readfds)) continue;

            /* Read all available data */
            char *buf = malloc(SIDE_AGENT_BUF_SIZE);
            if (!buf) { slots[i].done = 1; n_pending--; continue; }

            size_t total = 0;
            ssize_t n;
            while ((n = read(slots[i].fd, buf + total,
                             SIDE_AGENT_BUF_SIZE - total - 1)) > 0) {
                total += n;
                if (total >= SIDE_AGENT_BUF_SIZE - 1) break;
            }

            if (total > 0) {
                buf[total] = '\0';
                slots[i].result = buf;
            } else {
                free(buf);
            }

            slots[i].done = 1;
            n_pending--;
        }
    }

    /* Close pipes and reap children */
    for (int i = 0; i < slots_count; i++) {
        close(slots[i].fd);
        if (slots[i].pid > 0) {
            kill(slots[i].pid, SIGTERM);
            waitpid(slots[i].pid, NULL, WNOHANG);
            /* Second attempt in case first was too early */
            waitpid(slots[i].pid, NULL, WNOHANG);
        }
    }

    /* Combine results: wrap each in [name context]...[end name context] */
    size_t total_len = 0;
    for (int i = 0; i < slots_count; i++) {
        if (slots[i].result) {
            const char *name = g_agents[slots[i].agent_idx].name;
            /* [name context]\n + result + [end name context]\n */
            total_len += strlen(name) * 2 + strlen(slots[i].result) + 32;
        }
    }

    if (total_len == 0) return NULL;

    char *combined = malloc(total_len + 1);
    if (!combined) {
        for (int i = 0; i < slots_count; i++) free(slots[i].result);
        return NULL;
    }

    size_t off = 0;
    for (int i = 0; i < slots_count; i++) {
        if (!slots[i].result) continue;
        const char *name = g_agents[slots[i].agent_idx].name;

        off += snprintf(combined + off, total_len + 1 - off,
                        "[%s context]\n%s\n[end %s context]\n",
                        name, slots[i].result, name);

        /* Debug output is handled by the callback itself (e.g.,
         * stream_global_mem_agent_output in the forked child).
         * No need to duplicate here. */
        free(slots[i].result);
    }

    return combined;
}

/* ---- Post-query: double-fork per agent ---- */

void side_agents_post_query(const char *query, const char *response, const char *cwd)
{
    if (!query || !response) return;

    for (int i = 0; i < g_agent_count; i++) {
        if (!g_agents[i].enabled || !g_agents[i].post_query)
            continue;

        fflush(stdout);
        fflush(stderr);

        /*
         * Double-fork to avoid zombies:
         * Parent -> Child (exits immediately) -> Grandchild (does work)
         */
        pid_t pid = fork();
        if (pid < 0) continue;

        if (pid > 0) {
            /* Parent: reap the intermediate child */
            waitpid(pid, NULL, 0);
            continue;
        }

        /* Child: fork again and exit */
        pid_t pid2 = fork();
        if (pid2 > 0) _exit(0);     /* child exits, parent reaps it */
        if (pid2 < 0) _exit(1);     /* fork failed */

        /* Grandchild: do the actual work */
        close(STDIN_FILENO);
        g_agents[i].post_query(query, response, cwd);
        _exit(0);
    }
}

/* ---- Initialization ---- */

void side_agent_init(void)
{
    g_agent_count = 0;

    /* Register global memory agent (pre-query search + post-query extraction) */
    if (llm_global_mem_agent_ready()) {
        side_agent_register(&(side_agent_t){
            .name        = "global_memory",
            .timeout_sec = 5,
            .enabled     = 1,
            .pre_query   = global_mem_agent_pre_query_cb,
            .post_query  = global_mem_agent_post_query_cb,
        });
    }
}
