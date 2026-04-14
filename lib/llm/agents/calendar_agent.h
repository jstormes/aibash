#ifndef CALENDAR_AGENT_H
#define CALENDAR_AGENT_H

/*
 * Calendar Side Agent
 *
 * Manages iCal events. Implements side_agent_t callbacks.
 * Uses libical for production storage (.ics files).
 * Dependencies injected for testability.
 */

#include "side_agent.h"

typedef char *(*cal_api_chat_fn)(const char *system_prompt,
                                 const char *user_message,
                                 int enable_thinking,
                                 const char *log_caller);

typedef struct {
    /* LLM API for classify + post_query extraction */
    cal_api_chat_fn api_chat;

    /* Event store operations — mock in tests, libical in production */
    int   (*store_load)(const char *path);
    int   (*store_save)(const char *path);
    int   (*store_add)(const char *summary, const char *dtstart,
                       const char *dtend, const char *description);
    int   (*store_remove)(int id);
    char *(*store_list)(void);
    int   (*store_count)(void);
    void  (*store_cleanup)(void);
} calendar_agent_deps_t;

/* Side agent callbacks */
int   calendar_agent_init(void *config);
void  calendar_agent_cleanup(void);
char *calendar_agent_pre_query(const char *query, const char *cwd);
void  calendar_agent_post_query(const char *query, const char *response, const char *cwd);

/* User commands */
int   calendar_agent_add(const char *summary, const char *start,
                         const char *end, const char *description);
int   calendar_agent_remove(int id);
char *calendar_agent_list(void);
int   calendar_agent_count(void);
int   calendar_agent_ready(void);

/* Testable init */
int   calendar_agent_init_with_deps(const char *storage_dir,
                                    const calendar_agent_deps_t *deps);

#endif /* CALENDAR_AGENT_H */
