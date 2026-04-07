#ifndef BASH_LLM_MANSCAN_H
#define BASH_LLM_MANSCAN_H

int llm_manscan_init(void);
void llm_manscan_cleanup(void);
char *llm_manscan_whatis(const char *cmd);
char *llm_manscan_detail(const char *cmd, int max_bytes);
char *llm_manscan_enrich_pipeline(const char **cmds, int n);
int llm_manscan_count(void);

#endif /* BASH_LLM_MANSCAN_H */
