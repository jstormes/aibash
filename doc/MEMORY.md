# aibash Memory System

The aibash memory system gives the LLM persistent, long-term memory across
sessions. It stores facts, preferences, and project context that the LLM
can recall automatically or on demand.

## Architecture Overview

```
┌─────────────────────────────────────────────────────┐
│                    User Query                        │
│                                                      │
│  ┌──────────────┐    ┌─────────────────────────┐    │
│  │ Layer 1:     │    │ Layer 2:                 │    │
│  │ Keyword      │───▶│ Parallel Whisper Agents  │    │
│  │ Search       │ if │ (2 LLM agents on ai3)   │    │
│  │ (<1ms)       │ no │ (2-5 sec fallback)       │    │
│  │              │hit │                          │    │
│  └──────┬───────┘    └────────────┬─────────────┘    │
│         │                         │                   │
│         └──────────┬──────────────┘                   │
│                    ▼                                  │
│         [memory context] injected                     │
│         into system prompt                            │
│                    │                                  │
│                    ▼                                  │
│         Main Agent (ai server)                        │
│         responds to user                              │
│                    │                                  │
│                    ▼                                  │
│  ┌─────────────────────────────────────┐             │
│  │ Layer 3: Background Memory Agent    │             │
│  │ (forked, user doesn't wait)         │             │
│  │                                     │             │
│  │ Pass 1: Extract facts (thinking off)│             │
│  │ Pass 2: Cleanup (thinking on)       │             │
│  │   - Split compound entries          │             │
│  │   - Resolve conflicts               │             │
│  │   - Remove duplicates               │             │
│  └─────────────────────────────────────┘             │
└─────────────────────────────────────────────────────┘
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
  -np 2 \
  -fa on \
  --cache-type-k q8_0 \
  --cache-type-v q8_0
```

Key flags:
- `-np 2` -- two parallel slots (needed for parallel whisper agents)
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
LLM initialized: ai (Qwen3.5-122B), 2762 man pages, 4 memories
```

## How Memory Works

### Saving Memories

Memories are saved three ways:

**1. Explicit command** (instant, no LLM):
```bash
llm remember I prefer Python for scripting
llm remember This project uses PostgreSQL 16 on port 5432
```

**2. Main agent tool call** (during conversation):
The main LLM has a `memory_save` tool and will proactively save facts
when the user shares important information.

**3. Background extraction** (automatic, after each conversation):
The memory agent analyzes the conversation and extracts facts worth
remembering. This happens in a forked background process -- the user
never waits.

### Recalling Memories

**Automatic whisper injection:** Before each query, memories are
searched and relevant ones are injected into the system prompt. The
LLM sees this context and can use it to give better answers.

**Explicit search:** The main LLM can call `memory_search` to look
up specific memories during the agentic loop.

**User commands:**
```bash
llm memories               # list all saved memories
llm_config --memories      # show memory stats
```

### Forgetting Memories

```bash
# By ID
llm forget 5

# By keyword (case-insensitive substring match)
llm forget python

# Natural language (main agent searches and deletes)
llm please forget everything about my editor preference
```

### Conflict Resolution

When new information contradicts existing memories:

1. The **background memory agent** detects conflicts during the cleanup
   pass (thinking mode ON) and replaces outdated entries
2. The **main agent** can use `memory_forget` + `memory_save` to update
   facts in real-time when the user corrects something

Example:
```
$ llm I use neovim as my editor
# → memory saved: "User uses neovim as editor"

