#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "streams.h"

FILE *stdchat = NULL;
int streams_verbose = 1;
int streams_label_mode = 0;
int streams_llm_active = 0;

/* Track whether we're at the start of a line for each stream (for prefixing) */
static int chat_sol = 1;   /* start-of-line for chat */
static int tool_sol = 1;   /* start-of-line for tool output */
static int think_sol = 1;  /* start-of-line for thinking */
static int man_sol = 1;    /* start-of-line for man lookups */

void streams_init(void)
{
    /*
     * fd 3 is our chat output channel. Three cases:
     * 1. User explicitly redirected: "llmsh 3>file" — fd 3 open, not a tty → use it
     * 2. Leaked fd or inherited: fd 3 open, IS a tty → close it, reopen /dev/tty
     * 3. Not open: open /dev/tty on fd 3
     *
     * Cases 2 and 3 both result in fd 3 → terminal.
     */
    int fd3_open = (fcntl(LLMSH_FD_CHAT, F_GETFD) >= 0);
    int fd3_is_redirected = 0;

    if (fd3_open) {
        /* Check if fd 3 points to something other than a terminal */
        struct stat st3, st_err;
        if (fstat(LLMSH_FD_CHAT, &st3) == 0 && fstat(STDERR_FILENO, &st_err) == 0) {
            /* If fd 3 is a different device than stderr, it was redirected */
            fd3_is_redirected = (st3.st_dev != st_err.st_dev || st3.st_ino != st_err.st_ino)
                                && !isatty(LLMSH_FD_CHAT);
        }
    }

    if (fd3_is_redirected) {
        /* User redirected fd 3 to a file — use it as-is */
        stdchat = fdopen(LLMSH_FD_CHAT, "w");
    } else {
        /* Point fd 3 at the terminal */
        if (fd3_open) close(LLMSH_FD_CHAT);
        int tty_fd = open("/dev/tty", O_WRONLY);
        if (tty_fd >= 0) {
            if (tty_fd != LLMSH_FD_CHAT) {
                dup2(tty_fd, LLMSH_FD_CHAT);
                close(tty_fd);
            }
            stdchat = fdopen(LLMSH_FD_CHAT, "w");
        }
    }

    if (!stdchat)
        stdchat = stderr;
}

void streams_cleanup(void)
{
    if (stdchat && stdchat != stderr && stdchat != stdout) {
        fclose(stdchat);
        stdchat = NULL;
    }
}

/*
 * Write text with optional line-by-line label prefixing.
 * Tracks start-of-line state per stream so labels appear at each new line.
 */
static void labeled_write(FILE *fp, const char *label, const char *color,
                           const char *text, int *sol)
{
    if (!text || !fp) return;

    if (!streams_label_mode) {
        fputs(text, fp);
        fflush(fp);
        return;
    }

    for (const char *p = text; *p; p++) {
        if (*sol) {
            fprintf(fp, "%s[%s]%s ", color, label, "\033[0m");
            *sol = 0;
        }
        fputc(*p, fp);
        if (*p == '\n')
            *sol = 1;
    }
    fflush(fp);
}

void stream_tool_output(const char *text)
{
    if (!text) return;
    /* During LLM agentic loops, suppress unless debug mode */
    if (streams_llm_active && streams_label_mode < 2) return;
    if (streams_verbose)
        labeled_write(stdout, "stdout", "\033[36m", text, &tool_sol);
}

void stream_chat_output(const char *text)
{
    if (text && stdchat)
        labeled_write(stdchat, "chat", "\033[32m", text, &chat_sol);
}

void stream_think_output(const char *text)
{
    if (!text || !stdchat || !streams_label_mode) return;
    labeled_write(stdchat, "think", "\033[35m", text, &think_sol);
}

void stream_man_output(const char *text)
{
    if (!text || !stdchat || !streams_label_mode) return;
    labeled_write(stdchat, "man", "\033[95m", text, &man_sol);
}

void stream_tool_call(const char *name, const char *args)
{
    if (!streams_label_mode || !stdchat) return;
    fprintf(stdchat, "\033[33m[tool]\033[0m %s(%s)\n", name, args ? args : "");
    fflush(stdchat);
}

void stream_api_output(const char *direction, const char *text)
{
    if (streams_label_mode < 2 || !stdchat || !text) return;
    fprintf(stdchat, "\033[90m[api%s]\033[0m %s\n", direction, text);
    fflush(stdchat);
}
