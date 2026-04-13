#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm_streams.h"

int streams_verbose = 1;
int streams_label_mode = 0;
int streams_llm_active = 0;

/* Track whether we're at the start of a line for each stream (for prefixing) */
static int chat_sol = 1;
static int tool_sol = 1;
static int think_sol = 1;
static int man_sol = 1;
static int mem_sol = 1;

void llm_streams_init(void)
{
    /* Nothing to do -- stdout/stderr are already set up */
}

void llm_streams_cleanup(void)
{
    /* Nothing to do */
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
    if (text)
        labeled_write(stdout, "chat", "\033[32m", text, &chat_sol);
}

void stream_think_output(const char *text)
{
    if (!text || !streams_label_mode) return;
    labeled_write(stderr, "think", "\033[35m", text, &think_sol);
}

void stream_man_output(const char *text)
{
    if (!text || !streams_label_mode) return;
    labeled_write(stderr, "man", "\033[95m", text, &man_sol);
}

void stream_tool_call(const char *name, const char *args)
{
    if (!streams_label_mode) return;
    fprintf(stderr, "\033[33m[tool]\033[0m %s(%s)\n", name, args ? args : "");
    fflush(stderr);
}

void stream_api_output(const char *direction, const char *text)
{
    if (streams_label_mode < 2 || !text) return;
    fprintf(stderr, "\033[90m[api%s]\033[0m %s\n", direction, text);
    fflush(stderr);
}

void stream_memory_output(const char *text)
{
    if (!text || !streams_label_mode) return;
    labeled_write(stderr, "mem", "\033[95m", text, &mem_sol);
}

static int global_mem_sol = 1;

void stream_global_mem_agent_output(const char *text)
{
    if (!text || !streams_label_mode) return;
    labeled_write(stderr, "global-mem", "\033[93m", text, &global_mem_sol);
}

static int cron_sol = 1;

void stream_cron_agent_output(const char *text)
{
    if (!text || !streams_label_mode) return;
    labeled_write(stderr, "cron", "\033[94m", text, &cron_sol);
}

static int side_sol = 1;

void stream_side_agent_output(const char *name, const char *text)
{
    if (!text || !streams_label_mode) return;
    char label[64];
    snprintf(label, sizeof(label), "agent:%s", name);
    labeled_write(stderr, label, "\033[96m", text, &side_sol);
}
