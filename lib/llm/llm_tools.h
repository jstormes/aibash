#ifndef BASH_LLM_TOOLS_H
#define BASH_LLM_TOOLS_H

/* Built-in tool handler. Returns malloced output string, or NULL on error. */
typedef char *(*llm_builtin_fn)(const char *args_json);

typedef struct {
    const char *name;
    llm_builtin_fn handler;
    int         safety_tier;
    const char *description;
} llm_builtin_t;

/* Initialize builtin table */
void llm_tools_init(void);

/* Look up a builtin by name. Returns NULL if not found. */
const llm_builtin_t *llm_tools_find(const char *name);

/* Individual builtins */
char *llm_tool_ls(const char *args_json);
char *llm_tool_cat(const char *args_json);
char *llm_tool_pwd(const char *args_json);
char *llm_tool_cd(const char *args_json);
char *llm_tool_cp(const char *args_json);
char *llm_tool_mv(const char *args_json);
char *llm_tool_rm(const char *args_json);
char *llm_tool_mkdir(const char *args_json);
char *llm_tool_grep(const char *args_json);
char *llm_tool_head(const char *args_json);
char *llm_tool_wc(const char *args_json);
char *llm_tool_write_file(const char *args_json);
char *llm_tool_read_file(const char *args_json);
char *llm_tool_man(const char *args_json);

#endif /* BASH_LLM_TOOLS_H */
