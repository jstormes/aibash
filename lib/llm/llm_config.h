#ifndef BASH_LLM_CONFIG_H
#define BASH_LLM_CONFIG_H

/* LLM API configuration */
#define BASH_LLM_DEFAULT_API_URL    "http://localhost:8080/v1/chat/completions"
#define BASH_LLM_DEFAULT_MODEL      "default"
#define BASH_LLM_MAX_TOKENS         4096

/* History / context */
#define BASH_LLM_MAX_HISTORY        20
#define BASH_LLM_MAX_OUTPUT_CAPTURE 8192

/* Safety tiers */
#define SAFETY_AUTO     0   /* read-only: ls, cat, pwd, wc, head, grep */
#define SAFETY_CONFIRM  1   /* writes: cp, mv, write, mkdir */
#define SAFETY_DANGER   2   /* destructive: rm, run (arbitrary exec) */

/* Agentic loop */
#define BASH_LLM_MAX_ITERATIONS  20   /* max tool-call rounds per user input */

/* SSE streaming */
#define BASH_LLM_SSE_BUF_SIZE    4096
#define BASH_LLM_MAX_STREAM_TOOL_CALLS 16

/* Common buffer sizes */
#define BASH_LLM_PATH_BUF       4096   /* file paths, cwd */
#define BASH_LLM_CMD_BUF        2048   /* shell commands */
#define BASH_LLM_LINE_BUF       1024   /* config file lines */
#define BASH_LLM_MAX_PIPELINE   64     /* max commands in a pipeline */

/* Memory */
#define BASH_LLM_MEMORY_MAX         200    /* max memory entries */
#define BASH_LLM_MEMORY_WHISPER_MAX 2000   /* max chars in whisper text */

/* Max servers in config */
#define BASH_LLM_MAX_SERVERS 16

#endif /* BASH_LLM_CONFIG_H */
