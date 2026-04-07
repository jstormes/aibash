#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "llm_safety.h"

/*
 * We use bash's own read builtin via evalstring for the confirmation
 * prompt. This ensures readline works correctly and avoids stdin
 * buffering issues. The functions are declared here to avoid pulling
 * in bash headers from the library.
 */
extern int evalstring(char *, const char *, int);
extern char *get_string_value(const char *);
#define SEVAL_NOHIST 0x004

int llm_safety_confirm(const char *action_description)
{
    /* Flush both streams to prevent output ordering issues */
    fflush(stdout);
    fflush(stderr);

    /* Print the action description to stderr */
    fprintf(stderr, "\n\033[33m[confirm]\033[0m %s\n", action_description);
    fflush(stderr);

    /* Use bash's read builtin for the prompt -- this goes through
       readline properly and handles terminal state correctly */
    char *cmd = strdup("read -p $'\\033[33mAllow? [y/N]\\033[0m ' __llm_confirm_reply");
    evalstring(cmd, "llm_confirm", SEVAL_NOHIST);

    /* Check the reply */
    const char *reply = get_string_value("__llm_confirm_reply");
    int confirmed = 0;
    if (reply && (reply[0] == 'y' || reply[0] == 'Y'))
        confirmed = 1;

    /* Clean up the variable */
    char *cmd2 = strdup("unset __llm_confirm_reply");
    evalstring(cmd2, "llm_confirm", SEVAL_NOHIST);

    return confirmed;
}
