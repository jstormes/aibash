# aibash Memory System

The aibash memory system gives the LLM persistent, long-term memory across
sessions. It stores facts, preferences, and project context that the LLM
can recall automatically.

## Architecture Overview

All memory access goes through the **global memory side agent**. The main
LLM agent and builtins never access the memory store directly -- the agent
is the sole owner of memory data.

```
User query
  |
  |-- Side Agent Framework (serial fork/pipe)
  |     |
  |     |-- Memory Agent PRE-QUERY:
  |     |     LLM searches memories for relevant facts
  |     |     Injects as ## global_memory section
  |     |
  |     |-- Cron Agent PRE-QUERY:
  |     |     LLM classifies: schedule-related? YES/NO
  |     |     If YES: injects ## cron section (English job list)
  |     |
  |-- ## sections injected into system prompt
  |
  |-- Main Agent responds (uses ## context, no direct memory access)
  |
  |-- Side Agent Framework (double-fork, background)
        |
        |-- Memory Agent POST-QUERY:
        |     Extracts NEW facts from user input only
        |     (does NOT see existing memories or injected context)
        |     Pass 1: fast extraction (thinking off)
        |     Pass 2: cleanup (thinking on)
        |
        |-- Cron Agent POST-QUERY:
              Detects scheduling requests, creates cron/at jobs
```

## Quick Setup

### 1. Main agent server (large model)

```bash
# On your main AI server
llama-server -m Qwen3.5-122B.gguf --port 8080
```

### 2. Memory agent server (small, fast model)

```bash
# On a secondary machine or same machine on a different port
llama-server -m Qwen3.5-4B-UD-Q6_K_XL.gguf \
  --port 8080 \
  -ngl 999 \
  -c 131072 \
  --temp 0.7 \
  -np 1 \
  -fa on \
  --cache-type-k q8_0 \
  --cache-type-v q8_0
```

Key flags:
- `-np 1` -- one parallel slot (sufficient for side agent)
- `-c 131072` -- large context for cleanup pass with many memories
- `-ngl 999` -- offload to GPU if available

### 3. Configure aibash

```ini
# ~/.bashllmrc

[settings]
max_iterations = 20
man_enrich = 1
command_not_found = 1
memory = 1
memory_max = 200

[memory]
url = http://ai3:8080/v1/chat/completions
model = Qwen3.5-4B

[ai]
url = http://ai:8080/v1/chat/completions
model = Qwen3.5-122B
```

### 4. Launch aibash

```bash
./aibash
LLM initialized: ai (Qwen3.5-122B), 2762 man pages, 10 memories
```

## How Memory Works

### Separation of Concerns

The memory system enforces a strict boundary:

- **Memory agent** (`agents/mem_agent.c`): Sole owner of the memory store.
  Handles all reads, writes, searches, and cleanup. Dependencies injected
  for testability.
- **Side agent framework** (`agents/side_agent.c`): Manages fork/pipe/select
  plumbing for pre-query and post-query callbacks.
- **Main agent** (`llm_api.c`): Only receives injected `## global_memory`
  sections. Has no memory tools and cannot access memory data.
- **Builtins** (`llm.def`): User commands (`remember`, `forget`, `memories`)
  call the agent's public API, never the storage layer directly.

### Saving Memories

**1. Explicit command** (instant, through agent API):
```bash
llm remember I prefer Python for scripting
llm remember This project uses PostgreSQL 16 on port 5432
```

**2. Background extraction** (automatic, after each conversation):
The post-query side agent analyzes the conversation and extracts facts
worth remembering. This happens in a double-forked background process --
the user never waits.

### Recalling Memories

**Automatic context injection:** Before each query, the pre-query side
agent searches memories via LLM and injects relevant ones into the system
prompt as a `## global_memory` section.

**User commands:**
```bash
llm memories               # list all saved memories
llm cleanup                # manually run cleanup (split, deduplicate, resolve)
llm_config --memories      # show memory stats
```

### Forgetting Memories

```bash
# By ID
llm forget 5

# By keyword (case-insensitive substring match)
llm forget python
```

### Conflict Resolution

When new information contradicts existing memories, the background
extraction agent detects conflicts during the cleanup pass (thinking
mode ON) and replaces outdated entries automatically.

### Tombstone Mechanism

When a memory is deleted, a tombstone marker is saved:

```
DELETED: User wants to forget: I use helix as my editor
```

Tombstones solve a race condition: the background agent runs in a forked
process and may re-save a fact that the user already deleted. When the
cleanup pass runs, it sees the tombstone, removes conflicting re-saved
memories, and removes the tombstone itself.

## Pre-Query Search

The pre-query callback forks a child process that:
1. Sends all memories + the user's query to the memory LLM
2. The LLM returns only memories that DIRECTLY relate to the query
3. Results are piped back to the parent (5-second timeout)
4. Injected into the system prompt as `[global_memory context]`

