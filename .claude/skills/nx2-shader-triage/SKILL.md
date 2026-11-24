---
name: nx2-shader-triage
description: "Use when an NX2 native graphics or compute shader is rejected, an Ampere opcode needs lowering, or an NX2 scene is missing an effect."
version: 1.0.0
author: kraibse
license: MIT
metadata:
  hermes:
    tags: [eglantine, eden, switch2, ampere, sass, shader, lowering, maxwell]
    related_skills: [git-branch-reconciliation, switch-emulator-diagnostics]
---

# NX2 native shader triage (Eglantine/Eden)

Switch 2 titles ship Ampere SASS. `src/shader_recompiler/frontend/ampere/lower_to_maxwell.h` lowers
native programs to Maxwell code. A rejected instruction can reject the whole program on this path.
Test the same title, stage, lowering options and build when comparing failures.

## When to Use

- For a native shader rejection or unsupported opcode, begin with the log and re-lower the
  rejected words offline.
- For `Draw skipped (no pipeline)`, look first for a lowering or pipeline creation error. When
  programs lower and pipelines build, follow the frame investigation.
  [Visual bug investigation](references/visual-bug-investigation.md)
- If the user is already in the affected scene, collect live evidence before requesting a
  relaunch. [NX2 debug server](../nx2-debug-server/SKILL.md)

## Rules (always-on)

- **Never treat `vulkan.bin` as a source of rejected programs.** Use native shader dumps for
  rejected programs because those programs never got a pipeline.
- **Start from the log, finish from a dump.** The default log is
  `~/.local/share/eglantine/log/eden_log.txt` on Linux and
  `%APPDATA%\eglantine\log\eden_log.txt` on Windows. Check the active profile when a portable
  or custom data directory is in use. The log has one
  `Native ... shader rejected` line per program with the four instruction words, but only the
  *first* rejection per program. Once you fix those, the same programs can fail later on. Confirm
  with an `EDEN_NX2_SHADER_DUMP` run before calling a game clean.
- **Check which build wrote the log** (`Eglantine Version: ... | <branch>-<sha>` in the first line)
  before blaming the current code.
- **HMMA (0x23c) can be intentionally disabled.** With `EDEN_NX2_TENSOR` unset, the title and
  renderer choose whether to emulate HMMA or use the stand-in. Donkey Kong Bananza and supported
  Ocarina of Time configurations enable emulation. For a colour cast, compare the same scene
  with the title default, `EDEN_NX2_TENSOR=1` (forced FFMA emulation), and `=stub` (returns C).
  Record which mode was active because it changes the output.
