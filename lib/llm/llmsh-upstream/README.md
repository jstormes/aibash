# llmsh - Natural Language Shell

A minimal C shell that uses an LLM to translate natural language into Unix operations.
Supports both plain English and standard shell commands with SSE streaming output.

## Architecture

```
User Input → PATH Match → LLM (classify + tool calls) → Builtins / External Commands → Output
                                    ↓                                    ↓
                              fd 3 (stdchat)                      fd 1 (stdout)
                           LLM text → terminal                tool output → hidden
```

1. **PATH scanning**: On startup, all executables in `$PATH` are indexed into a hash table.
2. **Per-input matching**: Each word in the user's input is checked against the hash table. Matched commands are sent to the LLM as hints.
3. **Command classification**: The LLM determines if the input is a direct shell command, natural language, or ambiguous (in which case it asks for clarification).
4. **Execution**: Commands run through built-in tools or external execution with safety checks.
5. **Streaming**: LLM responses stream token-by-token to the terminal via SSE.

## Building

```bash
# Requires libcurl-dev and libreadline-dev
sudo apt install libcurl4-openssl-dev libreadline-dev   # Debian/Ubuntu

make
```

## Usage

### Interactive Mode

```bash
./llmsh            # tool output visible by default
./llmsh -q         # quiet: hide tool output
```

Both natural language and standard shell commands work:

```
default@/home/user> show me the largest files in this directory
default@/home/user> find all TODO comments in the source code
default@/home/user> ls -la
default@/home/user> grep -r TODO src/ | wc -l
default@/home/user> gcc -o foo foo.c
```

### Direct Execution

When the first word of your input is a known command in `$PATH`, it runs
directly without calling the LLM — just like a regular shell. This makes
`ls`, `grep`, `git`, etc. instant.

### Hybrid Pipelines

The `|` pipe bridges the shell and the LLM. Command segments run directly;
when a segment's first word is NOT a known command, it becomes a natural
language prompt with the previous command output as context.

```
default@~> ls | show me the markdown files
default@~> git log --oneline | summarize the recent changes
default@~> cat main.c | grep TODO | explain these TODOs
default@~> ps aux | which process is using the most memory
default@~> df -h | am I running low on disk space
```

The rule: split on `|`, run commands left-to-right until a non-command
segment is reached, then hand the accumulated output to the LLM as context.

### One-shot Mode

Pass a question as arguments — llmsh answers and exits:

```bash
./llmsh "what time is it"
./llmsh "how much disk space is free"
./llmsh -v "list all C files in src/"    # show tool output too
```

### Stdin Piping

Pipe data into llmsh as context for the LLM:

```bash
cat error.log | ./llmsh "summarize the errors"
git diff | ./llmsh "review this change"
ps aux | ./llmsh "which process is using the most memory"
```

## 4-Stream I/O Model

llmsh extends the Unix stream model with a fourth file descriptor for LLM output:

| fd | Name | Default | Purpose |
|----|------|---------|---------|
| 0 | stdin | terminal | User input / piped data as LLM context |
| 1 | stdout | terminal | Tool execution output (ls, cat, grep results) |
| 2 | stderr | terminal | Errors and safety confirmation prompts |
| 3 | stdchat | terminal | LLM conversational text (answers, summaries) |

**Tool output is visible by default**, just like a regular shell. Use `-q` to
hide intermediate tool output for cleaner LLM-only responses (e.g., code review).

### Controlling Output

**CLI flags:**

```bash
./llmsh            # tool output visible (default)
./llmsh -q         # quiet: hide tool output, show only LLM answers
./llmsh -v         # verbose: same as default (explicit)
```

**Runtime toggles:**

```
default@~> /verbose    # toggle tool output on/off
default@~> /labels     # toggle stream labels on/off
default@~> /debug      # toggle debug mode (labels + API info)
```

**Redirecting streams:**

```bash
# Save LLM answer to a file
./llmsh "review the code" 3>review.txt

# Pipe LLM answer through less
./llmsh "explain this codebase" 3>&1 | less

# Show only LLM answer, suppress tool output (default behavior)
./llmsh "summarize src/"

# Show everything: tool output + LLM answer
./llmsh -v "list files"

# Save tool output, LLM answer to terminal
./llmsh -v "find large files" 1>tools.log
```

