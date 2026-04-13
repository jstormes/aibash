#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>

#include "side_agent.h"

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

void side_agent_init(void *config)
{
    for (int i = 0; i < g_agent_count; i++) {
        if (g_agents[i].init) {
            if (g_agents[i].init(config) == 0)
                g_agents[i].enabled = 1;
            else
                g_agents[i].enabled = 0;
        }
    }
}

void side_agent_cleanup(void)
{
    for (int i = 0; i < g_agent_count; i++) {
        if (g_agents[i].cleanup)
            g_agents[i].cleanup();
    }
    g_agent_count = 0;
}

int side_agent_count(void)
{
    return g_agent_count;
}

/* ---- Pre-query: serial fork/pipe/select ---- */

typedef struct {
    int fd;         /* pipe read end */
    pid_t pid;
    int agent_idx;
    char *result;
    int done;
} pre_query_slot_t;

char *side_agents_pre_query(const char *query, const char *cwd)
{
    if (!query || !query[0]) return NULL;

    /* Phase 1: Create pipes for all enabled pre_query agents */
    int slots_count = 0;
    pre_query_slot_t slots[SIDE_AGENT_MAX];
    int write_fds[SIDE_AGENT_MAX];

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

    if (slots_count == 0)
        return NULL;

    /* Phase 2: Fork and wait for each agent serially */
    for (int s = 0; s < slots_count; s++) {
        pid_t pid = fork();
        if (pid < 0) {
            close(slots[s].fd);
            close(write_fds[s]);
            slots[s].done = 1;
            continue;
        }

        if (pid == 0) {
            /* Child */
            close(slots[s].fd);  /* close read end */
            close(STDIN_FILENO);

            /* Reset SIGPIPE — bash sets it to SIG_IGN which causes
             * curl and other I/O to hang on broken pipes. */
            signal(SIGPIPE, SIG_DFL);

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

        /* Parent: close write end, wait for this child */
        close(write_fds[s]);
        slots[s].pid = pid;

        /* Read with timeout */
        int t = g_agents[slots[s].agent_idx].timeout_sec;
        if (t <= 0) t = SIDE_AGENT_DEFAULT_TIMEOUT;

        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(slots[s].fd, &rfds);
        tv.tv_sec = t;
        tv.tv_usec = 0;

        int r = select(slots[s].fd + 1, &rfds, NULL, NULL, &tv);
        if (r > 0) {
            char *buf = malloc(SIDE_AGENT_BUF_SIZE);
            if (buf) {
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
        }

        slots[s].done = 1;
        close(slots[s].fd);
        kill(slots[s].pid, SIGTERM);
        waitpid(slots[s].pid, NULL, 0);
    }

    /* Phase 3: Combine results */
    size_t total_len = 0;
    for (int i = 0; i < slots_count; i++) {
        if (slots[i].result) {
            const char *name = g_agents[slots[i].agent_idx].name;
            /* ## name\nresult\n */
            total_len += strlen(name) + strlen(slots[i].result) + 8;
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
                        "## %s\n%s\n", name, slots[i].result);
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

        pid_t pid = fork();
        if (pid < 0) continue;

        if (pid > 0) {
            waitpid(pid, NULL, 0);
            continue;
        }

        /* Child: fork again to avoid zombies */
        pid_t pid2 = fork();
        if (pid2 > 0) _exit(0);
        if (pid2 < 0) _exit(1);

        /* Grandchild */
        close(STDIN_FILENO);
        signal(SIGPIPE, SIG_DFL);
        g_agents[i].post_query(query, response, cwd);
        _exit(0);
    }
}