- **Start from the game's shader catalogue** (`tools/nx2/catalog/<program id>.json`, `python3 -I
  tools/nx2/catalog.py find <key or hash>`): programs others already identified (trees, grass,
  culling computes) with how they were found. Add every program you identify (`catalog.py add`, or
  the shader lab's Catalogue box), and before committing run `catalog.py status` / `merge` as
  AGENTS.md says.
- Restore every reject you add and confirm the original set is restored. Never start or stop the
  user's emulator. Give your own hidden lab run an explicit watchdog and a stop on every exit
  path. Ask before a long series of runs if the user may be using the PC.
  [Lab automation](../nx2-lab-automation/SKILL.md)
- **Never suggest `EDEN_NATIVE_SHADER_OVERRIDE_DIR`.** It pauses the GPU thread for up to 30 min per
  rejected shader.
- Commits are local only. See the [AI policy](../../../docs/content/policies/AI.md).

## Procedure

### 1. Group the logged rejections

Set `D` to an existing scratch directory and `L` to the log for the build under test. This
example uses the Linux default profile:

```bash
L="${XDG_DATA_HOME:-$HOME/.local/share}/eglantine/log/eden_log.txt"
grep -a -o "rejected: .*" "$L" | sed -E 's/Native shader at 0x[0-9a-f]+, instruction 0x[0-9a-f]+: //; s/\[op ([0-9a-f]+) .*/op \1/' | sort | uniq -c | sort -rn
grep -a -o "rejected: .*" "$L" | grep -o "\[op [0-9a-f]* [0-9a-f ]*\]" | sed -E 's/\[op [0-9a-f]+ //; s/\]//' | sort -u > "$D/uniq.txt"
```

- `unsupported native opcode`: no `case` for the low 12 bits of word 0.
- Any other `native ... mode`: the case exists but a validity check failed. Usually a narrow fix.
- `native program uses lowering scratch registers`: the opcode is missing from
  `PrepareRegisterPressure`'s operand table. Add a case listing its register operand bit offsets.

### 2. Re-lower the unique words against the current tree

`scripts/relower_words.cpp` lowers each `w0 w1 w2 w3` line followed by EXIT. From the repository
root, with a C++20 compiler, fmt headers and libfmt available, build and run it in the scratch
directory `D`:

```bash
c++ -std=c++20 -O1 -I src .claude/skills/nx2-shader-triage/scripts/relower_words.cpp -lfmt -o "$D/relower"
"$D/relower" < "$D/uniq.txt"
```

An argument enables HMMA emulation in this helper. Compare full programs when the result depends
on uniform-bank use, register pressure, or call linkage.

### 3. Dump run (when the log is exhausted)

Use the bounded lab run and pass `launch --dump-dir "$D/shader-dump"` in addition to the normal
launch arguments. Create the dump directory first. Leave the title's tensor selection intact for
the baseline and enable it explicitly only for a tensor comparison.

Writes `<key>-gfx-<addr>.bin` / `-cs-` (16 bytes per instruction, no header), rejected programs
included. Build `scripts/lower_dump.cpp` like the single-instruction helper. The resulting binary
takes dump paths as arguments and prints `OK <filename> <stage> <hash>` or `REJECT` for each file.
This helper enables tensor lowering, so use its output to test lowering coverage. Runtime
acceptance still uses the intended title settings. Pass dump paths as arguments from the repo.

- **Loop until the dump is clean, not the log.** Fixing a program's first rejection exposes its
  next one; re-run `lower_dump` after every batch (Ocarina went 800 → 814 → 829 → 833).
- **Prove no unintended change by hash.** Run the same dump set through `lower_dump` built from
  the baseline and candidate trees. Compare the `OK` entries by filename and stage and explain
  any changed hashes.

### 4. Decode the failing check

Evaluate the words against the `case`'s `fail(...)` conditions (`insn.Bits(offset, count)` is a
128-bit field read; a 6-line Python bits() over the four words is enough). For field meanings use
Mesa NAK via the GitLab API (the web UI is Anubis-gated):

```bash
curl -sL "https://gitlab.freedesktop.org/api/v4/projects/176/repository/files/src%2Fnouveau%2Fcompiler%2Fnak%2Fsm70_encode.rs/raw?ref=main"
curl -sL "https://gitlab.freedesktop.org/api/v4/projects/176/repository/files/src%2Fnouveau%2Fcompiler%2Fnak_nir_lower_tex.c/raw?ref=main"
```

`sm70_encode.rs` gives the bit fields (`impl SM70Op for Op<Name>`). `nak_nir_lower_tex.c` gives how
texture sources are split across Ra/Rb. `references/texture-operands.md` has the decoded TEX/TLD4
layout.

### 5. Fix narrowly, then check the target side

Make the check agree with what the emit can express. Before widening a texture or memory form, read
the Maxwell translator it lowers to (`frontend/maxwell/translate/impl/*.cpp`) to confirm it reads the
same operand lists, and the SPIR-V emitter for constraints (e.g. PTP gather offsets must be
constants or are dropped with a warning).

### 6. Verify

- Add a focused test in `src/tests/video_core/ampere_lowering.cpp` using the smallest opcode
  fixture that reproduces the lowering error. Check the emitted fields affected by the change.
- Run `cmake --build build --target tests yuzu yuzu-cmd --parallel`, then
  `./build/bin/tests "[ampere]"` and the full suite. Use `tests.exe` on Windows.
- Re-run the single-instruction and dump checks. Record the targeted opcode, before/after
  rejection counts, changes to previously successful hashes, and the result of in-game
  reproduction when run. Report offline and gameplay evidence separately.

## Pitfalls

- **"All programs lower" is not "all pipelines build".** After the dump is clean, grep the log
  for `CreateComputePipeline:`/`CreateGraphicsPipeline:` errors. `SPIR-V Instruction is not
  implemented` right after `Storage buffer failed to track` means an untracked global atomic: the
  routed 32-bit forms exist since 6b934346d7 (`global_atomic_funcs`); 64-bit/packed still throw.
  Validate emitted modules with `spirv-val --target-env vulkan1.1 --uniform-buffer-standard-layout`.
- **`Draw skipped (no pipeline)` with no rejections** is usually `use_asynchronous_shaders=true`
  (`BuiltPipeline` skips depth draws while compiling); every lowering change re-hashes and
  rebuilds pipelines, so it spikes after each fix. Check the active `qt-config.ini`.
- **A log that just stops is a crash.** `coredumpctl list | grep eglantine`, then
  `coredumpctl info <pid>` for the symbolized top frames (gdb on the 13 GB core often can't unwind;
  delete dumped cores from scratch afterwards). A garbage command stream (nonsense formats, MME
  `start` outside uploaded RAM, syncpoint past the table) right before the end is the cause, not
  the crash site.

- **Stage 3 in `RefreshStages: Rejected program ... bound to stage 3` is the mesh slot, not
  tessellation.** NVN mesh programs sit in the tessellation slot and lower with geometry off; the
  dump name carries no stage, so `lower_dump` keeps whichever of plain/geometry gets furthest.
  Eglantine's `LoweringOptions` has no tessellation stage (Lichii's does).
- **Shadow-table operands: only the base register triggers shadowing.** `PrepareRegisterPressure`
  rejects an operand spanning two reserved quartets or straddling `reserved`, but an operand whose
  base is just below the boundary (R215 pair into R216) never reaches it. Known gap, not fixed.
- **Old caps like `carry_out > 1` (UP0/UP1 only) were scope, not hardware limits.** If the emit
  already goes through P(6-n)/R239 like UISETP, widen to UP0-UP6.

- **A dry `patch` of a fork's lowering commit can apply "with fuzz" into the wrong case.** The cases
  are long and similar. Read every fuzzed hunk.
- **Shadow boundary differs by design** between Eglantine (rounded down to 244) and Lichii (246).
  Different but equivalent output for R244/R245 operands is not a porting bug.
- **Don't edit the header while a `ninja` build of it is running.** The object can compile a
  half-edited state. Prepare edits in a scratch copy, or wait.

## Support files

- `scripts/relower_words.cpp`: lower `w0 w1 w2 w3` lines from stdin.
- `scripts/lower_dump.cpp`: lower every dumped program (geometry retried for `-gfx-`).
- `references/texture-operands.md`: Ampere TEX/TLD4 field and source-list layout, with Maxwell TLD4 mapping.
- `references/visual-bug-investigation.md`: lab_ctl workflow, image dumps, the A/B ladder and the
  bisection pitfalls for tints and missing effects.
- `scripts/lowered_for.cpp`: the Maxwell words one or more Ampere instructions of a dumped program
  lower to (via `source_instruction_offsets`), for checking one path instruction by instruction.
