#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <curl/curl.h>

#include "llm_api.h"
#include "llm_history.h"
#include "llm_side_agent.h"
#include "llm_log.h"
#include "llm_config.h"
#include "llm_streams.h"
#include "cJSON.h"

static char *g_api_url = NULL;
static char *g_model   = NULL;
static char *g_api_key = NULL;

/* Spinner state */
static int g_spinner_active = 0;
static int g_spinner_idx = 0;
static const char *spinner_frames[] = {
    "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
    "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
    "\xe2\xa0\x87", "\xe2\xa0\x8f"
};
#define SPINNER_FRAMES 10

/* Tools definition sent with every request */
static const char *TOOLS_JSON =
"["
"  {\"type\":\"function\",\"function\":{\"name\":\"ls\",\"description\":\"List directory contents\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Directory path (default: .)\"},\"flags\":{\"type\":\"string\",\"description\":\"Flags like -la\"}}}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"cat\",\"description\":\"Display file contents\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File to read\"}},\"required\":[\"path\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"description\":\"Read file with optional line range\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"start_line\":{\"type\":\"integer\"},\"end_line\":{\"type\":\"integer\"}},\"required\":[\"path\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"write_file\",\"description\":\"Write content to a file\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"pwd\",\"description\":\"Print working directory\",\"parameters\":{\"type\":\"object\",\"properties\":{}}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"cd\",\"description\":\"Change directory\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"cp\",\"description\":\"Copy files\",\"parameters\":{\"type\":\"object\",\"properties\":{\"src\":{\"type\":\"string\"},\"dst\":{\"type\":\"string\"}},\"required\":[\"src\",\"dst\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"mv\",\"description\":\"Move/rename files\",\"parameters\":{\"type\":\"object\",\"properties\":{\"src\":{\"type\":\"string\"},\"dst\":{\"type\":\"string\"}},\"required\":[\"src\",\"dst\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"rm\",\"description\":\"Remove files or directories\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"recursive\":{\"type\":\"boolean\"}},\"required\":[\"path\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"mkdir\",\"description\":\"Create directories\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"parents\":{\"type\":\"boolean\"}},\"required\":[\"path\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"grep\",\"description\":\"Search file contents\",\"parameters\":{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"recursive\":{\"type\":\"boolean\"},\"ignore_case\":{\"type\":\"boolean\"}},\"required\":[\"pattern\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"head\",\"description\":\"Show first N lines of a file\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"lines\":{\"type\":\"integer\"}},\"required\":[\"path\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"wc\",\"description\":\"Count lines/words/chars\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"flags\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"man\",\"description\":\"Get detailed man page for a command (synopsis, options, usage). Use when you need exact flags or options.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Command name to look up\"}},\"required\":[\"command\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"run\",\"description\":\"Execute a shell command pipeline. Use for any command not covered by builtins.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"pipeline\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Array of commands to pipe together\"},\"stdin_file\":{\"type\":\"string\"},\"stdout_file\":{\"type\":\"string\"},\"append\":{\"type\":\"boolean\"}},\"required\":[\"pipeline\"]}}}"
"]";

