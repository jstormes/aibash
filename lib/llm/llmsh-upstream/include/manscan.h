#ifndef LLMSH_MANSCAN_H
#define LLMSH_MANSCAN_H

/*
 * Initialize man page index from whatis database.
 * Parses whatis output into a hash table for O(1) lookup.
 * Returns number of entries indexed.
 */
int manscan_init(void);

/* Free all memory. */
void manscan_cleanup(void);

/*
 * Tier 1: Look up the one-line whatis summary for a command.
 * Returns a malloced string like "list directory contents", or NULL.
 */
char *manscan_whatis(const char *cmd);

/*
 * Tier 2: Fetch detailed man page sections (NAME + SYNOPSIS + OPTIONS).
 * Runs man via popen, extracts relevant sections, strips formatting,
 * truncates to max_bytes. Returns malloced string, or NULL on failure.
 */
char *manscan_detail(const char *cmd, int max_bytes);

/*
 * Build a combined whatis context for all commands in a pipeline.
 * Extracts the first word of each command, looks up whatis.
 * Returns malloced string like "ls: list directory contents\ngrep: ...\n"
 * or NULL if no matches.
 */
char *manscan_enrich_pipeline(const char **cmds, int n);

/* Return the total number of indexed entries. */
int manscan_count(void);

#endif /* LLMSH_MANSCAN_H */
