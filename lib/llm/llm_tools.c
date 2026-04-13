#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>

#include "llm_tools.h"
#include "llm_memory.h"
#include "llm_manscan.h"
#include "llm_streams.h"
#include "llm_config.h"
#include "cJSON.h"

static llm_builtin_t builtins[] = {
    {"ls",         llm_tool_ls,         SAFETY_AUTO,    "List directory contents"},
    {"cat",        llm_tool_cat,        SAFETY_AUTO,    "Display file contents"},
    {"read_file",  llm_tool_read_file,  SAFETY_AUTO,    "Read file with line range"},
    {"pwd",        llm_tool_pwd,        SAFETY_AUTO,    "Print working directory"},
    {"head",       llm_tool_head,       SAFETY_AUTO,    "Show first N lines"},
    {"wc",         llm_tool_wc,         SAFETY_AUTO,    "Count lines/words/chars"},
    {"grep",       llm_tool_grep,       SAFETY_AUTO,    "Search file contents"},
    {"cd",         llm_tool_cd,         SAFETY_AUTO,    "Change directory"},
    {"cp",         llm_tool_cp,         SAFETY_CONFIRM, "Copy files"},
    {"mv",         llm_tool_mv,         SAFETY_CONFIRM, "Move/rename files"},
    {"mkdir",      llm_tool_mkdir,      SAFETY_CONFIRM, "Create directory"},
    {"write_file", llm_tool_write_file, SAFETY_CONFIRM, "Write content to file"},
    {"man",        llm_tool_man,        SAFETY_AUTO,    "Get detailed man page info"},
    {"rm",         llm_tool_rm,         SAFETY_DANGER,  "Remove files"},
    /* All memory tools removed -- whisper agent handles memory retrieval,
       background agent handles saving/cleanup. User commands (llm remember,
       llm forget, llm memories) still work directly via the builtin. */
    {NULL, NULL, 0, NULL}
};

void llm_tools_init(void)
{
    /* Nothing needed for now */
}

const llm_builtin_t *llm_tools_find(const char *name)
{
    for (int i = 0; builtins[i].name; i++) {
        if (strcmp(builtins[i].name, name) == 0)
            return &builtins[i];
    }
    return NULL;
}

/* Helper: append to a dynamic buffer */
static void buf_append(char **buf, size_t *len, size_t *cap, const char *s)
{
    size_t slen = strlen(s);
    while (*len + slen + 1 > *cap) {
        *cap = (*cap == 0) ? 1024 : *cap * 2;
        *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + *len, s, slen);
    *len += slen;
    (*buf)[*len] = '\0';
}

char *llm_tool_ls(const char *args_json)
{
    cJSON *args = cJSON_Parse(args_json);
    const char *path = ".";
    if (args) {
        cJSON *p = cJSON_GetObjectItem(args, "path");
        if (p && cJSON_IsString(p)) path = p->valuestring;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        cJSON_Delete(args);
        char err[256];
        snprintf(err, sizeof(err), "ls: %s: %s", path, strerror(errno));
        return strdup(err);
    }

    char *buf = NULL;
    size_t len = 0, cap = 0;
    struct dirent *ent;

    while ((ent = readdir(dir))) {
        if (ent->d_name[0] == '.') continue;
        buf_append(&buf, &len, &cap, ent->d_name);
        buf_append(&buf, &len, &cap, "\n");
    }
    closedir(dir);
    cJSON_Delete(args);

    return buf ? buf : strdup("");
}

char *llm_tool_cat(const char *args_json)
{
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return strdup("error: invalid args");

    cJSON *p = cJSON_GetObjectItem(args, "path");
    if (!p || !cJSON_IsString(p)) {
        cJSON_Delete(args);
        return strdup("error: path required");
    }

    FILE *f = fopen(p->valuestring, "r");
    if (!f) {
        char err[256];
        snprintf(err, sizeof(err), "cat: %s: %s", p->valuestring, strerror(errno));
        cJSON_Delete(args);
        return strdup(err);
    }

    char *buf = NULL;
    size_t len = 0, cap = 0;
    char line[4096];

    while (fgets(line, sizeof(line), f))
        buf_append(&buf, &len, &cap, line);

    fclose(f);
    cJSON_Delete(args);
    return buf ? buf : strdup("");
}

char *llm_tool_read_file(const char *args_json)
{
    return llm_tool_cat(args_json);
}

char *llm_tool_pwd(const char *args_json)
{
    (void)args_json;
    char buf[4096];
    if (getcwd(buf, sizeof(buf)))
        return strdup(buf);
    return strdup("error: getcwd failed");
}

