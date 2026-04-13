#ifndef BASH_LLM_MEMORY_H
#define BASH_LLM_MEMORY_H

/* Memory entry */
typedef struct {
    int id;
    char *content;      /* the fact/preference/note */
    char *keywords;     /* comma-separated keywords */
    char *created;      /* ISO timestamp */
    long timestamp;     /* Unix epoch seconds (for sorting/conflict resolution) */
} mem_entry_t;

/*
 * Initialize memory system.
 * Creates memory_dir if needed, loads existing memories from memories.json.
 * Returns number of memories loaded.
 */
int llm_memory_init(const char *memory_dir, int max_entries);
void llm_memory_cleanup(void);

/* CRUD operations -- return 0 on success, -1 on error */
int llm_memory_save(const char *content, const char *keywords);
int llm_memory_forget(int id);
int llm_memory_forget_match(const char *text);

/* Search and list -- return malloced formatted text */
char *llm_memory_search(const char *query);
char *llm_memory_list(void);
int llm_memory_count(void);

/* Tool handlers (JSON in, malloced string out) */
char *llm_memory_tool_save(const char *args_json);
char *llm_memory_tool_search(const char *args_json);
char *llm_memory_tool_list(const char *args_json);
char *llm_memory_tool_forget(const char *args_json);

#endif /* BASH_LLM_MEMORY_H */
