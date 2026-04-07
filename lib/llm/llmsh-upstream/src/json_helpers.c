#include "json_helpers.h"

const char *json_get_string(const cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsString(item))
        return item->valuestring;
    return NULL;
}

int json_get_int(const cJSON *obj, const char *key, int default_val)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsNumber(item))
        return item->valueint;
    return default_val;
}

int json_get_bool(const cJSON *obj, const char *key, int default_val)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item)
        return cJSON_IsTrue(item);
    return default_val;
}
