#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "config.h"
#include "llm.h"
#include "builtin.h"
#include "history.h"
#include "serverconf.h"
#include "pathscan.h"
#include "manscan.h"
#include "streams.h"
#include "router.h"
#include "shell.h"

/* Signal flag — must be global for signal handler. Extern'd by llm.c and shell.c */
volatile sig_atomic_t interrupted = 0;

/* Shell context pointer for readline callback (can't pass userdata) */
static shell_ctx_t *g_ctx = NULL;

static void sigint_handler(int sig)
{
    (void)sig;
    interrupted = 1;
}

/* SSE streaming callbacks */
static void chat_token_cb(const char *token, void *userdata)
{
    (void)userdata;
    stream_chat_output(token);
}

static void think_token_cb(const char *token, void *userdata)
{
    (void)userdata;
    stream_think_output(token);
}

static llm_stream_cbs g_stream_cbs = {
    .on_token = chat_token_cb,
    .on_thinking = think_token_cb,
    .userdata = NULL
};

/* Shift+Tab handler: cycle to next server */
static int shift_tab_handler(int count, int key)
{
    (void)count;
    (void)key;
    if (!g_ctx) return 0;

    int next = (g_ctx->servers->active + 1) % g_ctx->servers->count;
    g_ctx->servers->active = next;

    const server_entry_t *s = serverconf_active(g_ctx->servers);
    llm_cleanup();
    if (llm_init(s->api_url, s->model, s->api_key) != 0) {
        fprintf(stderr, "\nllmsh: failed to connect to %s\n", s->name);
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "\n→ %s (%s)\n", s->name, s->model);
    stream_chat_output(msg);

    history_cleanup();
    history_init();
    free(g_ctx->last_output);
    g_ctx->last_output = NULL;

    rl_on_new_line();
    rl_replace_line("", 0);
    rl_redisplay();
    return 0;
}

/* Read piped stdin into a buffer. Returns NULL if stdin is a tty. */
static char *read_piped_stdin(void)
{
    if (isatty(STDIN_FILENO))
        return NULL;

    char *buf = NULL;
    size_t len = 0, cap = 0;
    char tmp[4096];
    ssize_t n;

    while ((n = read(STDIN_FILENO, tmp, sizeof(tmp))) > 0) {
        if (len + n + 1 > cap) {
            cap = (cap == 0) ? 8192 : cap * 2;
            buf = realloc(buf, cap);
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    if (buf) buf[len] = '\0';

    FILE *tty = fopen("/dev/tty", "r");
    if (tty) {
        dup2(fileno(tty), STDIN_FILENO);
        fclose(tty);
    }

    return buf;
}

static char *join_args(int argc, char **argv, int start)
{
    size_t len = 0;
    for (int i = start; i < argc; i++)
        len += strlen(argv[i]) + 1;
    char *s = malloc(len);
    s[0] = '\0';
    for (int i = start; i < argc; i++) {
        if (i > start) strcat(s, " ");
        strcat(s, argv[i]);
    }
    return s;
}

int main(int argc, char **argv)
{
    /* Parse flags */
    int opt;
    while ((opt = getopt(argc, argv, "vqldhH")) != -1) {
        switch (opt) {
        case 'v': streams_verbose = 1; break;
        case 'q': streams_verbose = 0; break;
        case 'l': streams_label_mode = 1; break;
        case 'd': streams_label_mode = 2; break;
        case 'h': case 'H': {
            shell_ctx_t help_ctx = {0};
            streams_init();
            shell_handle_slash(&help_ctx, "help");
            streams_cleanup();
            return 0;
        }
        }
    }

    /* Load config */
    server_config_t *servers = serverconf_load();
    const server_entry_t *active = serverconf_active(servers);

    /* Read piped stdin */
    char *piped_input = read_piped_stdin();

    /* Initialize subsystems */
    signal(SIGINT, sigint_handler);
    streams_init();
    history_init();

    char histfile[4096];
    snprintf(histfile, sizeof(histfile), "%s/.llmsh_history",
             getenv("HOME") ? getenv("HOME") : ".");
    read_history(histfile);
    rl_bind_keyseq("\\e[Z", shift_tab_handler);

    if (llm_init(active->api_url, active->model, active->api_key) != 0) {
        fprintf(stderr, "llmsh: failed to initialize LLM client\n");
        return 1;
    }

    int ncmds = pathscan_init();
    int nman = manscan_init();
    builtin_init();
    router_init(servers);

    /* Create shell context */
    shell_ctx_t ctx = {
        .servers = servers,
        .cbs = &g_stream_cbs,
        .last_output = NULL,
        .max_iterations = servers->max_iterations,
    };
    g_ctx = &ctx;  /* for readline callback */

    {
        char banner[512];
        snprintf(banner, sizeof(banner),
                 "llmsh - natural language shell\n"
                 "Server: %s (%s) | %d commands in PATH | %d man pages indexed\n"
                 "Shift+Tab to cycle servers. 'help' for usage. 'exit' to quit.\n\n",
                 active->name, active->model, ncmds, nman);
        stream_chat_output(banner);
    }

    /* ── One-shot mode ──────────────────────────────────────────── */
    if (optind < argc) {
        char *query = join_args(argc, argv, optind);
        char *result = shell_query(&ctx, query, piped_input);
        free(query);
        free(piped_input);
        free(result);
        goto cleanup;
    }

    /* ── Interactive REPL ───────────────────────────────────────── */
    for (;;) {
        interrupted = 0;
        active = serverconf_active(ctx.servers);

        char cwd[4096];
        if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, ".");

        char prompt[4200];
        snprintf(prompt, sizeof(prompt), "%s@%s> ",
                 active->model ? active->model : "llmsh", cwd);

        char *line = readline(prompt);
        if (!line) { stream_chat_output("\n"); break; }
        if (line[0] == '\0') { free(line); continue; }
        if (shell_is_exit(line)) { free(line); break; }

        add_history(line);

        if (shell_handle_slash(&ctx, line)) { free(line); continue; }

        int first_word_is_cmd = 0;
        char *matched_cmds = pathscan_match_input(line, &first_word_is_cmd);

        if (first_word_is_cmd) {
            free(matched_cmds);
            char *result = shell_execute(&ctx, line);
            free(result);
            free(line);
            continue;
        }

        free(matched_cmds);

        /* Check for output redirection: "review code > file.txt" */
        char *outfile = NULL;
        int append_mode = 0;
        char *stripped = shell_parse_redirect(line, &outfile, &append_mode);
        const char *query = stripped ? stripped : line;

        char *result = shell_query(&ctx, query, NULL);

        /* Write LLM result to file if redirected */
        if (outfile && result) {
            FILE *fp = fopen(outfile, append_mode ? "a" : "w");
            if (fp) {
                fputs(result, fp);
                fclose(fp);
                char msg[512];
                snprintf(msg, sizeof(msg), "Wrote to %s\n", outfile);
                stream_chat_output(msg);
            } else {
                fprintf(stderr, "llmsh: cannot write to %s\n", outfile);
            }
        }

        free(outfile);
        free(stripped);
        free(result);
        free(line);
    }

cleanup:
    free(ctx.last_output);
    free(piped_input);
    write_history(histfile);
    history_cleanup();
    llm_cleanup();
    pathscan_cleanup();
    manscan_cleanup();
    streams_cleanup();
    serverconf_free(servers);
    return 0;
}
