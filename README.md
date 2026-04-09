# aibash - GNU Bash with Agentic LLM Integration

> **WARNING: EXPERIMENTAL SOFTWARE - NOT FOR PRODUCTION USE**
>
> This is a modified version of GNU Bash that contains an **autonomous AI agent**
> with the ability to execute arbitrary shell commands on your system. The LLM
> can read files, write files, delete files, run pipelines, and perform any
> operation available to your user account.
>
> **By using this software, you accept the following risks:**
>
> - The AI may execute commands you did not explicitly request
> - The AI may misinterpret natural language and perform destructive actions
> - The AI sends your queries and file contents to an external LLM API server
> - The safety confirmation system is a convenience, not a security boundary
> - There is no sandboxing -- the AI operates with your full user permissions
> - AI responses may be incorrect, misleading, or harmful
>
> **DO NOT use this shell for:**
> - Production systems or servers
> - Systems containing sensitive data
> - Environments where unintended command execution could cause harm
> - Any system where security is a concern
>
> This project is a research experiment in human-AI shell interaction.
> Use it on isolated development machines at your own risk.

---

## What is aibash?

aibash is a fork of GNU Bash 5.3 with built-in LLM (Large Language Model)
support. You can type natural language alongside normal shell commands, and
an AI agent will translate your intent into tool calls, execute them, and
iterate until it has an answer.

```
$ show me the largest files in this directory
  → run du -sh * | sort -rh | head -10
The largest files are:
  45M  execute_cmd.o
  38M  subst.o
  ...

$ llm find all TODO comments in the C source files
  → run grep -rn TODO *.c
  → run wc -l
Found 23 TODO comments across 12 files...
```

Normal shell commands work exactly as before -- `ls`, `grep`, `git`, etc.
all execute directly without touching the LLM. The AI only activates when
you explicitly use the `llm` builtin or (optionally) when a command is not
found.

## Building

Requires `libcurl-dev` and `libreadline-dev`:

```bash
# Debian/Ubuntu
sudo apt install libcurl4-openssl-dev libreadline-dev

# Build
./configure
make

# Install to /usr/local/bin/aibash
sudo make install
```

`configure` will fail with an error if `libcurl` is not installed, since it
is required for the LLM integration.

To install to a different location, use `--prefix`:

```bash
./configure --prefix=/opt/aibash
make
sudo make install    # installs to /opt/aibash/bin/aibash
```

## Quick Start

