#ifndef BASH_LLM_LOG_H
#define BASH_LLM_LOG_H

#include "cJSON.h"

/*
 * Initialize logging. Creates log directory if needed.
 * Cleans up log files older than 1 day.
 * log_dir: path to log directory (e.g., ~/.aibash_memories/logs/)
 */
void llm_log_init(const char *log_dir);

/*
 * Log an API request/response pair.
 * Creates a new file per call: <timestamp>_<caller>.json
 *
 * caller: identifier (e.g., "main_chat", "mem-search", "extract", "cleanup")
 * request_body: the JSON body sent to the API (will be pretty-printed)
 * response_body: the raw response from the API (will be pretty-printed)
 * elapsed_ms: time taken for the API call
 */
void llm_log_api_call(const char *caller, const char *request_body,
                       const char *response_body, double elapsed_ms);

#endif /* BASH_LLM_LOG_H */
