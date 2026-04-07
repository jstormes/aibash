#ifndef LLMSH_SAFETY_H
#define LLMSH_SAFETY_H

/*
 * Prompt the user for confirmation.
 * Returns 1 if confirmed, 0 if denied.
 */
int safety_confirm(const char *action_description);

#endif /* LLMSH_SAFETY_H */
