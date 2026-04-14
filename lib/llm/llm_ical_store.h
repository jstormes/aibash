#ifndef LLM_ICAL_STORE_H
#define LLM_ICAL_STORE_H

/*
 * iCal event store using libical.
 * Reads/writes standard .ics files.
 * Used as the production store for the calendar side agent.
 */

int   llm_ical_load(const char *path);
int   llm_ical_save(const char *path);
int   llm_ical_add(const char *summary, const char *dtstart,
                    const char *dtend, const char *description);
int   llm_ical_remove(int id);
char *llm_ical_list(void);
int   llm_ical_count(void);
void  llm_ical_cleanup(void);

#endif /* LLM_ICAL_STORE_H */