static const char *SYSTEM_PROMPT =
    "You are an AI assistant integrated into GNU Bash. The user typed input that "
    "was not recognized as a command, or they explicitly invoked you with the 'llm' "
    "builtin. Determine their intent and help them.\n\n"
    "You can type either plain English OR standard shell commands. You must determine which:\n\n"
    "1. DIRECT COMMAND: If the input looks like a standard shell command "
    "(starts with a known command name, has flags like -la, pipes |, "
    "redirections >, etc.), execute it directly via the 'run' tool exactly "
    "as typed. Do not modify or interpret it. Pass the entire command line "
    "as a single pipeline entry, splitting only on pipes (|).\n\n"
    "2. NATURAL LANGUAGE: If the input is English describing what the user "
    "wants, translate their intent into the appropriate tool calls.\n\n"
    "3. AMBIGUOUS: If you cannot tell whether input is a command or English "
    "(e.g., a single word like 'make' could be the build tool or a request), "
    "ask the user to clarify.\n\n"
    "You have built-in tools for common file operations (ls, cat, cp, mv, rm, "
    "mkdir, grep, head, wc, read_file, write_file, cd, pwd) and a 'run' tool "
    "for arbitrary shell command pipelines.\n\n"
    "Rules:\n"
    "- Prefer built-in tools over 'run' when possible for natural language.\n"
    "- ALWAYS use the 'write_file' tool to create or write files. Never use "
    "'run' with 'write', 'echo >', or 'cat >' for writing files.\n"
    "- For direct commands, always use 'run' to preserve exact behavior.\n"
    "- Use 'run' with pipeline array for pipes: [\"grep -r TODO\", \"wc -l\"].\n"
    "- Be concise in text responses. Show results, not explanations.\n"
    "- For destructive operations, the safety system will handle confirmation.\n"
    "- If a file read fails, check the path with ls before retrying.\n\n"
    "Man page support:\n"
    "- One-line command summaries are automatically included with 'run' results.\n"
    "- Use the 'man' tool when you need specific flags, options, or usage details.\n"
    "- Prefer 'man' over guessing flags -- it returns accurate system documentation.\n\n"
    "\n"
    "Context blocks:\n"
    "- Blocks marked [name context] ... [end name context] may appear at the\n"
    "  end of this system prompt. They are injected by background agents and\n"
    "  contain information relevant to the user's query.\n"
    "- ALWAYS incorporate this information into your answer.\n"
    "- Do NOT say 'context is empty' -- if a block is present, use it.";

/* ---- Interrupt check ---- */

/*
 * Check bash's interrupt_state for Ctrl-C.
 * We declare it extern here; it's defined in bash's sig.c.
 */
extern volatile sig_atomic_t interrupt_state;

static void spinner_stop(void);

