# AGENTS

Read and follow the [AI policy](docs/content/policies/AI.md) and [coding guidelines](docs/content/policies/Coding.md). Review and test changes before submission, and ensure commit messages and PR descriptions clearly explain the change and verification.

Keep comments to a reasonable level. Do not overexplain or add/remove comments from unrelated parts of the codebase.

## Start here

- Use the [architecture task map](ARCHITECTURE.md#task-map) to find the subsystem owner, entry point, and test filter. Check the named sources and target definitions before relying on dated reports or migration proposals.
- Inspect the worktree before editing. Preserve existing changes and limit the diff to the requested domain. Project documentation lives under `docs/`. Treat `.cache/cpm/`, `build/`, and `ignore/` as local inputs or outputs.
- Reproduce using the smallest relevant test or supported entry point. For shared behavior, check the affected frontends and record the platform, command, result, and limits of the verification. Use the [tool index](tools/README.md) for the exact supported interfaces.
- Keep console keys, firmware, game files, and raw captures in local-only storage. Use synthetic or independently authored fixtures in tracked tests and docs.

## Switch 2 (NX2) work

- Build with `cmake --build build --target yuzu yuzu-cmd tests --parallel`. The binaries in `build/bin/` are `eglantine` (Qt), `eden-cli` (SDL), and `tests` (Catch2). Add `.exe` on Windows. Use `./build/bin/tests "[ampere]"` for the lowering suite.
- Start with [shader triage](.claude/skills/nx2-shader-triage/SKILL.md) for rejected shaders, lowering, or visual bugs. Use [lab automation](.claude/skills/nx2-lab-automation/SKILL.md) for unattended SDL runs, scripted input, and frame capture.
- Bound unattended runs with `lab_ctl.py launch --max-seconds` and stop the instance with `lab_ctl.py stop` when finished. A hidden instance can keep the GPU busy after the investigation ends.
- For a running game, use the [live debug workflow](.claude/skills/nx2-debug-server/SKILL.md) with `EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545` and `tools/nx2/debug_mcp.py`. Try a live experiment before requesting a new build or run. Restore every hash you reject, and leave the lifecycle of user-launched instances to the user.

## Shader catalogue

`tools/nx2/catalog/<program id>.json` lists, per game, the native programs people have identified (what they draw, how it was found), so the next investigation starts from them. Entries are keyed by native program key (the shader dump and shader lab key, the same in every build); `hash` is the frame dump / `reject_shaders` hash, which changes when that program's lowering changes. `tools/nx2/catalog/README.md` has the format.

- The shader lab (`tools/nx2/shader_lab.py`) saves what you name to your local copy (`EDEN_NX2_CATALOG_DIR`, default `%APPDATA%\eglantine\nx2-catalog` or `~/.local/share/eglantine/nx2-catalog`), which carries over between builds and checkouts. Programs identified another way (debug server rejects, frame dumps): `python3 -I tools/nx2/catalog.py add --title <program id> --key <key> [--hash <hash>] --object "..." --how "..."`.
- **Every commit uploads the catalogue when yours has more.** Before each commit run `python3 -I tools/nx2/catalog.py status`. If it exits 1 (your local copy has entries the repo copy lacks, or newer ones), run `python3 -I tools/nx2/catalog.py merge` and commit the changed `tools/nx2/catalog/*.json` as its own commit (`tools/nx2: catalogue: <game>: <n> programs`) next to your work, so it goes up with the same push. If the repo copy already has everything, change nothing. `merge` only adds and updates; never delete entries from the repo copy.
- Names, stages and short notes only: no shader code, textures, dumps or other game data.
