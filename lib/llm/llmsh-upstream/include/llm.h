#ifndef LLMSH_LLM_H
#define LLMSH_LLM_H

/* Tool call returned by the LLM */
typedef struct {
    char *id;           /* tool_call_id for matching response */
    char *name;
    char *arguments;    /* raw JSON string of arguments */
} tool_call_t;

/* LLM response: either text or a tool call */
typedef struct {
    char *text;         /* assistant text (may be NULL) */
    tool_call_t *tool_calls;
    int num_tool_calls;
} llm_response_t;

int  llm_init(const char *api_url, const char *model, const char *api_key);
void llm_cleanup(void);

/* Callback invoked for each text token during streaming */
typedef void (*llm_token_cb)(const char *token, void *userdata);

/* Callbacks for streaming */
typedef struct {
    llm_token_cb on_token;      /* chat text delta */
    llm_token_cb on_thinking;   /* reasoning/thinking delta */
    void *userdata;
} llm_stream_cbs;

/*
 * Send user input to the LLM (blocking, non-streaming).
 */
llm_response_t *llm_chat(const char *user_input, const char *cwd,
                          const char *last_output,
                          const char *matched_cmds, int first_word_is_cmd);

/*
 * Send user input to the LLM with SSE streaming.
 * cbs contains callbacks for text and thinking tokens.
 * Falls back to non-streaming if cbs is NULL or cbs->on_token is NULL.
 */
llm_response_t *llm_chat_stream(const char *user_input, const char *cwd,
                                 const char *last_output,
                                 const char *matched_cmds, int first_word_is_cmd,
                                 llm_stream_cbs *cbs);

void llm_response_free(llm_response_t *resp);

#endif /* LLMSH_LLM_H */
