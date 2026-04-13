#ifndef AGENTS_SETUP_H
#define AGENTS_SETUP_H

/*
 * Register all built-in side agents and initialize them.
 * Call once at startup from the llm builtin.
 * config: opaque pointer passed to each agent's init().
 */
void agents_setup(void *config);

#endif /* AGENTS_SETUP_H */
