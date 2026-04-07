#ifndef BASH_LLM_SAFETY_H
#define BASH_LLM_SAFETY_H

/*
 * Prompt the user for confirmation.
 * Returns 1 if confirmed, 0 if denied.
 */
int llm_safety_confirm(const char *action_description);

#endif /* BASH_LLM_SAFETY_H */
