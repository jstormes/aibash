#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

#include "shell.h"
#include "llm.h"
#include "router.h"
#include "history.h"
#include "pathscan.h"
#include "streams.h"
#include "serverconf.h"
#include "exec.h"
#include "cJSON.h"

/* interrupted stays global — signal handlers can't take context */
extern volatile sig_atomic_t interrupted;

/* ── Helpers ─────────────────────────────────────────────────────── */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t')) *end-- = '\0';
    return s;
}

static void first_word(const char *s, char *out, size_t out_sz)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t i = 0;
    while (*s && *s != ' ' && *s != '\t' && i < out_sz - 1)
        out[i++] = *s++;
    out[i] = '\0';
}

/* Helper to update ctx->last_output */
static void set_last_output(shell_ctx_t *ctx, char *output)
{
    free(ctx->last_output);
    ctx->last_output = output;
}

/* ── Agentic loop ────────────────────────────────────────────────── */

char *shell_agentic_loop(shell_ctx_t *ctx, llm_response_t *resp)
{
    char cwd[4096];
    int iterations = 0;
    char *final_text = NULL;

    while (resp && resp->num_tool_calls > 0
           && !interrupted && iterations < ctx->max_iterations) {
        iterations++;

        /* Stream any assistant text before tool calls */
        if (resp->text && resp->text[0])
            stream_chat_output("\n");

        /* Record assistant message with tool_calls (OpenAI protocol) */
        history_add_assistant_tool_calls(resp->text,
                                          resp->tool_calls,
                                          resp->num_tool_calls);

        set_last_output(ctx, NULL);

        /* Execute each tool call and record results */
        for (int i = 0; i < resp->num_tool_calls && !interrupted; i++) {
            /* Show brief status on stdchat so user knows what's happening */
            const char *name = resp->tool_calls[i].name;
            if (name) {
                char status[512];
                /* Parse a short summary from arguments */
                const char *args = resp->tool_calls[i].arguments;
                if (args && args[0]) {
                    /* Extract first string value for a hint */
                    cJSON *a = cJSON_Parse(args);
                    const char *hint = NULL;
                    if (a) {
                        cJSON *child = a->child;
                        while (child) {
                            if (cJSON_IsString(child)) { hint = child->valuestring; break; }
                            if (cJSON_IsArray(child)) {
                                cJSON *first = cJSON_GetArrayItem(child, 0);
                                if (first && cJSON_IsString(first)) { hint = first->valuestring; break; }
                            }
                            child = child->next;
                        }
                        snprintf(status, sizeof(status), "  → %s %s\n",
                                 name, hint ? hint : "");
                        cJSON_Delete(a);
                    } else {
                        snprintf(status, sizeof(status), "  → %s\n", name);
                    }
                } else {
                    snprintf(status, sizeof(status), "  → %s\n", name);
                }
                fprintf(stderr, "\033[90m%s\033[0m", status);
            }

            char *result = router_dispatch(&resp->tool_calls[i]);
            if (result) {
                stream_tool_output(result);
                if (result[0] && result[strlen(result)-1] != '\n')
                    stream_tool_output("\n");
                history_add_tool_result(resp->tool_calls[i].id,
                                        resp->tool_calls[i].name,
                                        result);
                set_last_output(ctx, result);
            }
        }

        llm_response_free(resp);
        resp = NULL;

        if (interrupted) break;

        if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, ".");
        resp = llm_chat_stream(NULL, cwd, ctx->last_output, NULL, 0, ctx->cbs);
    }

    if (resp && resp->text && resp->text[0]) {
        stream_chat_output("\n");
        history_add_assistant(resp->text);
        final_text = strdup(resp->text);
    }

    if (iterations >= ctx->max_iterations)
        fprintf(stderr, "llmsh: max iterations reached (%d)\n", ctx->max_iterations);

    llm_response_free(resp);
    return final_text;
}

/* ── Query ───────────────────────────────────────────────────────── */