1. Run an OpenAI-compatible LLM server (e.g., [llama.cpp](https://github.com/ggml-org/llama.cpp)):

```bash
# Download and run a model
./llama-server -m Qwen3-4B-Q4_K_M.gguf --port 8080
```

2. Configure aibash:

```bash
cat > ~/.bashllmrc << 'EOF'
[settings]
command_not_found = 1

[memory]
url = http://localhost:8081/v1/chat/completions
model = Qwen3.5-4B

[local]
url = http://localhost:8080/v1/chat/completions
model = Qwen3-4B
EOF
```

The `[memory]` section is optional -- without it, automatic memory
extraction is disabled but explicit commands still work.

3. Launch:

```bash
./aibash
```

The LLM subsystem initializes automatically in interactive shells.

## Configuration

All configuration lives in `~/.bashllmrc` (INI format):

```ini
[settings]
max_iterations = 20       # max tool-call rounds per query (default: 20)
man_enrich = 1            # auto-inject man summaries with commands (default: 1)
command_not_found = 0     # route unknown commands to LLM (default: 0)
memory = 1                # enable long-term memory (default: 1)
memory_max = 200          # max memory entries (default: 200)

[local]
url = http://localhost:8080/v1/chat/completions
model = Qwen3-4B

[openai]
url = https://api.openai.com/v1/chat/completions
model = gpt-4o
key = sk-your-key-here

[anthropic]
url = https://api.anthropic.com/v1/messages
model = claude-sonnet-4-20250514
key = sk-ant-your-key-here
```

If no config file exists, falls back to environment variables:
- `BASH_LLM_API_URL` (default: `http://localhost:8080/v1/chat/completions`)
- `BASH_LLM_MODEL` (default: `default`)
- `BASH_LLM_API_KEY` (optional)

## Builtins

### `llm [-c] [query ...]`

Send a query to the LLM and run the agentic tool loop.

```bash
llm what files are in /tmp
llm -c                        # clear conversation history
llm find all .c files over 10k
```

The LLM can call tools (ls, cat, grep, run, man, memory, etc.) and will
iterate until it produces a final text answer or the max iterations limit
is reached.

**Memory subcommands:**

```bash
llm remember I prefer Python over Node    # save a fact to long-term memory
llm remember this project uses PostgreSQL  # save project context
llm memories                               # list all saved memories
llm forget 3                               # delete memory by ID
llm forget Python                          # delete by content match
```

These commands manage memory directly without calling the LLM. The LLM
can also use memory tools (`memory_save`, `memory_search`, `memory_forget`)
during the agentic loop.

### `llm_init [-n]`

Initialize the LLM subsystem. Runs automatically at startup in interactive
shells.

- `-n` -- do not install `command_not_found_handle`

The `command_not_found_handle` is installed when ALL of:
- The `-n` flag is NOT passed
- `command_not_found = 1` in config (default: 0)
- The shell is interactive

When installed, unrecognized commands are sent to the LLM as natural
language queries.

### `llm_config [options]`

Configure the LLM subsystem at runtime.

```bash
llm_config                    # show current config
llm_config --list             # list all servers (* marks active)
llm_config --switch openai    # switch to a different server
llm_config --verbose          # toggle tool output visibility
llm_config --labels           # toggle [chat]/[stdout]/[tool]/[mem] labels
llm_config --debug            # toggle debug mode (labels + API info)
llm_config --memories         # show memory stats
```

### Shift-Tab

Press Shift-Tab at the prompt to cycle through configured LLM servers.

## Agentic Loop

When the LLM needs to perform actions, it enters an agentic loop:

```
User query
  → LLM decides which tools to call
  → Tools execute, output captured
  → Results sent back to LLM
  → LLM calls more tools or produces final answer
  → Repeat (max 20 iterations)
```

### Available Tools

| Tool | Safety | Description |
|------|--------|-------------|
| ls | Auto | List directory contents |
| cat | Auto | Display file contents |
| read_file | Auto | Read file with line range |
| pwd | Auto | Print working directory |
| cd | Auto | Change directory |
| head | Auto | Show first N lines |
| wc | Auto | Count lines/words/chars |
| grep | Auto | Search file contents |
| man | Auto | Get man page info |
| cp | Confirm | Copy files |
| mv | Confirm | Move/rename files |
| mkdir | Confirm | Create directory |
| write_file | Confirm | Write content to file |
| rm | Danger | Remove files |
| run | Auto/Confirm | Execute arbitrary pipeline |
| memory_save | Auto | Save a fact/preference to long-term memory |
| memory_search | Auto | Search memories by keyword |
| memory_list | Auto | List all saved memories |
| memory_forget | Auto | Delete a memory by ID or content |

### Safety Tiers

- **Auto**: Read-only operations run immediately
- **Confirm**: Write operations prompt for confirmation
- **Danger**: Destructive operations prompt for confirmation

The `run` tool (arbitrary shell pipelines) auto-classifies based on the
commands in the pipeline. Read-only commands (grep, find, git, etc.) run
immediately. Unknown commands or file redirections require confirmation.

## Man Page RAG

On startup, aibash indexes all man page summaries (~3000 entries) from the
`whatis` database. When the LLM executes commands via the `run` tool,
one-line man summaries are automatically injected into the results, giving
the LLM accurate knowledge of available commands. The LLM can also call
the `man` tool for detailed flag and option documentation.

## Long-Term Memory

aibash includes a persistent long-term memory system that lets the LLM
remember facts, preferences, and project context across sessions.

### How It Works

Memories are stored as JSON in `~/.aibash_memories/memories.json`. Before
each query, aibash searches memories for keywords matching your input and
"whispers" relevant context into the LLM's system prompt. The LLM sees
this context but the user doesn't (unless debug/label mode is on).

```
$ llm remember my name is James
Remembered: my name is James

$ llm I prefer Python for scripts, use neovim, and deploy to AWS
# → main agent responds, then background memory agent auto-extracts:
#   [3] User prefers Python for scripts
#   [4] User uses neovim as editor
#   [5] User deploys to AWS

# Later, even in a new session:
$ llm what do you know about me
You are James, a DevOps engineer who prefers Python for scripting
and deploys to AWS.

$ llm actually I switched to helix editor
# → memory agent replaces neovim entry with helix

$ llm please forget everything about my editor
# → main agent searches memories, deletes editor entries
Done — editor-related memories deleted.
```

### Memory Agent (Background)

After each conversation, a **background memory agent** automatically
extracts facts worth remembering. It uses a two-pass approach:

**Pass 1 -- Fast extraction (thinking off):** Quickly scans the
conversation and saves any new facts. Takes 2-3 seconds.

**Pass 2 -- Cleanup (thinking on):** Reviews all memories and:
- Splits compound entries into individual facts
- Resolves conflicts (replaces outdated memories)
- Removes duplicates

The thinking mode lets the model reason through complex cleanup
logic. Both passes run in a forked background process -- the user
is already back at their prompt and never waits.

The memory agent requires its own LLM server, configured via the
`[memory]` section in `~/.bashllmrc`. A small, fast model like
Qwen3.5-4B works well. An instruct-only or thinking-capable model
is recommended.

### Memory Commands

| Command | Description |
|---------|-------------|
| `llm remember <text>` | Save a fact directly (no LLM call) |
| `llm memories` | List all saved memories with IDs |
| `llm forget <id>` | Delete a memory by ID |
| `llm forget <text>` | Delete memories matching text |
| `llm please forget <topic>` | Natural language forget (LLM searches and deletes) |
| `llm_config --memories` | Show memory stats |

### LLM Memory Tools

The main LLM agent can also manage memories during the agentic loop:

- **`memory_save`** -- Save facts when the user shares preferences
- **`memory_search`** -- Search memories for context from previous sessions
- **`memory_forget`** -- Delete memories by ID (uses search→forget workflow)
- **`memory_list`** -- List all saved memories

### Whisper Injection (Three Layers)

Before each query, relevant memories are injected into the system prompt.
The whisper system has three layers:

**Layer 1 -- Keyword search with stemming (<1ms):** Fast substring
matching plus lightweight English stemming. "deployment" matches
"deploy", "containers" matches "container", etc. Handles most queries
instantly including word form variations.

**Layer 2 -- Parallel whisper agents (2-5s, fallback):** When keyword
search finds nothing, two LLM-powered agents fork in parallel and
semantically search all memories for relevant context:
- Agent 1 searches for user preferences and personal context
- Agent 2 searches for project and technical context

Both call the memory agent LLM simultaneously. Results are merged with
a 5-second timeout -- if agents don't respond in time, the query
proceeds without whispered context.

**Layer 3 -- Background extraction (post-response):** After each
conversation, a background process extracts new facts (see Memory Agent
section above).

In label/debug mode, you can see which layer was used:

```
$ llm_config --labels
Labels: on

$ llm what database do I use
[mem] - The main project database is PostgreSQL 16     ← Layer 1: keyword hit

$ llm describe my infrastructure setup
[mem-whisper] searching (2 agents)...                  ← Layer 2: fallback
[mem-whisper] - User deploys to AWS us-east-1
[mem-whisper] - The main project database is PostgreSQL 16
[mem-whisper] - The CI pipeline uses GitHub Actions

[mem-agent] extracted: User asked about infrastructure  ← Layer 3: background
```

### Configuration

```ini
[settings]
memory = 1          # 0=off, 1=on (default: 1)
memory_max = 200    # max entries before oldest are evicted (default: 200)

[memory]
url = http://ai3:8080/v1/chat/completions
model = Qwen3.5-4B
# key = optional-api-key
```

The memory agent is active when `memory = 1` AND a `[memory]` section
with a `url` is present. Without the `[memory]` section, explicit
commands (`llm remember`, `llm forget`) and the main agent's memory
tools still work -- only automatic extraction and whisper agents are
disabled.

For details on setting up the memory agent server, model selection,
and tuning, see [doc/MEMORY.md](doc/MEMORY.md).

### Storage

Memories are stored in `~/.aibash_memories/memories.json` as a JSON array.
Each entry has an ID, content, auto-generated keywords, and a timestamp.
The rolling window evicts the oldest entries when `memory_max` is reached.

## Recommended Models

For CPU-only local inference with tool calling support:

**Main agent** (quality matters, user is waiting):

| Model | RAM | Notes |
|-------|-----|-------|
| Qwen3-4B | ~3GB | Good for simple tasks |
| Qwen3.5-9B | ~6.5GB | Best quality under 10B |
| Qwen3.5-122B (MoE) | ~80GB | Top quality, needs big server |

**Memory agent** (speed matters, runs in background):

| Model | RAM | Notes |
|-------|-----|-------|
| Qwen3.5-4B Q6 | ~3.5GB | Recommended -- good extraction with thinking |
| Qwen3-4B-Instruct Q8 | ~4GB | Alternative -- instruct-only, no thinking overhead |

The memory agent benefits from thinking-capable models (Qwen3.5) for the
cleanup pass. Instruct-only models (Qwen3-Instruct) work for extraction
but produce lower quality conflict resolution.

Use [llama.cpp](https://github.com/ggml-org/llama.cpp) `llama-server` for
the simplest local setup with OpenAI-compatible API and tool calling.

## Architecture

The LLM integration is implemented as:

- **`lib/llm/`** -- Self-contained library (`libllm.a`) with the API client,
  tool system, agentic loop, memory store, and memory agent. Adapted from
  [llmsh](lib/llm/llmsh-upstream/README.md) (upstream source preserved for
  reference).
  - `llm_api.c` -- Main agent: curl/SSE streaming, tool call parsing
  - `llm_memory.c` -- Memory store: save, search, whisper, forget
  - `llm_mem_api.c` -- Memory agent API: separate curl client with own connection
  - `llm_mem_agent.c` -- Two-pass extraction: fast extract + thinking cleanup
  - `llm_shell.c` -- Agentic loop + background fork for memory agent
- **`builtins/llm.def`**, **`llm_init.def`**, **`llm_config.def`** -- Thin
  bash builtin wrappers that bridge bash's `WORD_LIST` interface to the
  library's string-based API.
- **`bashline.c`** -- Shift-Tab keybinding for server cycling.
- **`shell.c`** -- Auto-initialization hook for interactive shells.

No changes to bash's core execution engine (`execute_cmd.c`, `eval.c`, etc.).
The `command_not_found_handle` hook uses bash's existing shell function
mechanism.

### Dependencies

| Library | Purpose | Required |
|---------|---------|----------|
| **libcurl** | HTTP client for LLM API (SSE streaming) | Yes |
| **libreadline** | Line editing, history, Shift-Tab binding | Yes |
| **cJSON** | JSON parsing | Vendored (no install needed) |

Install development packages:

```bash
# Debian / Ubuntu
sudo apt install libcurl4-openssl-dev libreadline-dev

# Fedora / RHEL
sudo dnf install libcurl-devel readline-devel

# Arch
sudo pacman -S curl readline

# macOS (Homebrew)
brew install curl readline
```

`configure` will fail with a clear error if libcurl is missing.

---

## Original Bash README

### Introduction

This is GNU Bash, version 5.3. Bash is the GNU Project's Bourne
Again SHell, a complete implementation of the POSIX shell spec,
but also with interactive command line editing, job control on
architectures that support it, csh-like features such as history
substitution and brace expansion, and a slew of other features.
For more information on the features of Bash that are new to this
type of shell, see the file `doc/bashref.info`. There is also a
large Unix-style man page. If the info file and the man page conflict,
the man page is the definitive description of the shell's features.

See the file POSIX for a discussion of how the Bash defaults differ
from the POSIX spec and a description of the Bash `posix mode`.

There are some user-visible incompatibilities between this version
of Bash and previous widely-distributed versions, bash-5.0, bash-5.1,
and bash-5.2. The COMPAT file has the details. The NEWS file tersely
lists features that are new in this release.

Bash is free software, distributed under the terms of the [GNU] General
Public License as published by the Free Software Foundation,
version 3 of the License (or any later version). For more information,
see the file COPYING.

A number of frequently-asked questions are answered in the file
`doc/FAQ`. (That file is no longer updated.)

To compile Bash, type `./configure`, then `make`. Bash auto-configures
the build process, so no further intervention should be necessary. Bash
builds with `gcc` by default if it is available. If you want to use `cc`
instead, type

    CC=cc ./configure

if you are using a Bourne-style shell.

Read the file INSTALL in this directory for more information about how
to customize and control the build process, including how to build in a
directory different from the source directory. The file NOTES contains
platform-specific installation and configuration information.

### Reporting Bugs

Bug reports for the LLM integration should be sent to:
https://github.com/jstormes/aibash/issues

Bug reports for bash itself should be sent to: bug-bash@gnu.org

### License

GNU General Public License v3.0 -- see COPYING for details.

Chet Ramey
chet.ramey@case.edu
