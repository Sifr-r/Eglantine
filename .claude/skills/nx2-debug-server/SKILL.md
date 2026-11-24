---
name: nx2-debug-server
description: "Use when inspecting a running NX2 game, bisecting a visual bug with live shader rejects, or looking up and recording a native program in the shader catalogue."
version: 1.0.0
author: kraibse
license: MIT
---

# NX2 debug server, MCP adapter and shader catalogue

References: [frame dumps and runtime limits](../../../docs/content/nx2/debug-mcp.md) and
[shader catalogue](../../../tools/nx2/catalog/README.md).

Start with `status` on the live session. Record the renderer, build, title, and program ID, then
take a screenshot or read the log around the symptom. For a shader that already has a key or
hash, check the catalogue before running a new experiment.

## Rules

- Use the emulator the user launched with `EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545` or your own
  bounded lab session. Never start or stop the user's emulator.
- Try a live experiment before asking for a new build or run. Use
  [lab automation](../nx2-lab-automation/SKILL.md) for an owned test run.
- Restore every hash you reject, release injected input, and restore the original bytes of
  every memory write before finishing. Use one input source at a time.
- `write_memory` requires `EDEN_NX2_DEBUG_WRITE=1` on the emulator. Leave it off unless needed.

## Access

The repository's `.mcp.json` declares two MCP servers:

- `eglantine` (HTTP, 127.0.0.1:6544, built into the emulator, PR #421): emulator-wide. Boot/pause/stop games, event timeline (logs, crashes, unimplemented service commands), IPC/SVC trace, memory watch, GDB breakpoints, GPU profile, routes, A/B tests. Needs the server enabled (Tools > MCP Server or `--mcp`); input and memory writes need Allow Writes. Client: `tools/mcp/scripts/mcp.py`.
- `nx2-debug` (stdio adapter to the loopback debug server on 6545): rendering and shaders. `capture_frame`, live `reject_shaders` by key/hash, `read_gpu_memory`, frame-exact `input_sequence`. Needs `EDEN_NX2_DEBUG_SERVER`.

Use `eglantine` for emulator-wide timeline and traces, and `nx2-debug` for shader and frame
investigation. Select the server explicitly when their tool names overlap.

`nx2-debug` runs `python3 -I tools/nx2/debug_mcp.py` and reconnects after a game restart. If the MCP
tools are unavailable, use one-shot calls from the repository root. Use `--server host:port` if
the session uses a different loopback port.

```sh
python3 -I tools/nx2/debug_mcp.py --call '{"method":"status"}'
python3 -I tools/nx2/debug_mcp.py --call '{"method":"screenshot","params":{"height":1080}}' --out shot.png
python3 -I tools/nx2/debug_mcp.py --call '{"method":"reject_shaders","params":{"hashes":["<hex16>"]}}'
python3 -I tools/nx2/debug_mcp.py --call '{"method":"reject_shaders","params":{"hashes":["<hex16>"],"reject":false}}'  # restore
python3 -I tools/nx2/debug_mcp.py --call '{"method":"reject_shaders"}'  # no hashes: list rejected
python3 -I tools/nx2/debug_mcp.py --call '{"method":"capture_frame","params":{"index":true}}'
```

Hashes and keys must be all 16 hex digits.

## Tools

| Tool | Use |
|---|---|
| `status` | title, program id, fps, paused, rejected hashes |
| `screenshot` | PNG of next presented frame |
| `log_tail`, `log_filter` | new log lines; change filter live |
| `capture_frame` | frame dump now (`sampled`, `buffers` options); `index` builds contact sheet |
| `capture_image` | one PNG of a capture made this session |
| `reject_shaders` | reject/restore programs by native key or shader hash |
| `read_memory`, `search_memory`, `read_gpu_memory` | guest/GPU memory (<= 64 KiB per read) |
| `input`, `input_sequence` | buttons/sticks for exact numbers of presented frames |
| `sample` | profiler (Windows only) |

Limits: `capture_frame`, `reject_shaders`, `read_gpu_memory` are Vulkan only; fps and input follow Switch 2 presents; one request at a time; no pause or frame step.

## Bisecting a visual bug

1. Take a baseline screenshot and save the baseline rejected set with `reject_shaders` and no hashes.
2. Reject half the candidates from the capture manifest, `diag-state.json`, or the catalogue.
   Wait for `applied: true` before comparing the next screenshot. A paused or loading game may
   need another presented frame before the pipeline change takes effect.
3. Narrow the candidates against the same scene. Restore only the keys or hashes added for the
   experiment with `reject: false`, including any request that timed out waiting for a frame.
4. Verify that the rejected set matches the baseline and that the restore was applied. Record
   the key, build-specific hash, symptom and reproducible evidence in the catalogue.

## Keys vs hashes

- `key`: native program key, hash of the game's code, same in every build. Shader dump names, lab, `ablate.txt`.
- `hash`: shader hash of the lowered code; changes when that program's lowering changes. Frame dump `shaders`, `reject_shaders`.
- Both work in `reject_shaders` and `EDEN_NX2_REJECT_HASH`.

## Shader catalogue

`tools/nx2/catalog/<program id>.json` (one per game, Ocarina of Time = `0400486024DEC000`). Entries: key, hash, stage, object, group, how, notes, source, updated. Names, stages and short notes only; no shader code, textures or dumps.

The shader lab keeps a local copy (`EDEN_NX2_CATALOG_DIR`, default
`~/.local/share/eglantine/nx2-catalog` or `%APPDATA%\eglantine\nx2-catalog`) and merges the repo
copy at start.

```sh
python3 -I tools/nx2/catalog.py find <key or hash>
python3 -I tools/nx2/catalog.py show --title <id>
python3 -I tools/nx2/catalog.py add --title <id> --key <k> [--hash <h>] --object "..." --how "..."
python3 -I tools/nx2/catalog.py status   # before every commit; exit 1 = local has more
python3 -I tools/nx2/catalog.py merge    # then commit tools/nx2/catalog/*.json on its own:
                                         # "tools/nx2: catalogue: <game>: <n> programs"
```

`merge` only adds/updates; never delete entries from the repo copy. Look a program up with `find` before investigating it from scratch.
