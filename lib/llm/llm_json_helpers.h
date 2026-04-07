#ifndef BASH_LLM_JSON_HELPERS_H
#define BASH_LLM_JSON_HELPERS_H

#include "cJSON.h"

/* Get a string field from a JSON object. Returns NULL if missing. */
const char *llm_json_get_string(const cJSON *obj, const char *key);

/* Get an int field, returns default_val if missing. */
int llm_json_get_int(const cJSON *obj, const char *key, int default_val);

/* Get a bool field, returns default_val if missing. */
int llm_json_get_bool(const cJSON *obj, const char *key, int default_val);

#endif /* BASH_LLM_JSON_HELPERS_H */
