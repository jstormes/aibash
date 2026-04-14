#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libical/ical.h>

#include "llm_ical_store.h"

/* ---- Internal event array ---- */

#define ICAL_MAX_EVENTS 256

typedef struct {
    int id;
    char *summary;
    char *dtstart;
    char *dtend;
    char *description;
} ical_event_t;

static ical_event_t g_events[ICAL_MAX_EVENTS];
static int g_count = 0;
static int g_next_id = 1;
static char g_store_path[4096];

static void free_event(ical_event_t *e)
{
    free(e->summary);
    free(e->description);
    free(e->dtstart);
    free(e->dtend);
    e->summary = e->description = e->dtstart = e->dtend = NULL;
}

/* Format icaltime to "YYYY-MM-DD HH:MM" */
static char *format_time(icaltimetype t)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
             t.year, t.month, t.day, t.hour, t.minute);
    return strdup(buf);
}

/* Parse "YYYY-MM-DD HH:MM" to icaltime */
static icaltimetype parse_time(const char *s)
{
    icaltimetype t = icaltime_null_time();
    if (!s || !s[0]) return t;

    int y, mo, d, h = 0, mi = 0;
    if (sscanf(s, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) >= 3) {
        t.year = y;
        t.month = mo;
        t.day = d;
        t.hour = h;
        t.minute = mi;
        t.second = 0;
        t.is_date = (h == 0 && mi == 0) ? 1 : 0;
    }
    return t;
}

/* ---- Load from .ics file ---- */

int llm_ical_load(const char *path)
{
    snprintf(g_store_path, sizeof(g_store_path), "%s", path);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return 0; }

    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    icalcomponent *cal = icalparser_parse_string(buf);
    free(buf);
    if (!cal) return 0;

    g_count = 0;
    g_next_id = 1;

    icalcomponent *c;
    for (c = icalcomponent_get_first_component(cal, ICAL_VEVENT_COMPONENT);
         c && g_count < ICAL_MAX_EVENTS;
         c = icalcomponent_get_next_component(cal, ICAL_VEVENT_COMPONENT)) {

        icalproperty *p_sum = icalcomponent_get_first_property(c, ICAL_SUMMARY_PROPERTY);
        icalproperty *p_start = icalcomponent_get_first_property(c, ICAL_DTSTART_PROPERTY);
        icalproperty *p_end = icalcomponent_get_first_property(c, ICAL_DTEND_PROPERTY);
        icalproperty *p_desc = icalcomponent_get_first_property(c, ICAL_DESCRIPTION_PROPERTY);

        /* Try to read X-AIBASH-ID custom property for our ID */
        icalproperty *p_id = icalcomponent_get_first_property(c, ICAL_X_PROPERTY);
        int event_id = g_next_id;
        while (p_id) {
            const char *xname = icalproperty_get_x_name(p_id);
            if (xname && strcmp(xname, "X-AIBASH-ID") == 0) {
                event_id = atoi(icalproperty_get_x(p_id));
                break;
            }
            p_id = icalcomponent_get_next_property(c, ICAL_X_PROPERTY);
        }

        ical_event_t *e = &g_events[g_count];
        e->id = event_id;
        e->summary = p_sum ? strdup(icalproperty_get_summary(p_sum)) : strdup("");
        e->dtstart = p_start ? format_time(icalproperty_get_dtstart(p_start)) : strdup("");
        e->dtend = p_end ? format_time(icalproperty_get_dtend(p_end)) : strdup("");
        e->description = p_desc ? strdup(icalproperty_get_description(p_desc)) : strdup("");

        if (e->id >= g_next_id) g_next_id = e->id + 1;
        g_count++;
    }

    icalcomponent_free(cal);
    return g_count;
}

/* ---- Save to .ics file ---- */

int llm_ical_save(const char *path)
{
    icalcomponent *cal = icalcomponent_new_vcalendar();
    icalcomponent_add_property(cal, icalproperty_new_version("2.0"));
    icalcomponent_add_property(cal, icalproperty_new_prodid("-//aibash//EN"));

    for (int i = 0; i < g_count; i++) {
        ical_event_t *e = &g_events[i];

        icalcomponent *event = icalcomponent_new_vevent();
        icalcomponent_add_property(event, icalproperty_new_summary(e->summary));
        icalcomponent_add_property(event, icalproperty_new_dtstart(parse_time(e->dtstart)));

        if (e->dtend && e->dtend[0])
            icalcomponent_add_property(event, icalproperty_new_dtend(parse_time(e->dtend)));

        if (e->description && e->description[0])
            icalcomponent_add_property(event, icalproperty_new_description(e->description));

        /* Store our ID as a custom property */
        char id_str[32];
        snprintf(id_str, sizeof(id_str), "%d", e->id);
        icalproperty *p_id = icalproperty_new_x(id_str);
        icalproperty_set_x_name(p_id, "X-AIBASH-ID");
        icalcomponent_add_property(event, p_id);

        icalcomponent_add_component(cal, event);
    }

    char *ics_str = icalcomponent_as_ical_string(cal);
    FILE *f = fopen(path, "w");
    if (!f) {
        icalcomponent_free(cal);
        return -1;
    }
    fputs(ics_str, f);
    fclose(f);
    icalcomponent_free(cal);
    return 0;
}

/* ---- CRUD ---- */

int llm_ical_add(const char *summary, const char *dtstart,
                  const char *dtend, const char *description)
{
    if (g_count >= ICAL_MAX_EVENTS) return -1;

    ical_event_t *e = &g_events[g_count];
    e->id = g_next_id++;
    e->summary = strdup(summary ? summary : "");
    e->dtstart = strdup(dtstart ? dtstart : "");
    e->dtend = strdup(dtend ? dtend : "");
    e->description = strdup(description ? description : "");
    g_count++;

    if (g_store_path[0])
        llm_ical_save(g_store_path);

    return e->id;
}

int llm_ical_remove(int id)
{
    for (int i = 0; i < g_count; i++) {
        if (g_events[i].id == id) {
            free_event(&g_events[i]);
            for (int j = i; j < g_count - 1; j++)
                g_events[j] = g_events[j + 1];
            g_count--;
            if (g_store_path[0])
                llm_ical_save(g_store_path);
            return 0;
        }
    }
    return -1;
}

char *llm_ical_list(void)
{
    if (g_count == 0) return strdup("(no calendar events)\n");

    char *buf = malloc(8192);
    size_t off = 0;
    for (int i = 0; i < g_count; i++) {
        ical_event_t *e = &g_events[i];
        off += snprintf(buf + off, 8192 - off, "[%d] %s: %s",
                        e->id, e->dtstart, e->summary);
        if (e->dtend && e->dtend[0])
            off += snprintf(buf + off, 8192 - off, " (until %s)", e->dtend);
        if (e->description && e->description[0])
            off += snprintf(buf + off, 8192 - off, " — %s", e->description);
        off += snprintf(buf + off, 8192 - off, "\n");
    }
    return buf;
}

int llm_ical_count(void)
{
    return g_count;
}

void llm_ical_cleanup(void)
{
    for (int i = 0; i < g_count; i++)
        free_event(&g_events[i]);
    g_count = 0;
    g_next_id = 1;
    g_store_path[0] = 0;
}