static int curl_progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow)
{
    (void)clientp; (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;

    if (interrupt_state) {
        spinner_stop();
        return 1; /* non-zero aborts the transfer */
    }

    if (g_spinner_active) {
        fprintf(stderr, "\r%s ", spinner_frames[g_spinner_idx % SPINNER_FRAMES]);
        g_spinner_idx++;
    }
    return 0;
}

static void spinner_start(void)
{
    g_spinner_active = 1;
    g_spinner_idx = 0;
    fprintf(stderr, "\033[90mthinking...\033[0m");
    fflush(stderr);
}

static void spinner_stop(void)
{
    if (g_spinner_active) {
        fprintf(stderr, "\r\033[0m           \r");
        fflush(stderr);
        g_spinner_active = 0;
    }
}

/* ---- Shared helpers ---- */

static char *build_system_prompt(const char *cwd, const char *last_output,
                                  const char *matched_cmds, int first_word_is_cmd,
                                  const char *query)
{
    size_t sys_len = strlen(SYSTEM_PROMPT) + strlen(cwd) + 576 + 2048;
    if (matched_cmds) sys_len += strlen(matched_cmds) + 256;

    time_t now = time(NULL);
    char timestr[64];
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S %Z", localtime(&now));
    const char *user = getenv("USER");
    if (!user) user = getenv("LOGNAME");
    if (!user) user = "unknown";
    const char *hostname = getenv("HOSTNAME");
    if (!hostname) hostname = "";

    char *sys_buf = malloc(sys_len);
    int off = snprintf(sys_buf, sys_len,
             "%s\n\nCurrent directory: %s\nCurrent time: %s\nUser: %s%s%s\n",
             SYSTEM_PROMPT, cwd, timestr, user,
             hostname[0] ? "@" : "", hostname);

    if (matched_cmds) {
        off += snprintf(sys_buf + off, sys_len - off,
                 "\nThe following words in the user's input match installed "
                 "system commands: %s\n", matched_cmds);
        if (first_word_is_cmd) {
            off += snprintf(sys_buf + off, sys_len - off,
                     "NOTE: The FIRST word is a known command -- this is very "
                     "likely a direct shell command. Execute it via 'run'.\n");
        }
    }

    (void)last_output;

    /* Inject context from pre-query side agents */
    if (query) {
        char *pre_ctx = side_agents_pre_query(query, cwd);
        if (pre_ctx) {
            size_t ctx_len = strlen(pre_ctx);
            if ((size_t)off + ctx_len + 2 > sys_len) {
                sys_len = off + ctx_len + 256;
                sys_buf = realloc(sys_buf, sys_len);
            }
            off += snprintf(sys_buf + off, sys_len - off, "\n%s", pre_ctx);
            free(pre_ctx);
        }
    }

    return sys_buf;
}

static char *build_request_body(const char *sys_prompt, int streaming)
{
    char *messages_json = llm_history_build_messages(sys_prompt);
    if (!messages_json) return NULL;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", g_model);

    cJSON *msgs = cJSON_Parse(messages_json);
    free(messages_json);
    if (msgs) cJSON_AddItemToObject(req, "messages", msgs);

    cJSON *tools = cJSON_Parse(TOOLS_JSON);
    if (tools) cJSON_AddItemToObject(req, "tools", tools);

    cJSON_AddNumberToObject(req, "max_tokens", BASH_LLM_MAX_TOKENS);

    if (streaming)
        cJSON_AddBoolToObject(req, "stream", 1);

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return body;
}

static struct curl_slist *build_headers(void)
{
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (g_api_key) {
        char auth[512];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", g_api_key);
        headers = curl_slist_append(headers, auth);
    }
    return headers;
}

/* ---- Non-streaming ---- */

struct curl_buf {
    char *data;
    size_t size;
};

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct curl_buf *buf = userdata;
    size_t total = size * nmemb;
    char *tmp = realloc(buf->data, buf->size + total + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

static llm_response_t *parse_full_response(const char *json_str)
{
    cJSON *json = cJSON_Parse(json_str);
    if (!json) {
        fprintf(stderr, "bash-llm: failed to parse LLM response\n");
        return NULL;
    }

    llm_response_t *result = calloc(1, sizeof(*result));

    cJSON *choices = cJSON_GetObjectItem(json, "choices");
    cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
    cJSON *message = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;

    if (message) {
        cJSON *content = cJSON_GetObjectItem(message, "content");
        if (content && cJSON_IsString(content) && content->valuestring[0])
            result->text = strdup(content->valuestring);

        cJSON *tcs = cJSON_GetObjectItem(message, "tool_calls");
        if (tcs && cJSON_IsArray(tcs)) {
            int n = cJSON_GetArraySize(tcs);
            result->tool_calls = calloc(n, sizeof(tool_call_t));
            result->num_tool_calls = n;
            for (int i = 0; i < n; i++) {
                cJSON *tc = cJSON_GetArrayItem(tcs, i);
                cJSON *tc_id = cJSON_GetObjectItem(tc, "id");
                if (tc_id && cJSON_IsString(tc_id))
                    result->tool_calls[i].id = strdup(tc_id->valuestring);
                cJSON *fn = cJSON_GetObjectItem(tc, "function");
                if (fn) {
                    cJSON *name = cJSON_GetObjectItem(fn, "name");
                    cJSON *args = cJSON_GetObjectItem(fn, "arguments");
                    if (name && cJSON_IsString(name))
                        result->tool_calls[i].name = strdup(name->valuestring);
                    if (args && cJSON_IsString(args))
                        result->tool_calls[i].arguments = strdup(args->valuestring);
                }
            }
        }
    }

    cJSON_Delete(json);
    return result;
}

static int g_curl_initialized = 0;

int llm_api_init(const char *api_url, const char *model, const char *api_key)
{
    if (!g_curl_initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        g_curl_initialized = 1;
    }
    free(g_api_url);
    free(g_model);
    free(g_api_key);
    g_api_url = strdup(api_url);
    g_model   = strdup(model);
    g_api_key = api_key ? strdup(api_key) : NULL;
    return 0;
}

void llm_api_cleanup(void)
{
    free(g_api_url);  g_api_url = NULL;
    free(g_model);    g_model = NULL;
    free(g_api_key);  g_api_key = NULL;
    if (g_curl_initialized) {
        curl_global_cleanup();
        g_curl_initialized = 0;
    }
}

llm_response_t *llm_chat(const char *user_input, const char *cwd,
                          const char *last_output,
                          const char *matched_cmds, int first_word_is_cmd)
{
    (void)user_input;

    char *sys_prompt = build_system_prompt(cwd, last_output, matched_cmds, first_word_is_cmd, user_input);
    char *body = build_request_body(sys_prompt, 0);
    free(sys_prompt);
    if (!body) return NULL;

    CURL *curl = curl_easy_init();
    if (!curl) { free(body); return NULL; }

    struct curl_buf response = {NULL, 0};
    struct curl_slist *headers = build_headers();

    curl_easy_setopt(curl, CURLOPT_URL, g_api_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    spinner_start();
    CURLcode res = curl_easy_perform(curl);
    spinner_stop();

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0
                      + (t_end.tv_nsec - t_start.tv_nsec) / 1e6;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "bash-llm: curl error: %s\n", curl_easy_strerror(res));
        llm_log_api_call("main_chat", body, "(curl error)", elapsed_ms);
        free(body);
        free(response.data);
        return NULL;
    }

    llm_log_api_call("main_chat", body, response.data, elapsed_ms);
    free(body);

    llm_response_t *result = parse_full_response(response.data);
    free(response.data);
    return result;
}

/* ---- SSE Streaming ---- */

typedef struct {
    char *line_buf;
    size_t line_len;
    size_t line_cap;

    char *full_text;
    size_t text_len;
    size_t text_cap;

    tool_call_t tool_calls[BASH_LLM_MAX_STREAM_TOOL_CALLS];
    char *tc_args[BASH_LLM_MAX_STREAM_TOOL_CALLS];
    size_t tc_args_len[BASH_LLM_MAX_STREAM_TOOL_CALLS];
    size_t tc_args_cap[BASH_LLM_MAX_STREAM_TOOL_CALLS];
    int num_tool_calls;

    llm_stream_cbs cbs;
} sse_state_t;

static void sse_state_init(sse_state_t *st, llm_stream_cbs *cbs)
{
    memset(st, 0, sizeof(*st));
    if (cbs) st->cbs = *cbs;
}

static void sse_state_cleanup(sse_state_t *st)
{
    free(st->line_buf);
}

static void sse_append_text(sse_state_t *st, const char *s, size_t len)
{
    if (st->text_len + len + 1 > st->text_cap) {
        st->text_cap = (st->text_cap == 0) ? 1024 : st->text_cap * 2;
        while (st->text_len + len + 1 > st->text_cap)
            st->text_cap *= 2;
        st->full_text = realloc(st->full_text, st->text_cap);
    }
    memcpy(st->full_text + st->text_len, s, len);
    st->text_len += len;
    st->full_text[st->text_len] = '\0';
}

static void sse_append_tc_args(sse_state_t *st, int idx, const char *s, size_t len)
{
    if (idx < 0 || idx >= BASH_LLM_MAX_STREAM_TOOL_CALLS) return;
    size_t *al = &st->tc_args_len[idx];
    size_t *ac = &st->tc_args_cap[idx];
    if (*al + len + 1 > *ac) {
        *ac = (*ac == 0) ? 256 : *ac * 2;
        while (*al + len + 1 > *ac) *ac *= 2;
        st->tc_args[idx] = realloc(st->tc_args[idx], *ac);
    }
    memcpy(st->tc_args[idx] + *al, s, len);
    *al += len;
    st->tc_args[idx][*al] = '\0';
}

static void sse_process_data(sse_state_t *st, const char *data)
{
    if (strcmp(data, "[DONE]") == 0)
        return;

    cJSON *json = cJSON_Parse(data);
    if (!json) return;

    cJSON *choices = cJSON_GetObjectItem(json, "choices");
    cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
    cJSON *delta = choice0 ? cJSON_GetObjectItem(choice0, "delta") : NULL;

    if (delta) {
        cJSON *content = cJSON_GetObjectItem(delta, "content");
        if (content && cJSON_IsString(content) && content->valuestring[0]) {
            spinner_stop();
            const char *tok = content->valuestring;
            size_t toklen = strlen(tok);
            sse_append_text(st, tok, toklen);
            if (st->cbs.on_token)
                st->cbs.on_token(tok, st->cbs.userdata);
        }

        const char *think_keys[] = {"reasoning_content", "thinking", "reasoning", NULL};
        for (int k = 0; think_keys[k]; k++) {
            cJSON *think = cJSON_GetObjectItem(delta, think_keys[k]);
            if (think && cJSON_IsString(think) && think->valuestring[0]) {
                if (st->cbs.on_thinking)
                    st->cbs.on_thinking(think->valuestring, st->cbs.userdata);
                break;
            }
        }

        cJSON *tcs = cJSON_GetObjectItem(delta, "tool_calls");
        if (tcs && cJSON_IsArray(tcs)) {
            int n = cJSON_GetArraySize(tcs);
            for (int i = 0; i < n; i++) {
                cJSON *tc = cJSON_GetArrayItem(tcs, i);

                cJSON *idx_j = cJSON_GetObjectItem(tc, "index");
                int idx = idx_j ? idx_j->valueint : st->num_tool_calls;
                if (idx >= BASH_LLM_MAX_STREAM_TOOL_CALLS) continue;

                if (idx >= st->num_tool_calls)
                    st->num_tool_calls = idx + 1;

                cJSON *tc_id = cJSON_GetObjectItem(tc, "id");
                if (tc_id && cJSON_IsString(tc_id) && !st->tool_calls[idx].id)
                    st->tool_calls[idx].id = strdup(tc_id->valuestring);

                cJSON *fn = cJSON_GetObjectItem(tc, "function");
                if (fn) {
                    cJSON *name = cJSON_GetObjectItem(fn, "name");
                    if (name && cJSON_IsString(name) && !st->tool_calls[idx].name)
                        st->tool_calls[idx].name = strdup(name->valuestring);

                    cJSON *args = cJSON_GetObjectItem(fn, "arguments");
                    if (args && cJSON_IsString(args)) {
                        size_t alen = strlen(args->valuestring);
                        if (alen > 0)
                            sse_append_tc_args(st, idx, args->valuestring, alen);
                    }
                }
            }
        }
    }

    cJSON_Delete(json);
}

static void sse_process_buffer(sse_state_t *st)
{
    size_t start = 0;
    for (size_t i = 0; i < st->line_len; i++) {
        if (st->line_buf[i] == '\n') {
            st->line_buf[i] = '\0';
            char *line = st->line_buf + start;

            if (line[0] != '\0') {
                if (strncmp(line, "data: ", 6) == 0)
                    sse_process_data(st, line + 6);
                else if (strncmp(line, "data:", 5) == 0)
                    sse_process_data(st, line + 5);
            }

            start = i + 1;
        }
    }

    if (start > 0 && start < st->line_len) {
        memmove(st->line_buf, st->line_buf + start, st->line_len - start);
        st->line_len -= start;
    } else if (start == st->line_len) {
        st->line_len = 0;
    }
}

static size_t curl_sse_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    sse_state_t *st = userdata;
    size_t total = size * nmemb;

    if (st->line_len + total + 1 > st->line_cap) {
        st->line_cap = (st->line_cap == 0) ? BASH_LLM_SSE_BUF_SIZE : st->line_cap * 2;
        while (st->line_len + total + 1 > st->line_cap)
            st->line_cap *= 2;
        st->line_buf = realloc(st->line_buf, st->line_cap);
    }
    memcpy(st->line_buf + st->line_len, ptr, total);
    st->line_len += total;
    st->line_buf[st->line_len] = '\0';

    sse_process_buffer(st);

    return total;
}

