#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "manscan.h"
#include "streams.h"

#define MAN_HASH_SIZE 4096

typedef struct man_node {
    char *name;
    char *summary;
    struct man_node *next;
} man_node_t;

static man_node_t *man_table[MAN_HASH_SIZE];
static int man_count = 0;

static unsigned int hash_str(const char *s)
{
    unsigned int h = 5381;
    while (*s)
        h = ((h << 5) + h) + (unsigned char)*s++;
    return h % MAN_HASH_SIZE;
}

static int table_insert(const char *name, const char *summary)
{
    unsigned int h = hash_str(name);
    for (man_node_t *n = man_table[h]; n; n = n->next) {
        if (strcmp(n->name, name) == 0)
            return 0; /* already exists */
    }
    man_node_t *node = malloc(sizeof(*node));
    node->name = strdup(name);
    node->summary = strdup(summary);
    node->next = man_table[h];
    man_table[h] = node;
    return 1;
}

int manscan_init(void)
{
    memset(man_table, 0, sizeof(man_table));
    man_count = 0;

    FILE *fp = popen("whatis -s 1,8 -w '*' 2>/dev/null", "r");
    if (!fp) return 0;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        /* Format: "name (section) - summary" */
        /* Or: "name1, name2 (section) - summary" */

        /* Find " (" to locate end of name(s) */
        char *paren = strstr(line, " (");
        if (!paren) continue;

        /* Find " - " to locate start of summary */
        char *dash = strstr(line, " - ");
        if (!dash) continue;

        /* Extract summary (trim trailing newline) */
        char *summary = dash + 3;
        char *nl = strchr(summary, '\n');
        if (nl) *nl = '\0';

        /* Extract name(s) — everything before " (" */
        *paren = '\0';
        char *names = line;

        /* Handle multi-name entries: "gawk, awk (1) - ..." */
        char *saveptr = NULL;
        char *name = strtok_r(names, ",", &saveptr);
        while (name) {
            /* Trim whitespace */
            while (*name == ' ' || *name == '\t') name++;
            char *end = name + strlen(name) - 1;
            while (end > name && (*end == ' ' || *end == '\t'))
                *end-- = '\0';

            if (*name && table_insert(name, summary))
                man_count++;

            name = strtok_r(NULL, ",", &saveptr);
        }
    }

    pclose(fp);
    return man_count;
}

void manscan_cleanup(void)
{
    for (int i = 0; i < MAN_HASH_SIZE; i++) {
        man_node_t *n = man_table[i];
        while (n) {
            man_node_t *next = n->next;
            free(n->name);
            free(n->summary);
            free(n);
            n = next;
        }
        man_table[i] = NULL;
    }
    man_count = 0;
}

char *manscan_whatis(const char *cmd)
{
    unsigned int h = hash_str(cmd);
    for (man_node_t *n = man_table[h]; n; n = n->next) {
        if (strcmp(n->name, cmd) == 0)
            return strdup(n->summary);
    }
    return NULL;
}

/* Validate command name to prevent shell injection */
static int valid_cmd_name(const char *cmd)
{
    for (const char *p = cmd; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_'
            && *p != '-' && *p != '+')
            return 0;
    }
    return cmd[0] != '\0';
}

