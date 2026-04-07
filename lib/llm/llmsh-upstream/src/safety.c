#include <stdio.h>
#include <string.h>

#include "safety.h"

int safety_confirm(const char *action_description)
{
    fprintf(stderr, "\033[33m[confirm]\033[0m %s\n", action_description);
    fprintf(stderr, "Allow? [y/N] ");

    char buf[16];
    if (!fgets(buf, sizeof(buf), stdin))
        return 0;

    return (buf[0] == 'y' || buf[0] == 'Y');
}
