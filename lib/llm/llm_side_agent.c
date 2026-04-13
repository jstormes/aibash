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
#include "llm_cron_agent.h"
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

    /* Phase 1: Create all pipes up front */
    int slots_count = 0;
    pre_query_slot_t slots[SIDE_AGENT_MAX];
    int write_fds[SIDE_AGENT_MAX];  /* track write-ends for child cleanup */

    for (int i = 0; i < g_agent_count; i++) {
        if (!g_agents[i].enabled || !g_agents[i].pre_query)
            continue;

        int pipefd[2];
        if (pipe(pipefd) < 0) continue;

        slots[slots_count].fd = pipefd[0];
        slots[slots_count].pid = -1;
        slots[slots_count].agent_idx = i;
        slots[slots_count].result = NULL;
        slots[slots_count].done = 0;
        write_fds[slots_count] = pipefd[1];
        slots_count++;
    }

    /* Phase 2: Fork children sequentially — each gets a clean process */
    for (int s = 0; s < slots_count; s++) {
        pid_t pid = fork();
        if (pid < 0) {
            close(slots[s].fd);
            close(write_fds[s]);
            slots[s].done = 1;
            continue;
        }

        if (pid == 0) {
            /* Child: clean environment for curl */
            close(STDIN_FILENO);
            close(slots[s].fd);  /* close our read-end */
            signal(SIGPIPE, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);

            int my_fd = write_fds[s];
            int idx = slots[s].agent_idx;
            char *result = g_agents[idx].pre_query(query, cwd);
            if (result) {
                size_t len = strlen(result);
                size_t written = 0;
                while (written < len) {
                    ssize_t n = write(my_fd, result + written, len - written);
                    if (n <= 0) break;
                    written += n;
                }
                free(result);
            }
            close(my_fd);
            _exit(0);
        }

        /* Parent: close this write-end, record pid */
        close(write_fds[s]);
        slots[s].pid = pid;

        /*
         * Wait for this child to finish before forking the next one.
         * Multiple forked children using libcurl simultaneously hang
         * due to inherited bash process state (signal handlers, fds).
         * Serializing the forks avoids this — each agent still runs
         * in its own process, just not concurrently.
         */
        {
            fd_set rfds;
            struct timeval wait_tv;
            int t = g_agents[slots[s].agent_idx].timeout_sec;
            if (t <= 0) t = SIDE_AGENT_DEFAULT_TIMEOUT;
            FD_ZERO(&rfds);
            FD_SET(slots[s].fd, &rfds);
            wait_tv.tv_sec = t;
            wait_tv.tv_usec = 0;
            int r = select(slots[s].fd + 1, &rfds, NULL, NULL, &wait_tv);
            if (r > 0) {
                char *buf = malloc(SIDE_AGENT_BUF_SIZE);
                size_t total = 0;
                ssize_t n;
                while ((n = read(slots[s].fd, buf + total,
                                 SIDE_AGENT_BUF_SIZE - total - 1)) > 0) {
                    total += n;
                    if (total >= SIDE_AGENT_BUF_SIZE - 1) break;
                }
                if (total > 0) {
                    buf[total] = '\0';
                    slots[s].result = buf;
                } else {
                    free(buf);
                }
            }
            slots[s].done = 1;
            close(slots[s].fd);
            kill(slots[s].pid, SIGTERM);
            waitpid(slots[s].pid, NULL, 0);
        }
    }

    if (slots_count == 0)
        return NULL;

    /* All slots were read and reaped inline above (serial execution) */

    /* Combine results: wrap each in [name context]...[end name context] */
    size_t total_len = 0;
    for (int i = 0; i < slots_count; i++) {
        if (slots[i].result) {
            const char *name = g_agents[slots[i].agent_idx].name;
            /* ## name\n + result + \n */
            total_len += strlen(name) + strlen(slots[i].result) + 16;
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
                        "## %s\n%s\n",
                        name, slots[i].result);

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

    /* Register global memory agent */
    if (llm_global_mem_agent_ready()) {
        side_agent_register(&(side_agent_t){
            .name        = "global_memory",
            .timeout_sec = 5,
            .enabled     = 1,
            .pre_query   = global_mem_agent_pre_query_cb,
            .post_query  = global_mem_agent_post_query_cb,
        });
    }

    /* Register cron agent */
    if (llm_cron_agent_ready()) {
        side_agent_register(&(side_agent_t){
            .name        = "cron",
            .timeout_sec = 5,
            .enabled     = llm_global_mem_agent_ready(),
            .pre_query   = cron_agent_pre_query_cb,
            .post_query  = llm_global_mem_agent_ready() ? cron_agent_post_query_cb : NULL,
        });
    }
}
