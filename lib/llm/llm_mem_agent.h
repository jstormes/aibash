#ifndef BASH_LLM_MEM_AGENT_H
#define BASH_LLM_MEM_AGENT_H

/*
 * Initialize memory agent (call after llm_memory_init and config load).
 * Stores the API connection for the memory agent LLM.
 */
int  llm_mem_agent_init(const char *api_url, const char *model, const char *api_key);
void llm_mem_agent_cleanup(void);

/* Returns 1 if memory agent is initialized and ready */
int llm_mem_agent_ready(void);

/*
 * Extract facts from a conversation exchange and save to memory.
 * Designed to be called in a forked child process.
 * conversation: the full exchange text (user query + assistant response)
 * memory_dir: path to ~/.aibash_memories for reloading in child
 * memory_max: max entries config value
 */
void llm_mem_agent_extract(const char *conversation, const char *memory_dir, int memory_max);

#endif /* BASH_LLM_MEM_AGENT_H */