char *llm_tool_cd(const char *args_json)
{
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return strdup("error: invalid args");

    cJSON *p = cJSON_GetObjectItem(args, "path");
    const char *path = (p && cJSON_IsString(p)) ? p->valuestring : getenv("HOME");

    if (chdir(path) != 0) {
        char err[256];
        snprintf(err, sizeof(err), "cd: %s: %s", path, strerror(errno));
        cJSON_Delete(args);
        return strdup(err);
    }

    cJSON_Delete(args);
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd)))
        return strdup("(unknown)");
    return strdup(cwd);
}

char *llm_tool_cp(const char *args_json)
{
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return strdup("error: invalid args");

    cJSON *src = cJSON_GetObjectItem(args, "src");
    cJSON *dst = cJSON_GetObjectItem(args, "dst");
    if (!src || !dst || !cJSON_IsString(src) || !cJSON_IsString(dst)) {
        cJSON_Delete(args);
        return strdup("error: src and dst required");
    }

    FILE *in = fopen(src->valuestring, "rb");
    if (!in) {
        char err[256];
        snprintf(err, sizeof(err), "cp: %s: %s", src->valuestring, strerror(errno));
        cJSON_Delete(args);
        return strdup(err);
    }

    FILE *out = fopen(dst->valuestring, "wb");
    if (!out) {
        char err[256];
        snprintf(err, sizeof(err), "cp: %s: %s", dst->valuestring, strerror(errno));
        fclose(in);
        cJSON_Delete(args);
        return strdup(err);
    }

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);

    fclose(in);
    fclose(out);
    cJSON_Delete(args);

    char msg[512];
    snprintf(msg, sizeof(msg), "copied %s -> %s", src->valuestring, dst->valuestring);
    return strdup(msg);
}

char *llm_tool_mv(const char *args_json)
{
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return strdup("error: invalid args");

    cJSON *src = cJSON_GetObjectItem(args, "src");
    cJSON *dst = cJSON_GetObjectItem(args, "dst");
    if (!src || !dst || !cJSON_IsString(src) || !cJSON_IsString(dst)) {
        cJSON_Delete(args);
        return strdup("error: src and dst required");
    }

    if (rename(src->valuestring, dst->valuestring) != 0) {
        char err[256];
        snprintf(err, sizeof(err), "mv: %s", strerror(errno));
        cJSON_Delete(args);
        return strdup(err);
    }

    char msg[512];
    snprintf(msg, sizeof(msg), "moved %s -> %s", src->valuestring, dst->valuestring);
    cJSON_Delete(args);
    return strdup(msg);
}

char *llm_tool_rm(const char *args_json)
{
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return strdup("error: invalid args");

    cJSON *p = cJSON_GetObjectItem(args, "path");
    if (!p || !cJSON_IsString(p)) {
        cJSON_Delete(args);
        return strdup("error: path required");
    }

    if (remove(p->valuestring) != 0) {
        char err[256];
        snprintf(err, sizeof(err), "rm: %s: %s", p->valuestring, strerror(errno));
        cJSON_Delete(args);
        return strdup(err);
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "removed %s", p->valuestring);
    cJSON_Delete(args);
    return strdup(msg);
}

char *llm_tool_mkdir(const char *args_json)
{
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return strdup("error: invalid args");

    cJSON *p = cJSON_GetObjectItem(args, "path");
    if (!p || !cJSON_IsString(p)) {
        cJSON_Delete(args);
        return strdup("error: path required");
    }

    if (mkdir(p->valuestring, 0755) != 0 && errno != EEXIST) {
        char err[256];
        snprintf(err, sizeof(err), "mkdir: %s: %s", p->valuestring, strerror(errno));
        cJSON_Delete(args);
        return strdup(err);
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "created %s", p->valuestring);
    cJSON_Delete(args);
    return strdup(msg);
}

static char *shell_escape(const char *s)
{
    size_t n = 0;
    for (const char *p = s; *p; p++)
        if (*p == '\'') n++;

    if (n == 0) return strdup(s);

    size_t len = strlen(s) + n * 3 + 1;
    char *out = malloc(len);
    char *w = out;
    for (const char *p = s; *p; p++) {
        if (*p == '\'') {
            *w++ = '\''; *w++ = '\\'; *w++ = '\''; *w++ = '\'';
        } else {
            *w++ = *p;
        }
    }
    *w = '\0';
    return out;
}