### How It Works

When the user asks "review the code in this directory":

1. LLM calls `ls` to list files → output captured, sent back to LLM (hidden from terminal)
2. LLM calls `cat` on each file → contents captured, sent back to LLM (hidden)
3. LLM streams its review → text appears token-by-token on stdchat (fd 3)

The tool results are always sent to the LLM for context regardless of whether
they're displayed. Use `-v` or `/verbose` when you want to see what tools are doing.

## Stream Labels

Use `-l` or `/labels` to see labeled, color-coded output showing exactly what
each stream is doing:

```
default@~> /labels
Labels: on
default@~> review the code
[tool] ls({"path":"."})
[stdout] src/main.c  src/llm.c  src/router.c
[tool] cat({"path":"src/main.c"})
[stdout] #include <stdio.h>...
[think] Let me analyze the code structure and patterns...
[chat] The code is well structured. Here's my analysis...
```

| Label | Color | Description |
|-------|-------|-------------|
| `[chat]` | Green | LLM response text |
| `[stdout]` | Cyan | Tool execution output |
| `[think]` | Magenta | LLM reasoning/thinking tokens |
| `[tool]` | Yellow | Tool call name and arguments (before execution) |
| `[man]` | Bright magenta | Man page RAG lookups (automatic + on-demand) |
| `[api]` | Gray | API request/response info (debug mode only) |

**Debug mode** (`-d` or `/debug`) adds API-level information:

```
[api→] http://ai:8080/v1/chat/completions
[tool] run({"pipeline":["grep -rn TODO src/"]})
[man] grep: print lines that match patterns
[stdout] src/main.c:42: // TODO: implement
[chat] Found 1 TODO comment...
[api←] text=142B, tool_calls=0
```

Thinking tokens are detected from multiple API formats:
`reasoning_content` (OpenAI), `thinking` (Anthropic/llama.cpp), `reasoning`.

## Man Page RAG

On startup, llmsh indexes all man page summaries (~3000 entries) into a hash
table using the `whatis` database. This gives the LLM accurate knowledge of
available commands and their purposes.

### Tier 1: Automatic Enrichment

Every time the LLM uses the `run` tool to execute a command, the one-line man
page summary for each command in the pipeline is automatically injected into the
result. The LLM sees this context and can use it to refine subsequent commands.

Visible in label mode as `[man]`:
```
[man] grep: print lines that match patterns
[man] wc: print newline, word, and byte counts
```

### Tier 2: On-demand Detail

The LLM can explicitly call the `man` tool to get detailed documentation
(NAME, SYNOPSIS, OPTIONS sections) when it needs exact flags or usage info:

```
default@~> what flags does tar use for gzip compression?
[tool] man({"command":"tar"})
[man] NAME: tar - an archiving utility
      SYNOPSIS: tar [OPTION...] [FILE]...
      OPTIONS: -z, --gzip: filter the archive through gzip
               -c, --create: create a new archive
               ...
[chat] Use tar -czf archive.tar.gz files/ for gzip compression.
```

### Configuration

```ini
[settings]
man_enrich = 1         # 0=off, 1=auto whatis with run (default)
man_max_bytes = 4096   # max bytes for detail lookups
```

## SSE Streaming

LLM responses are streamed token-by-token using Server-Sent Events (SSE),
the standard streaming format for OpenAI-compatible APIs. Text appears
on the terminal as it's generated rather than waiting for the full response.

Tool calls are also handled during streaming — the SSE parser accumulates
tool call fragments (name and arguments arrive in chunks) and assembles
them into complete tool calls for execution.

## Safety Model

llmsh uses a three-tier safety system. Read-only operations run immediately; write and destructive operations require confirmation.

### Built-in Tools

| Tier | Behavior | Tools |
|------|----------|-------|
| **Auto** | Runs immediately | ls, cat, head, wc, grep, pwd, cd, read_file |
| **Confirm** | Asks before running | cp, mv, mkdir, write_file |
| **Danger** | Explicit confirmation | rm |

### External Commands (via `run`)

External commands executed through the `run` tool are classified automatically:

**Safe commands (no confirmation needed):**

