#ifndef LLMSH_PATHSCAN_H
#define LLMSH_PATHSCAN_H

/*
 * Build a hash table of all executables found in $PATH.
 * Call once at startup. Returns number of commands found.
 */
int pathscan_init(void);

/*
 * Free the hash table.
 */
void pathscan_cleanup(void);

/*
 * Check if a single word is a known command in PATH.
 * Returns 1 if found, 0 if not.
 */
int pathscan_lookup(const char *name);

/*
 * Tokenize user input and match each word against the PATH hash table.
 * Returns a malloced comma-separated string of matched commands.
 * Returns NULL if no matches.
 * Also sets *first_word_match to 1 if the very first word is a command
 * (strong signal that this is a direct shell command).
 */
char *pathscan_match_input(const char *input, int *first_word_match);

#endif /* LLMSH_PATHSCAN_H */