char *shell_query(shell_ctx_t *ctx, const char *query, const char *context)
{
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, ".");

    char *full_query;
    if (context) {
        size_t len = strlen(query) + strlen(context) + 64;
        full_query = malloc(len);
        snprintf(full_query, len,
                 "[context]:\n%s\n\n[user request]: %s",
                 context, query);
    } else {
        full_query = strdup(query);
    }

    int fw = 0;
    char *mc = pathscan_match_input(full_query, &fw);

    history_add_user(full_query);
    streams_llm_active = 1;
    llm_response_t *resp = llm_chat_stream(full_query, cwd, ctx->last_output,
                                            mc, fw, ctx->cbs);
    free(full_query);
    free(mc);

    if (!resp) {
        streams_llm_active = 0;
        fprintf(stderr, "llmsh: LLM request failed\n");
        return NULL;
    }

    char *result = shell_agentic_loop(ctx, resp);
    streams_llm_active = 0;
    return result;
}

/* ── Split and execute ───────────────────────────────────────────── */

static int split_pipeline(const char *cmdline,
                           char **llm_prompt, char **pipe_context)
{
    *llm_prompt = NULL;
    *pipe_context = NULL;

    char *copy = strdup(cmdline);
    char *segments[64];
    int nseg = 0;

    char *saveptr = NULL;
    char *seg = strtok_r(copy, "|", &saveptr);
    while (seg && nseg < 64) {
        segments[nseg++] = trim(seg);
        seg = strtok_r(NULL, "|", &saveptr);
    }

    int handoff = nseg;
    for (int i = 0; i < nseg; i++) {
        char word[256];
        first_word(segments[i], word, sizeof(word));
        if (!pathscan_lookup(word)) {
            handoff = i;
            break;
        }
    }

    char *cmd_output = NULL;
    if (handoff > 0) {
        const char *cmds[64];
        for (int i = 0; i < handoff; i++)
            cmds[i] = segments[i];
        cmd_output = exec_pipeline(cmds, handoff, NULL, NULL, 0);
    }

    if (handoff < nseg) {
        size_t plen = 0;
        for (int i = handoff; i < nseg; i++)
            plen += strlen(segments[i]) + 3;
        char *prompt = malloc(plen + 1);
        prompt[0] = '\0';
        for (int i = handoff; i < nseg; i++) {
            if (i > handoff) strcat(prompt, " | ");
            strcat(prompt, segments[i]);
        }
        *llm_prompt = prompt;
        *pipe_context = cmd_output;
        free(copy);
        return 1;
    }

    if (cmd_output) {
        stream_tool_output(cmd_output);
        if (cmd_output[0] && cmd_output[strlen(cmd_output)-1] != '\n')
            stream_tool_output("\n");
    }
    *pipe_context = cmd_output;
    free(copy);
    return 0;
}

char *shell_execute(shell_ctx_t *ctx, const char *cmdline)
{
    if (strncmp(cmdline, "cd", 2) == 0
        && (cmdline[2] == '\0' || cmdline[2] == ' ' || cmdline[2] == '\t')) {
        const char *path = cmdline + 2;
        while (*path == ' ' || *path == '\t') path++;
        if (*path == '\0') path = getenv("HOME");
        if (path && chdir(path) != 0)
            fprintf(stderr, "cd: %s: %s\n", path, strerror(errno));
        return NULL;
    }

    char *llm_prompt = NULL;
    char *pipe_context = NULL;
    int needs_llm = split_pipeline(cmdline, &llm_prompt, &pipe_context);

    if (needs_llm && llm_prompt) {
        char *result = shell_query(ctx, llm_prompt, pipe_context);
        free(llm_prompt);
        free(pipe_context);
        return result;
    }

    set_last_output(ctx, pipe_context);
    free(llm_prompt);
    return NULL;
}

/* ── Slash commands ──────────────────────────────────────────────── */

