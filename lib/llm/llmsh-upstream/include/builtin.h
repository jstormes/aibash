#ifndef LLMSH_BUILTIN_H
#define LLMSH_BUILTIN_H

/* Built-in tool handler. Returns malloced output string, or NULL on error. */
typedef char *(*builtin_fn)(const char *args_json);

typedef struct {
    const char *name;
    builtin_fn  handler;
    int         safety_tier;
    const char *description;
} builtin_t;

/* Initialize builtin table */
void builtin_init(void);

/* Look up a builtin by name. Returns NULL if not found. */
const builtin_t *builtin_find(const char *name);

/* Individual builtins */
char *builtin_ls(const char *args_json);
char *builtin_cat(const char *args_json);
char *builtin_pwd(const char *args_json);
char *builtin_cd(const char *args_json);
char *builtin_cp(const char *args_json);
char *builtin_mv(const char *args_json);
char *builtin_rm(const char *args_json);
char *builtin_mkdir(const char *args_json);
char *builtin_grep(const char *args_json);
char *builtin_head(const char *args_json);
char *builtin_wc(const char *args_json);
char *builtin_write_file(const char *args_json);
char *builtin_read_file(const char *args_json);
char *builtin_man(const char *args_json);

#endif /* LLMSH_BUILTIN_H */