char *llm_tool_grep(const char *args_json)
{
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return strdup("error: invalid args");

    cJSON *pattern = cJSON_GetObjectItem(args, "pattern");
    if (!pattern || !cJSON_IsString(pattern)) {
        cJSON_Delete(args);
        return strdup("error: pattern required");
    }

    cJSON *path   = cJSON_GetObjectItem(args, "path");
    cJSON *rec    = cJSON_GetObjectItem(args, "recursive");
    cJSON *icase  = cJSON_GetObjectItem(args, "ignore_case");

    char *esc_pattern = shell_escape(pattern->valuestring);
    const char *raw_path = (path && cJSON_IsString(path)) ? path->valuestring : ".";
    char *esc_path = shell_escape(raw_path);

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "grep %s %s -- '%s' '%s' 2>&1",
             (rec && cJSON_IsTrue(rec)) ? "-r" : "",
             (icase && cJSON_IsTrue(icase)) ? "-i" : "",
             esc_pattern, esc_path);

    free(esc_pattern);
    free(esc_path);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        cJSON_Delete(args);
        return strdup("error: grep failed");
    }

    char *buf = NULL;
    size_t len = 0, cap = 0;
    char line[4096];

    while (fgets(line, sizeof(line), fp))
        buf_append(&buf, &len, &cap, line);

    pclose(fp);
    cJSON_Delete(args);
    return buf ? buf : strdup("(no matches)");
}

char *llm_tool_head(const char *args_json)
{
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return strdup("error: invalid args");

    cJSON *p = cJSON_GetObjectItem(args, "path");
    if (!p || !cJSON_IsString(p)) {
        cJSON_Delete(args);
        return strdup("error: path required");
    }

    cJSON *n = cJSON_GetObjectItem(args, "lines");
    int lines = (n && cJSON_IsNumber(n)) ? n->valueint : 10;

    FILE *f = fopen(p->valuestring, "r");
    if (!f) {
        char err[256];
        snprintf(err, sizeof(err), "head: %s: %s", p->valuestring, strerror(errno));
        cJSON_Delete(args);
        return strdup(err);
    }

    char *buf = NULL;
    size_t len = 0, cap = 0;
    char line[4096];
    int count = 0;

    while (count < lines && fgets(line, sizeof(line), f)) {
        buf_append(&buf, &len, &cap, line);
        count++;
    }

    fclose(f);
    cJSON_Delete(args);
    return buf ? buf : strdup("");
}

char *llm_tool_wc(const char *args_json)
{
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return strdup("error: invalid args");

    cJSON *p = cJSON_GetObjectItem(args, "path");
    if (!p || !cJSON_IsString(p)) {
        cJSON_Delete(args);
        return strdup("error: path required");
    }

    FILE *f = fopen(p->valuestring, "r");
    if (!f) {
        char err[256];
        snprintf(err, sizeof(err), "wc: %s: %s", p->valuestring, strerror(errno));
        cJSON_Delete(args);
        return strdup(err);
    }

    int lines = 0, words = 0, chars = 0;
    int in_word = 0;
    int c;

    while ((c = fgetc(f)) != EOF) {
        chars++;
        if (c == '\n') lines++;
        if (c == ' ' || c == '\n' || c == '\t') {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            words++;
        }
    }

    fclose(f);
    cJSON_Delete(args);

    char msg[256];
    snprintf(msg, sizeof(msg), "%d %d %d %s", lines, words, chars, p->valuestring);
    return strdup(msg);
}

char *llm_tool_write_file(const char *args_json)
{
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return strdup("error: invalid args");

    cJSON *p = cJSON_GetObjectItem(args, "path");
    cJSON *content = cJSON_GetObjectItem(args, "content");
    if (!p || !content || !cJSON_IsString(p) || !cJSON_IsString(content)) {
        cJSON_Delete(args);
        return strdup("error: path and content required");
    }

    FILE *f = fopen(p->valuestring, "w");
    if (!f) {
        char err[256];
        snprintf(err, sizeof(err), "write_file: %s: %s", p->valuestring, strerror(errno));
        cJSON_Delete(args);
        return strdup(err);
    }

    fputs(content->valuestring, f);
    fclose(f);
    cJSON_Delete(args);

    char msg[256];
    snprintf(msg, sizeof(msg), "wrote %s", p->valuestring);
    return strdup(msg);
}

char *llm_tool_man(const char *args_json)
{
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return strdup("error: invalid args");

    cJSON *cmd = cJSON_GetObjectItem(args, "command");
    if (!cmd || !cJSON_IsString(cmd)) {
        cJSON_Delete(args);
        return strdup("error: command required");
    }

    cJSON *max = cJSON_GetObjectItem(args, "max_bytes");
    int max_bytes = (max && cJSON_IsNumber(max)) ? max->valueint : 4096;

    char *detail = llm_manscan_detail(cmd->valuestring, max_bytes);
    cJSON_Delete(args);

    if (!detail)
        return strdup("(no man page found)");

    stream_man_output(detail);
    return detail;
}
