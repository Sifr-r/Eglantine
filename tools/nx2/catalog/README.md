# Shader catalogue

The shader catalogue stores one JSON file per game, named with its uppercase 16-digit
program ID. Each entry records what a native program draws or computes and the observation
that identified it. The [shader lab](../shader_lab.py) displays these names next to programs.

## Look up an entry

From the repository root, use Python 3 to read the catalogue before repeating a shader
investigation. `show` prints a merged Markdown view of the repo and local copies.
`find` prints entries matching a native program key or a lowering-dependent shader hash
and exits 1 if it finds neither.

```sh
python3 -I tools/nx2/catalog.py show --title 0400486024DEC000
python3 -I tools/nx2/catalog.py find <key-or-hash>
```

## Entry format

```json
{
 "title": "0400486024DEC000",
 "game": "The Legend of Zelda: Ocarina of Time",
 "notes": "frame structure and anything that helps read the entries",
 "programs": [
  {"key": "...", "hash": "...", "stage": "pixel", "object": "Tree canopy leaf cards",
   "group": "Foliage / trees", "how": "hidden live: every tree goes bare", "notes": "...",
   "source": "build and settings it was found with", "updated": "2026-11-06T17:36:00Z"}
 ]
}
```

- `key` is the native program key. The shader dump file name (`<key>-gfx-<addr>.bin`), the
  shader lab and `ablate.txt` use it. It hashes the game's code and stays stable across builds.
- `hash` is the shader hash, as the frame dump (`"shaders"`), `reject_shaders` and
  `EDEN_NX2_REJECT_HASH` show it. It is a hash of the lowered code, so it changes when the lowering
  of that program changes. Entries copied from another build may have only a hash; the shader lab
  fills in the key when it sees a program with that hash.
- `stage` is vertex, mesh, task, geometry, pixel or compute. `how` records the evidence,
  such as a live reject and what vanished, frame dump bindings, or disassembly. `updated`
  is the UTC time of the last edit. Merging keeps the newer text field by field.

## Record and share findings

The lab writes names to the local copy at `EDEN_NX2_CATALOG_DIR` or, by default,
`%APPDATA%\eglantine\nx2-catalog` on Windows and `~/.local/share/eglantine/nx2-catalog`
elsewhere. The local copy carries over between builds and checkouts. Findings from other
tools can be added with `catalog.py add --title <program-id> --key <key> --object "..." --how "..."`.

Before a commit, run:

```sh
python3 -I tools/nx2/catalog.py status
```

Exit 0 means the repo already has the local entries. Exit 1 means the local copy has
newer or missing entries. When it exits 1, run `python3 -I tools/nx2/catalog.py merge`
and include the JSON changes in a separate catalogue commit alongside your work, as
described in [AGENTS.md](../../../AGENTS.md). The merge adds or updates entries.

Keep entries to names, stages and short notes. Shader code, textures, dumps and other game
data belong outside the catalogue.
