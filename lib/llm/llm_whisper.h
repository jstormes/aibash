#ifndef BASH_LLM_WHISPER_H
#define BASH_LLM_WHISPER_H

/*
 * LLM-powered whisper: parallel agents that search memories for
 * context relevant to a query. Used as fallback when fast keyword
 * search finds nothing.
 *
 * Forks two agents in parallel (user context + project context),
 * each calling the memory agent LLM. Results merged with 5-second timeout.
 *
 * Returns malloced whisper text, or NULL if nothing found / timeout.
 * Caller frees.
 */
char *llm_whisper_agents(const char *query);

#endif /* BASH_LLM_WHISPER_H */
