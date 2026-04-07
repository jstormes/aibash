#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pathscan.h"

#define HASH_SIZE 8192

typedef struct hash_node {
    char *name;
    struct hash_node *next;
} hash_node_t;

static hash_node_t *cmd_table[HASH_SIZE];
static int cmd_count = 0;

static unsigned int hash_str(const char *s)
{
    unsigned int h = 5381;
    while (*s)
        h = ((h << 5) + h) + (unsigned char)*s++;
    return h % HASH_SIZE;
}

static int table_insert(const char *name)
{
    unsigned int h = hash_str(name);
    for (hash_node_t *n = cmd_table[h]; n; n = n->next) {
        if (strcmp(n->name, name) == 0)
            return 0; /* already exists */
    }
    hash_node_t *node = malloc(sizeof(*node));
    node->name = strdup(name);
    node->next = cmd_table[h];
    cmd_table[h] = node;
    return 1;
}

int pathscan_init(void)
{
    const char *path_env = getenv("PATH");
    if (!path_env) return 0;

    char *path_copy = strdup(path_env);
    memset(cmd_table, 0, sizeof(cmd_table));
    cmd_count = 0;

    char *saveptr = NULL;
    char *dir = strtok_r(path_copy, ":", &saveptr);

    while (dir) {
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d))) {
                if (ent->d_name[0] == '.')
                    continue;

                char fullpath[4096];
                snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, ent->d_name);

                if (access(fullpath, X_OK) == 0 && table_insert(ent->d_name))
                    cmd_count++;
            }
            closedir(d);
        }
        dir = strtok_r(NULL, ":", &saveptr);
    }

    free(path_copy);
    return cmd_count;
}

void pathscan_cleanup(void)
{
    for (int i = 0; i < HASH_SIZE; i++) {
        hash_node_t *n = cmd_table[i];
        while (n) {
            hash_node_t *next = n->next;
            free(n->name);
            free(n);
            n = next;
        }
        cmd_table[i] = NULL;
    }
    cmd_count = 0;
}

int pathscan_lookup(const char *name)
{
    unsigned int h = hash_str(name);
    for (hash_node_t *n = cmd_table[h]; n; n = n->next) {
        if (strcmp(n->name, name) == 0)
            return 1;
    }
    return 0;
}

/*
 * Extract a word from input at position *pos.
 * Skips leading whitespace/punctuation. Returns malloced word or NULL.
 */
static char *next_word(const char *input, int *pos)
{
    int i = *pos;

    /* Skip non-alnum (whitespace, punctuation) but keep - and _ for command names */
    while (input[i] && !isalnum((unsigned char)input[i])
           && input[i] != '-' && input[i] != '_' && input[i] != '.')
        i++;

    if (!input[i]) {
        *pos = i;
        return NULL;
    }

    int start = i;
    while (input[i] && (isalnum((unsigned char)input[i])
           || input[i] == '-' || input[i] == '_' || input[i] == '.'
           || input[i] == '+'))
        i++;

    int len = i - start;
    char *word = malloc(len + 1);
    memcpy(word, input + start, len);
    word[len] = '\0';

    *pos = i;
    return word;
}

char *pathscan_match_input(const char *input, int *first_word_match)
{
    *first_word_match = 0;

    /* Dedup matches for this input */
    char *seen[64];
    int seen_count = 0;

    char *result = NULL;
    size_t res_len = 0, res_cap = 0;

    int pos = 0;
    int word_index = 0;

    while (1) {
        char *word = next_word(input, &pos);
        if (!word) break;

        /* Skip very short words (a, an, to, etc.) - unlikely command names */
        if (strlen(word) <= 1) {
            free(word);
            word_index++;
            continue;
        }

        /* Check if it's a known command */
        if (pathscan_lookup(word)) {
            /* Check for duplicates in this input */
            int dup = 0;
            for (int i = 0; i < seen_count; i++) {
                if (strcmp(seen[i], word) == 0) { dup = 1; break; }
            }

            if (!dup && seen_count < 64) {
                seen[seen_count++] = word;

                if (word_index == 0)
                    *first_word_match = 1;

                /* Append to result */
                size_t wlen = strlen(word);
                while (res_len + wlen + 3 > res_cap) {
                    res_cap = (res_cap == 0) ? 256 : res_cap * 2;
                    result = realloc(result, res_cap);
                }
                if (res_len > 0) {
                    result[res_len++] = ',';
                    result[res_len++] = ' ';
                }
                memcpy(result + res_len, word, wlen);
                res_len += wlen;
                result[res_len] = '\0';

                word_index++;
                continue; /* don't free, stored in seen[] */
            }
        }

        free(word);
        word_index++;
    }

    /* Free seen entries */
    for (int i = 0; i < seen_count; i++)
        free(seen[i]);

    return result;
}