/* Strip backspace formatting from man output (bold: x\bx, underline: _\bx) */
static void strip_backspaces(char *text)
{
    char *r = text, *w = text;
    while (*r) {
        if (r[0] != '\0' && r[1] == '\b' && r[2] != '\0') {
            r += 2; /* skip char + backspace, keep next char */
        } else if (*r == '\b') {
            r++; /* stray backspace, skip */
            if (w > text) w--; /* erase previous char */
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* Check if a line is a section header (all caps at column 0) */
static int is_section_header(const char *line)
{
    if (line[0] == ' ' || line[0] == '\t' || line[0] == '\0' || line[0] == '\n')
        return 0;
    /* Must have at least 2 uppercase chars */
    int upper = 0;
    for (const char *p = line; *p && *p != '\n'; p++) {
        if (isupper((unsigned char)*p)) upper++;
        else if (*p != ' ' && *p != '-') return 0;
    }
    return upper >= 2;
}

char *manscan_detail(const char *cmd, int max_bytes)
{
    if (!valid_cmd_name(cmd)) return NULL;
    if (max_bytes <= 0) max_bytes = 4096;

    char command[512];
    snprintf(command, sizeof(command), "man '%s' 2>/dev/null", cmd);

    FILE *fp = popen(command, "r");
    if (!fp) return NULL;

    /* Read full man page */
    char *raw = NULL;
    size_t raw_len = 0, raw_cap = 0;
    char buf[4096];

    while (fgets(buf, sizeof(buf), fp)) {
        size_t blen = strlen(buf);
        while (raw_len + blen + 1 > raw_cap) {
            raw_cap = (raw_cap == 0) ? 8192 : raw_cap * 2;
        }
        raw = realloc(raw, raw_cap);
        if (!raw) { pclose(fp); return NULL; }
        memcpy(raw + raw_len, buf, blen);
        raw_len += blen;
        raw[raw_len] = '\0';
    }
    pclose(fp);

    if (!raw || raw_len == 0) { free(raw); return NULL; }

    /* Strip backspace formatting */
    strip_backspaces(raw);

    /* Extract relevant sections: NAME, SYNOPSIS, OPTIONS (or DESCRIPTION) */
    char *result = malloc(max_bytes + 1);
    size_t res_len = 0;
    int in_wanted_section = 0;
    int found_options = 0;

    char *line = raw;
    while (line && *line && (int)res_len < max_bytes) {
        char *eol = strchr(line, '\n');
        size_t llen = eol ? (size_t)(eol - line) : strlen(line);

        /* Check for section header */
        if (is_section_header(line)) {
            /* Check if it's a section we want */
            if (strncmp(line, "NAME", 4) == 0 ||
                strncmp(line, "SYNOPSIS", 8) == 0 ||
                strncmp(line, "OPTIONS", 7) == 0 ||
                strncmp(line, "OPTION", 6) == 0) {
                in_wanted_section = 1;
                if (strncmp(line, "OPTIONS", 7) == 0 || strncmp(line, "OPTION", 6) == 0)
                    found_options = 1;
            } else if (strncmp(line, "DESCRIPTION", 11) == 0 && !found_options) {
                /* Fall back to DESCRIPTION if no OPTIONS */
                in_wanted_section = 1;
            } else {
                in_wanted_section = 0;
            }
        }

        if (in_wanted_section) {
            size_t avail = (size_t)max_bytes - res_len;
            size_t to_copy = llen < avail ? llen : avail;
            memcpy(result + res_len, line, to_copy);
            res_len += to_copy;
            if (res_len < (size_t)max_bytes)
                result[res_len++] = '\n';
        }

        line = eol ? eol + 1 : NULL;
    }

    free(raw);

    if (res_len == 0) {
        free(result);
        return NULL;
    }

    result[res_len] = '\0';
    return result;
}

char *manscan_enrich_pipeline(const char **cmds, int n)
{
    char *result = NULL;
    size_t res_len = 0, res_cap = 0;

    for (int i = 0; i < n; i++) {
        /* Extract first word */
        const char *p = cmds[i];
        while (*p == ' ' || *p == '\t') p++;
        char word[256];
        int wi = 0;
        while (*p && *p != ' ' && *p != '\t' && wi < 255)
            word[wi++] = *p++;
        word[wi] = '\0';

        char *summary = manscan_whatis(word);
        if (summary) {
            size_t line_len = strlen(word) + strlen(summary) + 4;
            while (res_len + line_len + 1 > res_cap) {
                res_cap = (res_cap == 0) ? 256 : res_cap * 2;
                result = realloc(result, res_cap);
            }
            res_len += snprintf(result + res_len, res_cap - res_len,
                                "%s: %s\n", word, summary);
            free(summary);
        }
    }

    return result;
}

int manscan_count(void)
{
    return man_count;
}
