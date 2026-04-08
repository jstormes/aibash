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

[local]
url = http://localhost:8080/v1/chat/completions
model = Qwen3-4B
EOF
```

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

The LLM can call tools (ls, cat, grep, run, man, etc.) and will iterate
until it produces a final text answer or the max iterations limit is reached.

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
llm_config --labels           # toggle [chat]/[stdout]/[tool] labels
llm_config --debug            # toggle debug mode (labels + API info)
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

## Recommended Models

For CPU-only local inference with tool calling support:

| Model | RAM | Notes |
|-------|-----|-------|
| Qwen3-1.7B | ~1.5GB | Fast, good tool calling |
| Qwen3-4B | ~3GB | Best balance of speed and quality |
| Qwen3-8B | ~5.5GB | Slower but more capable |

Use [llama.cpp](https://github.com/ggml-org/llama.cpp) `llama-server` for
the simplest local setup with OpenAI-compatible API and tool calling.

## Architecture

The LLM integration is implemented as:

- **`lib/llm/`** -- Self-contained library (`libllm.a`) with the API client,
  tool system, agentic loop, and supporting modules. Adapted from
  [llmsh](lib/llm/llmsh-upstream/README.md) (upstream source preserved for
  reference).
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
