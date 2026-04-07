#ifndef LLMSH_EXEC_H
#define LLMSH_EXEC_H

/*
 * Execute an external command pipeline.
 * pipeline: array of command strings, e.g. {"grep -r TODO src/", "wc -l"}
 * n_cmds:   number of commands in the pipeline
 * stdin_file:  redirect stdin from file (NULL for none)
 * stdout_file: redirect stdout to file (NULL for none)
 * append:      if true, append to stdout_file instead of truncate
 *
 * Returns malloced string of captured stdout, or NULL on error.
 */
char *exec_pipeline(const char **pipeline, int n_cmds,
                    const char *stdin_file, const char *stdout_file,
                    int append);

#endif /* LLMSH_EXEC_H */