These run immediately when used in a pipeline with no output file redirection:

- **File inspection**: ls, cat, head, tail, less, more, file, stat, tree, wc, du, df
- **Search**: find, locate, grep, egrep, fgrep, rg, which, whereis
- **Text processing**: sort, uniq, tr, cut, paste, awk, sed, diff, cmp, comm
- **System info**: uname, hostname, uptime, free, date, cal, whoami, id, ps, top
- **Networking** (read-only): ping, host, dig, nslookup, ip, ifconfig, ss, curl, wget
- **Development**: git, make, cmake, gcc, g++, clang, python, node, go, rustc, cargo, java, javac, npm, pip
- **Binary inspection**: ldd, nm, objdump, readelf, strings, xxd, od, hexdump
- **Archives** (read/extract): tar, gzip, gunzip, zcat, bzip2, xz, zip, unzip
- **Package queries**: dpkg, apt, rpm
- **Containers**: docker, kubectl
- **Checksums**: md5sum, sha256sum, sha1sum

**Confirmation required when:**

- Any command in the pipeline is **not** on the safe list
- The pipeline includes **output file redirection** (`>` or `>>`)

Examples:

```
default@~> ls -la                          # safe, runs immediately
default@~> grep -r TODO src/ | wc -l       # all safe, runs immediately
default@~> gcc -o foo foo.c                # safe, runs immediately
default@~> rm -rf /tmp/junk                # NOT safe, asks for confirmation
default@~> curl example.com > out.txt      # file redirect, asks for confirmation
```

## Agentic Loop

llmsh uses an agentic loop for multi-step tasks. When the LLM needs to make
multiple tool calls to complete a request (e.g., "review the code in this directory"),
it will keep calling tools and feeding results back until it has a final answer.

The loop stops when:
- The LLM responds with only text (no more tool calls)
- Ctrl-C interrupts
- The max iterations limit is reached (default: 20)

## Configuration

All configuration lives in `~/.llmshrc` (INI format).

### Global Settings

The `[settings]` section controls shell behavior:

```ini
[settings]
max_iterations = 20    # max tool-call rounds per user input (default: 20)
```

### Server Definitions

Each other `[section]` defines a named LLM server:

```ini
[local]
url = http://ai:8080/v1/chat/completions
model = llama3

[openai]
url = https://api.openai.com/v1/chat/completions
model = gpt-4o
key = sk-your-key-here
```

Switch at runtime:

```
default@~> /server           # list all servers
default@~> /server openai    # switch to openai
```

Falls back to environment variables if no `~/.llmshrc` exists.

## Shell Commands

| Command | Description |
|---------|-------------|
| `/server` | List configured LLM servers |
| `/server <name>` | Switch to a different server |
| `/clear` | Clear conversation history |
| `/verbose` | Toggle tool output visibility |
| `/labels` | Toggle stream labels on/off |
| `/debug` | Toggle debug mode (labels + API info) |
| `help` | Show usage help |
| `exit`, `quit` | Exit the shell |

**Keyboard shortcut:** Press `Shift+Tab` to cycle through configured LLM servers
without typing a command. The active server and model are shown in the prompt.

## CLI Flags

| Flag | Description |
|------|-------------|
| `-v` | Verbose: show tool output on stdout (default) |
| `-q` | Quiet: hide tool output, show only LLM answers |
| `-l` | Labels: prefix output with `[chat]`, `[stdout]`, `[think]`, `[tool]` |
| `-d` | Debug: labels + `[api]` request/response info |
| `-h` | Show help and exit |

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `LLMSH_API_URL` | `http://localhost:8080/v1/chat/completions` | LLM API endpoint (fallback if no ~/.llmshrc) |
| `LLMSH_MODEL` | `default` | Model name |
| `LLMSH_API_KEY` | (none) | Bearer token for API auth |

## Files

| Path | Description |
|------|-------------|
| `~/.llmshrc` | Server and settings configuration (INI format) |
| `~/.llmsh_history` | Persistent command history (readline) |

## Dependencies

- **libcurl** - HTTP client for LLM API (with SSE streaming)
- **libreadline** - Line editing with up/down arrow history
- **cJSON** - JSON parsing (vendored, no install needed)

## License

MIT