$ llm actually I switched to helix
# → memory agent cleanup pass:
#   - Deletes "User uses neovim as editor"
#   - Saves "User uses helix as editor"
```

## Whisper Layers in Detail

### Layer 1: Keyword Search with Stemming

- **Speed:** <1ms
- **Method:** Exact substring match + lightweight English stemming
- **When:** Every query
- **Returns:** Top 10 matching memories (max ~2000 chars)

The search uses two matching strategies with weighted scoring:

**Exact match (2 points):** Case-insensitive substring search via
`strcasestr()`. "deploy" in query matches "I deploy to AWS".

**Stemmed match (1 point):** Strips common English suffixes to match
morphological variants. Only applied to words >= 5 characters.

Suffixes stripped: `-ation`, `-ment`, `-ness`, `-able`, `-ible`, `-ing`,
`-tion`, `-sion`, `-ous`, `-ive`, `-ize`, `-ful`, `-less`, `-ed`,
`-er`, `-ly`, `-al`, `-es`, `-s`

Examples:
- "deployment" → "deploy" matches "I deploy to AWS"
- "containers" → "contain" matches "uses Docker for containers"
- "databases" → "databas" matches "project database is MySQL"
- "scripting" → "script" matches "prefer Python for scripts"

This handles most queries including word form variations without
needing to call the LLM.

### Layer 2: Parallel Whisper Agents

- **Speed:** 2-5 seconds
- **Method:** Two LLM agents fork in parallel, each analyzing all memories
  against the query for semantic relevance
- **When:** Only when Layer 1 finds nothing (fallback)
- **Timeout:** 5 seconds -- if agents don't respond, query proceeds without
- **Slots required:** 2 (configure memory server with `-np 2`)

Agent 1 focuses on user preferences and personal context.
Agent 2 focuses on project and technical context.

This catches queries like "describe my infrastructure setup" where no
single keyword matches but multiple memories are semantically relevant.

### Layer 3: Background Extraction

- **Speed:** 5-30 seconds (user doesn't wait)
- **Method:** Double-fork background process, two-pass LLM analysis
- **When:** After every conversation exchange

**Pass 1 (thinking off):** Quick extraction of obvious facts.
Temperature 0.1 for deterministic output.

**Pass 2 (thinking on):** Reviews all memories with reasoning:
- Splits compound entries into individual facts
- Detects and resolves conflicts via `replaces`
- Removes duplicates
- Handles forget requests via `forget`

## Model Selection

### Memory agent model requirements

The memory agent model needs:
- **Tool-free operation** -- it just returns JSON text, no function calling
- **Instruction following** -- must output valid JSON arrays reliably
- **Thinking capability** -- for the cleanup pass (Pass 2)
- **Speed on CPU** -- extraction happens after every query

### Recommended models

| Model | Quant | Size | Thinking | Notes |
|-------|-------|------|----------|-------|
| **Qwen3.5-4B** | Q6_K | 3.5GB | Yes | Recommended -- best balance |
| **Qwen3.5-4B** | Q8_0 | 4.5GB | Yes | Higher quality, slightly slower |
| Qwen3-4B-Instruct-2507 | Q8_0 | 4.0GB | No | Good extraction, weaker cleanup |
| Qwen3.5-9B | Q6_K | 6.5GB | Yes | Better quality but slower |

**Qwen3.5-4B Q6 is recommended** because:
- Thinking mode enables the cleanup pass to reason through conflicts
- Small enough for CPU inference with reasonable speed
- 262K native context supports large memory stores
- Temperature 0.1 produces reliable JSON output

### Models NOT recommended

- **Qwen3.5-9B Q8** -- too large for most memory agent servers (9GB RAM)
- **Models without thinking** -- cleanup pass produces worse results
- **Models >9B** -- diminishing returns for extraction; speed matters more

## Server Configuration

### Minimum hardware (memory agent server)

- CPU: 4+ cores (8 recommended)
- RAM: 8GB (for 4B model + context)
- GPU: Optional (iGPU helps, discrete not needed)
- Disk: 10GB for model files

### llama-server flags explained

```bash
llama-server \
  -m model.gguf \
  --port 8080 \
  -np 2 \              # 2 parallel slots for whisper agents
  -c 131072 \          # context length (increase for more memories)
  -ngl 999 \           # GPU layers (all if GPU available)
  -fa on \             # flash attention (faster)
  --cache-type-k q8_0 \  # quantized KV cache (saves memory)
  --cache-type-v q8_0 \
  --temp 0.7 \         # server default (overridden per-request)
  -t 8 -tb 16          # threads (match your CPU cores)
```

### Context length vs memory capacity

| Context (-c) | Max memories in cleanup | RAM overhead |
|-------------|----------------------|-------------|
| 32768 | ~200 | Low |
| 65536 | ~500 | Medium |
| 131072 | ~1000 | High |
| 262144 | ~2000-3000 | Very high |

The cleanup pass sends all memories to the model. Increase `-c` to
support more memories, but this increases RAM usage for KV cache.

## Tuning

### Temperature

The memory agent sends `temperature: 0.1` in all API requests,
overriding the server default. This ensures deterministic, reliable
JSON output. The server's `--temp` flag only affects requests that
don't specify temperature.

### Thinking mode

- **Pass 1 (extraction):** Thinking OFF -- fast, direct JSON output
- **Pass 2 (cleanup):** Thinking ON -- model reasons through conflicts
- **Whisper agents:** Thinking OFF -- speed matters for user-facing latency

Thinking is controlled per-request via `chat_template_kwargs.enable_thinking`.
The server doesn't need any special configuration for this.

### Memory max

```ini
[settings]
memory_max = 200    # default
```

Increase if you have a large context length configured. The oldest
memories are evicted (FIFO) when the limit is reached.

## Debug Mode

Enable labels to see memory system activity:

```bash
llm_config --labels     # or --debug for API-level info
```

Labels:
- `[mem]` -- keyword search whisper results (Layer 1)
- `[mem-whisper]` -- parallel agent whisper results (Layer 2)
- `[mem-agent]` -- background extraction activity (Layer 3)

## Files

| Path | Description |
|------|-------------|
| `~/.bashllmrc` | Configuration (servers, settings, [memory] section) |
| `~/.aibash_memories/` | Memory storage directory |
| `~/.aibash_memories/memories.json` | All saved memories (JSON array) |
| `~/.aibash_llm_history` | Conversation history (separate from memories) |

### Memory entry format

```json
{
  "id": 1,
  "content": "User prefers Python for scripting",
  "keywords": "user,prefers,python,scripting",
  "created": "2026-04-09T00:30:00Z"
}
```

## Troubleshooting

**Memory agent not extracting:**
- Check `[memory]` section in `~/.bashllmrc` has a valid `url`
- Verify the memory server is running: `curl http://ai3:8080/v1/models`
- Check `memory = 1` in `[settings]`

**Whisper agents timing out:**
- Memory server may be overloaded or slow
- Check `-np 2` is set (need 2 parallel slots)
- Increase timeout by rebuilding (WHISPER_TIMEOUT_SEC in llm_whisper.c)

**Memories not splitting:**
- The cleanup pass (thinking ON) handles splitting
- Ensure the model supports thinking mode (Qwen3.5 does, Qwen3-Instruct does not)
- Check the server isn't running with `--chat-template-kwargs '{"enable_thinking":false}'`

**Conflicts not resolved:**
- The 4B model resolves conflicts well but not perfectly
- Manual cleanup: `llm memories` to see all, `llm forget <id>` to remove stale ones
- Natural language: `llm please forget everything about <topic>`

**Too many memories / slow cleanup:**
- Reduce `memory_max` in config
- Ensure server context (`-c`) is large enough for your memory count
- The cleanup pass scales linearly with memory count