int shell_handle_slash(shell_ctx_t *ctx, const char *line)
{
    if (strcmp(line, "help") == 0 || strcmp(line, "/help") == 0) {
        stream_chat_output(
            "llmsh - natural language shell\n"
            "\n"
            "Type plain English OR standard shell commands. llmsh understands both.\n"
            "\n"
            "Natural language examples:\n"
            "  show me what's in this directory\n"
            "  find all .c files larger than 10k\n"
            "  count the lines of code in src/\n"
            "\n"
            "Direct shell commands also work:\n"
            "  ls -la\n"
            "  gcc -o foo foo.c\n"
            "  grep -r TODO src/ | wc -l\n"
            "\n"
            "Hybrid pipes: ls | show me the markdown files\n"
            "\n"
            "Output streams:\n"
            "  fd 1 (stdout)   Tool output (visible by default)\n"
            "  fd 2 (stderr)   Errors and confirmations\n"
            "  fd 3 (stdchat)  LLM responses (visible by default)\n"
            "\n"
            "Flags: -v verbose  -q quiet  -l labels  -d debug  -h help\n"
            "Keys:  Shift+Tab cycle servers  Up/Down history\n"
            "\n"
            "Commands:\n"
            "  /server [name]   List or switch servers\n"
            "  /clear           Clear conversation history\n"
            "  /verbose         Toggle tool output\n"
            "  /labels          Toggle stream labels\n"
            "  /debug           Toggle debug mode\n"
            "  exit, /exit      Exit the shell\n"
        );
        return 1;
    }

    if (strcmp(line, "/clear") == 0) {
        history_cleanup();
        history_init();
        set_last_output(ctx, NULL);
        stream_chat_output("Conversation history cleared.\n");
        return 1;
    }

    if (strcmp(line, "/verbose") == 0) {
        streams_verbose = !streams_verbose;
        stream_chat_output(streams_verbose
            ? "Tool output: visible\n"
            : "Tool output: hidden\n");
        return 1;
    }

    if (strcmp(line, "/labels") == 0) {
        streams_label_mode = streams_label_mode ? 0 : 1;
        stream_chat_output(streams_label_mode
            ? "Labels: on\n" : "Labels: off\n");
        return 1;
    }

    if (strcmp(line, "/debug") == 0) {
        streams_label_mode = (streams_label_mode == 2) ? 0 : 2;
        stream_chat_output(streams_label_mode == 2
            ? "Debug mode: on (labels + API)\n"
            : "Debug mode: off\n");
        return 1;
    }

    if (strncmp(line, "/server", 7) == 0) {
        const char *arg = line + 7;
        while (*arg == ' ') arg++;

        if (*arg == '\0') {
            serverconf_list(ctx->servers);
            return 1;
        }

        if (serverconf_switch(ctx->servers, arg) != 0) {
            fprintf(stderr, "llmsh: unknown server '%s'\n", arg);
            return 1;
        }

        const server_entry_t *s = serverconf_active(ctx->servers);
        llm_cleanup();
        if (llm_init(s->api_url, s->model, s->api_key) != 0) {
            fprintf(stderr, "llmsh: failed to connect to %s\n", s->name);
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg), "Switched to %s (%s)\n", s->name, s->model);
            stream_chat_output(msg);
        }

        history_cleanup();
        history_init();
        set_last_output(ctx, NULL);
        return 1;
    }

    return 0;
}

int shell_is_exit(const char *line)
{
    return strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0
        || strcmp(line, "/exit") == 0 || strcmp(line, "/quit") == 0;
}

char *shell_parse_redirect(const char *input, char **outfile, int *append)
{
    *outfile = NULL;
    *append = 0;

    /* Scan from end for > or >> not inside quotes */
    const char *redir = NULL;
    int in_sq = 0, in_dq = 0;
    for (const char *p = input; *p; p++) {
        if (*p == '\'' && !in_dq) in_sq = !in_sq;
        else if (*p == '"' && !in_sq) in_dq = !in_dq;
        else if (!in_sq && !in_dq && *p == '>') redir = p;
    }

    if (!redir) return NULL;

    /* Check for >> (append) */
    const char *start = redir;
    if (start > input && *(start - 1) == '>') {
        /* It's actually >> at start-1 */
        /* But wait, we found the LAST >, so >> means start is second > */
        /* Re-check: is there a > before this one? */
    }
    /* Simpler: check if >> */
    if (redir > input && *(redir - 1) == '>') {
        *append = 1;
        start = redir - 1;
    }

    /* Extract filename after > or >> */
    const char *fname = redir + 1;
    while (*fname == ' ' || *fname == '\t') fname++;
    if (*fname == '\0') return NULL; /* no filename */

    /* Trim trailing whitespace from filename */
    const char *fend = fname + strlen(fname) - 1;
    while (fend > fname && (*fend == ' ' || *fend == '\t')) fend--;

    size_t flen = fend - fname + 1;
    *outfile = malloc(flen + 1);
    memcpy(*outfile, fname, flen);
    (*outfile)[flen] = '\0';

    /* Return input with redirect stripped and trimmed */
    size_t qlen = start - input;
    while (qlen > 0 && (input[qlen-1] == ' ' || input[qlen-1] == '\t'))
        qlen--;

    char *query = malloc(qlen + 1);
    memcpy(query, input, qlen);
    query[qlen] = '\0';

    return query;
}
