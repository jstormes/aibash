#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "router.h"
#include "builtin.h"
#include "exec.h"
#include "safety.h"
#include "streams.h"
#include "manscan.h"
#include "config.h"
#include "cJSON.h"

static const server_config_t *g_conf = NULL;

void router_init(const server_config_t *conf)
{
    g_conf = conf;
}

/* Commands that are read-only / safe to run without confirmation */
static const char *safe_commands[] = {
    "ls", "dir", "cat", "head", "tail", "less", "more",
    "wc", "grep", "egrep", "fgrep", "rg",
    "find", "locate", "which", "whereis", "whatis", "type",
    "file", "stat", "du", "df", "free", "uptime",
    "date", "cal", "whoami", "id", "groups", "hostname",
    "uname", "arch", "lsb_release", "lscpu", "lsblk", "lspci", "lsusb",
    "pwd", "echo", "printf", "true", "false",
    "env", "printenv", "set",
    "ps", "top", "htop", "pgrep",
    "ip", "ifconfig", "netstat", "ss", "ping", "host", "dig", "nslookup",
    "git", "svn",   /* read operations are safe; destructive ones have subcommands */
    "man", "info", "help", "apropos",
    "diff", "cmp", "comm", "sort", "uniq", "tr", "cut", "paste",
    "awk", "sed",   /* typically used for reading/transforming */
    "tee", "xargs",
    "md5sum", "sha256sum", "sha1sum",
    "xxd", "od", "hexdump",
    "tree", "realpath", "basename", "dirname",
    "ldd", "nm", "objdump", "readelf", "strings",
    "dpkg", "apt", "rpm",  /* query operations */
    "pip", "npm", "cargo", "go", "rustc", "python", "python3", "node",
    "make", "cmake", "gcc", "g++", "clang", "clang++", "cc",
    "java", "javac", "mvn", "gradle",
    "docker", "kubectl",
    "curl", "wget",
    "tar", "gzip", "gunzip", "zcat", "bzip2", "xz", "zip", "unzip",
    NULL
};

/*
 * Extract the first word (command name) from a command string.
 * Returns a pointer into a static buffer.
 */
static const char *first_cmd_word(const char *cmd)
{
    static char buf[256];
    int i = 0;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    while (*cmd && *cmd != ' ' && *cmd != '\t' && i < 255)
        buf[i++] = *cmd++;
    buf[i] = '\0';
    return buf;
}

/*
 * Check if a pipeline is entirely safe (all commands are read-only).
 * Also checks: no output redirection (writing to files).
 */
static int pipeline_is_safe(const char **cmds, int n,
                             const cJSON *stdout_f)
{
    /* Writing to a file is not safe */
    if (stdout_f && cJSON_IsString(stdout_f))
        return 0;

    for (int i = 0; i < n; i++) {
        const char *name = first_cmd_word(cmds[i]);
        int found = 0;
        for (int j = 0; safe_commands[j]; j++) {
            if (strcmp(name, safe_commands[j]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    return 1;
}

/* Handle the special "run" tool for arbitrary command pipelines */
static char *handle_run(const char *args_json)
{
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return strdup("error: invalid arguments");

    cJSON *pipeline = cJSON_GetObjectItem(args, "pipeline");
    if (!pipeline || !cJSON_IsArray(pipeline)) {
        cJSON_Delete(args);
        return strdup("error: pipeline must be an array of commands");
    }

    int n = cJSON_GetArraySize(pipeline);
    const char **cmds = calloc(n, sizeof(char *));
    for (int i = 0; i < n; i++) {
        cJSON *cmd = cJSON_GetArrayItem(pipeline, i);
        cmds[i] = cmd ? cmd->valuestring : "";
    }

    /* Build description for confirmation */
    char desc[2048] = "Execute: ";
    for (int i = 0; i < n; i++) {
        if (i > 0) strcat(desc, " | ");
        strncat(desc, cmds[i], sizeof(desc) - strlen(desc) - 4);
    }

    cJSON *stdin_f  = cJSON_GetObjectItem(args, "stdin_file");
    cJSON *stdout_f = cJSON_GetObjectItem(args, "stdout_file");
    cJSON *append_j = cJSON_GetObjectItem(args, "append");

    if (stdin_f && cJSON_IsString(stdin_f)) {
        strncat(desc, " < ", sizeof(desc) - strlen(desc) - 1);
        strncat(desc, stdin_f->valuestring, sizeof(desc) - strlen(desc) - 1);
    }
    if (stdout_f && cJSON_IsString(stdout_f)) {
        int app = append_j && cJSON_IsTrue(append_j);
        strncat(desc, app ? " >> " : " > ", sizeof(desc) - strlen(desc) - 1);
        strncat(desc, stdout_f->valuestring, sizeof(desc) - strlen(desc) - 1);
    }

    /* Skip confirmation for read-only commands */
    if (!pipeline_is_safe(cmds, n, stdout_f)) {
        if (!safety_confirm(desc)) {
            free(cmds);
            cJSON_Delete(args);
            return strdup("[denied by user]");
        }
    }

    const char *sin  = (stdin_f && cJSON_IsString(stdin_f)) ? stdin_f->valuestring : NULL;
    const char *sout = (stdout_f && cJSON_IsString(stdout_f)) ? stdout_f->valuestring : NULL;
    int append = (append_j && cJSON_IsTrue(append_j));

    char *result = exec_pipeline(cmds, n, sin, sout, append);

    /* Tier 1: inject whatis summaries for pipeline commands */
    if (g_conf && g_conf->man_enrich >= 1) {
        char *man_ctx = manscan_enrich_pipeline(cmds, n);
        if (man_ctx) {
            stream_man_output(man_ctx);

            size_t mc_len = strlen(man_ctx);
            size_t r_len = result ? strlen(result) : 0;
            char *enriched = malloc(mc_len + r_len + 32);
            snprintf(enriched, mc_len + r_len + 32,
                     "[man context]\n%s[output]\n%s",
                     man_ctx, result ? result : "");
            free(result);
            free(man_ctx);
            result = enriched;
        }
    }

    free(cmds);
    cJSON_Delete(args);
    return result ? result : strdup("(no output)");
}

char *router_dispatch(const tool_call_t *tc)
{
    if (!tc || !tc->name)
        return strdup("error: invalid tool call");

    /* Label the tool call */
    stream_tool_call(tc->name, tc->arguments);

    /* Special case: run */
    if (strcmp(tc->name, "run") == 0)
        return handle_run(tc->arguments ? tc->arguments : "{}");

    /* Look up builtin */
    const builtin_t *bi = builtin_find(tc->name);
    if (!bi)
        return strdup("error: unknown tool");

    /* Check safety tier */
    if (bi->safety_tier >= SAFETY_CONFIRM) {
        char desc[512];
        snprintf(desc, sizeof(desc), "%s: %s",
                 bi->name, tc->arguments ? tc->arguments : "{}");
        if (!safety_confirm(desc))
            return strdup("[denied by user]");
    }

    return bi->handler(tc->arguments ? tc->arguments : "{}");
}
