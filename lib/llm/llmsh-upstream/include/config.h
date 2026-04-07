#ifndef LLMSH_CONFIG_H
#define LLMSH_CONFIG_H

/* LLM API configuration */
#define LLMSH_DEFAULT_API_URL    "http://localhost:8080/v1/chat/completions"
#define LLMSH_DEFAULT_MODEL      "default"
#define LLMSH_MAX_TOKENS         4096

/* History / context */
#define LLMSH_MAX_HISTORY        20
#define LLMSH_MAX_OUTPUT_CAPTURE  8192

/* Safety tiers */
#define SAFETY_AUTO     0   /* read-only: ls, cat, pwd, wc, head, grep */
#define SAFETY_CONFIRM  1   /* writes: cp, mv, write, mkdir */
#define SAFETY_DANGER   2   /* destructive: rm, run (arbitrary exec) */

/* Agentic loop */
#define LLMSH_MAX_ITERATIONS  20   /* max tool-call rounds per user input */

/* SSE streaming */
#define LLMSH_SSE_BUF_SIZE    4096
#define LLMSH_MAX_STREAM_TOOL_CALLS 16

/* Common buffer sizes */
#define LLMSH_PATH_BUF       4096   /* file paths, cwd */
#define LLMSH_CMD_BUF        2048   /* shell commands */
#define LLMSH_LINE_BUF       1024   /* config file lines */
#define LLMSH_MAX_PIPELINE   64     /* max commands in a pipeline */

/* Prompt */
#define LLMSH_PROMPT    "llmsh> "

#endif /* LLMSH_CONFIG_H */