This produces precise, context-efficient results:

| Query | Injected | Not injected |
|-------|----------|--------------|
| "what database do I use" | MySQL only | Docker, Python, AWS, family |
| "tell me about my family" | wife, kids, name | ice cream, tech stack |
| "what's the weather" | NONE | everything |

In debug mode:
```
$ llm_config --labels
$ llm what database do I use
[global-mem] searching memories...
[global-mem] - The project database is MySQL
```

## Post-Query Extraction

The post-query callback double-forks a grandchild that:

**Pass 1 (thinking off):** Quick extraction of obvious facts.
Temperature 0.1 for deterministic output.

**Pass 2 (thinking on):** Reviews all memories with reasoning:
- Splits compound entries into individual facts
- Detects and resolves conflicts
- Removes duplicates

Both passes run in the background -- the user is already back at their prompt.

## Side Agent Framework

The memory system uses the generic side agent framework. Each side agent
registers callbacks:

```c
side_agent_register(&(side_agent_t){
    .name        = "global_memory",
    .timeout_sec = 5,
    .enabled     = 1,
    .pre_query   = global_mem_agent_pre_query_cb,
    .post_query  = global_mem_agent_post_query_cb,
});
```

The framework handles:
- Parallel fork/pipe/select for pre-query agents
- Double-fork for post-query agents
- Timeout management and child reaping
- Result wrapping in `[name context]...[end name context]` blocks

New side agents (e.g., local directory memory, cron reminders) can be
added by writing callbacks and registering them.

## Model Selection

### Memory agent model requirements

- **Instruction following** -- must output valid JSON arrays reliably
- **Thinking capability** -- for the cleanup pass (Pass 2)
- **Speed** -- extraction happens after every query

### Recommended models

| Model | Quant | Size | Thinking | Notes |
|-------|-------|------|----------|-------|
| **Qwen3.5-4B** | Q6_K | 3.5GB | Yes | Recommended -- best balance |
| **Qwen3.5-4B** | Q8_0 | 4.5GB | Yes | Higher quality, slightly slower |
| Qwen3-4B-Instruct-2507 | Q8_0 | 4.0GB | No | Good extraction, weaker cleanup |

## Server Configuration

### Minimum hardware (memory agent server)

- CPU: 4+ cores (8 recommended)
- RAM: 8GB (for 4B model + context)
- GPU: Optional (iGPU helps)

### Context length vs memory capacity

| Context (-c) | Max memories in cleanup | RAM overhead |
|-------------|----------------------|-------------|
| 32768 | ~200 | Low |
| 65536 | ~500 | Medium |
| 131072 | ~1000 | High |
| 262144 | ~2000-3000 | Very high |

## Debug Mode

Enable labels to see memory agent activity:

```bash
llm_config --labels     # or --debug for API-level info
```

Labels:
- `[global-mem]` -- memory agent search and extraction activity

## Files

| Path | Description |
|------|-------------|
| `~/.bashllmrc` | Configuration (servers, settings, [memory] section) |
| `~/.aibash_memories/` | Memory storage directory |
| `~/.aibash_memories/memories.json` | All saved memories (JSON array) |
| `~/.aibash_memories/logs/` | API call logs (auto-cleaned after 24h) |

### Source files

| File | Description |
|------|-------------|
| `lib/llm/agents/side_agent.c/.h` | Side agent framework (tested) |
| `lib/llm/agents/mem_agent.c/.h` | Memory agent (dependency-injected, tested) |
| `lib/llm/agents/cron_agent.c/.h` | Cron agent (classify + translate, tested) |
| `lib/llm/agents/agents_setup.c` | Agent registration and initialization |
| `lib/llm/agents/tests/` | 56 tests, 121 assertions |
| `lib/llm/llm_memory.c/.h` | Memory store (private to memory agent) |
| `lib/llm/llm_global_mem_api.c/.h` | Side agent LLM client |

## Troubleshooting

**Memory agent not extracting:**
- Check `[memory]` section in `~/.bashllmrc` has a valid `url`
- Verify the memory server is running: `curl http://ai3:8080/v1/models`
- Check `memory = 1` in `[settings]`

**Memory search timing out:**
- Memory server may be overloaded or slow
- Default timeout is 15 seconds
- Check `~/.aibash_memories/logs/` for `mem-search` and `cron-classify` logs

**Memories not splitting:**
- The cleanup pass (thinking ON) handles splitting
- Ensure the model supports thinking mode (Qwen3.5 does)
- Run `llm cleanup` to trigger manually

**Too many memories / slow cleanup:**
- Reduce `memory_max` in config
- Ensure server context (`-c`) is large enough
- The cleanup pass scales linearly with memory count