llm_response_t *llm_chat_stream(const char *user_input, const char *cwd,
                                 const char *last_output,
                                 const char *matched_cmds, int first_word_is_cmd,
                                 llm_stream_cbs *cbs)
{
    if (!cbs || !cbs->on_token)
        return llm_chat(user_input, cwd, last_output, matched_cmds, first_word_is_cmd);

    (void)user_input;

    char *sys_prompt = build_system_prompt(cwd, last_output, matched_cmds, first_word_is_cmd, user_input);
    char *body = build_request_body(sys_prompt, 1);
    free(sys_prompt);
    if (!body) return NULL;

    stream_api_output("\xe2\x86\x92", g_api_url);

    CURL *curl = curl_easy_init();
    if (!curl) { free(body); return NULL; }

    sse_state_t sse;
    sse_state_init(&sse, cbs);

    struct curl_slist *headers = build_headers();

    curl_easy_setopt(curl, CURLOPT_URL, g_api_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_sse_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sse);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    spinner_start();
    CURLcode res = curl_easy_perform(curl);
    spinner_stop();

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0
                      + (t_end.tv_nsec - t_start.tv_nsec) / 1e6;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "bash-llm: curl error: %s\n", curl_easy_strerror(res));
        llm_log_api_call("main_stream", body, "(curl error)", elapsed_ms);
        free(body);
        sse_state_cleanup(&sse);
        return NULL;
    }

    /* Log streaming result as a summary response */
    {
        char dbg[256];
        snprintf(dbg, sizeof(dbg), "text=%zuB, tool_calls=%d",
                 sse.text_len, sse.num_tool_calls);
        stream_api_output("\xe2\x86\x90", dbg);

        /* Build a summary JSON for the log */
        cJSON *resp_summary = cJSON_CreateObject();
        cJSON_AddStringToObject(resp_summary, "type", "sse_stream");
        cJSON_AddNumberToObject(resp_summary, "text_bytes", (double)sse.text_len);
        cJSON_AddNumberToObject(resp_summary, "tool_calls", sse.num_tool_calls);
        if (sse.full_text && sse.text_len > 0)
            cJSON_AddStringToObject(resp_summary, "text", sse.full_text);
        for (int i = 0; i < sse.num_tool_calls; i++) {
            char key[32];
            snprintf(key, sizeof(key), "tool_%d", i);
            cJSON *tc = cJSON_CreateObject();
            if (sse.tool_calls[i].name)
                cJSON_AddStringToObject(tc, "name", sse.tool_calls[i].name);
            if (sse.tc_args[i])
                cJSON_AddStringToObject(tc, "arguments", sse.tc_args[i]);
            cJSON_AddItemToObject(resp_summary, key, tc);
        }
        char *resp_str = cJSON_PrintUnformatted(resp_summary);
        cJSON_Delete(resp_summary);

        llm_log_api_call("main_stream", body, resp_str, elapsed_ms);
        free(resp_str);
    }
    free(body);

    llm_response_t *result = calloc(1, sizeof(*result));

    if (sse.full_text && sse.text_len > 0)
        result->text = sse.full_text;
    else
        free(sse.full_text);

    if (sse.num_tool_calls > 0) {
        result->num_tool_calls = sse.num_tool_calls;
        result->tool_calls = calloc(sse.num_tool_calls, sizeof(tool_call_t));
        for (int i = 0; i < sse.num_tool_calls; i++) {
            result->tool_calls[i].id = sse.tool_calls[i].id;
            result->tool_calls[i].name = sse.tool_calls[i].name;
            result->tool_calls[i].arguments = sse.tc_args[i];
        }
    }

    sse_state_cleanup(&sse);
    return result;
}

void llm_response_free(llm_response_t *resp)
{
    if (!resp) return;
    free(resp->text);
    for (int i = 0; i < resp->num_tool_calls; i++) {
        free(resp->tool_calls[i].id);
        free(resp->tool_calls[i].name);
        free(resp->tool_calls[i].arguments);
    }
    free(resp->tool_calls);
    free(resp);
}
