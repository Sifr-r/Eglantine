---
name: nx2-lab-automation
description: "Use when reproducing an NX2 gameplay, rendering, crash, or hang issue in an unattended Eglantine run."
version: 1.0.0
author: kraibse
license: MIT
metadata:
  hermes:
    tags: [eglantine, eden, switch2, automation, headless, eden-cli, playtest]
    related_skills: [nx2-shader-triage, switch-emulator-diagnostics]
---

# Driving Eglantine NX2 runs unattended

Use `eden-cli` and a scratch diagnostic directory to replay a scene and capture evidence.
If a user-launched session is already running with live diagnostics enabled, inspect it first.
[NX2 debug server](../nx2-debug-server/SKILL.md)

## When to Use

- Reproducing a visual bug, crash or hang instead of asking the user to "launch it again".
- Checking a fix in game, A/B-ing environment switches (`EDEN_NX2_TENSOR`, `EDEN_NX2_DUMP`, ...).
- Any "test and control it yourself" request on an NX2 title.
- Not for lowering rejections alone: `nx2-shader-triage` handles those offline from dumps.

## Rules

- **Always bound the run.** Pass a positive `--max-seconds` and call `lab_ctl stop` on every exit
  path, using the same diagnostic directory. Hidden runs use the GPU.
- **Keep runs short.** The user's own checks take 1-2 minutes; boot-to-save (~130 s with
  `scripts/to_gameplay.sh`) plus about 1 minute of gameplay is enough for a fix check. Don't run
  4-5 minute walks or sleep through long fixed waits. Ask before a long series of runs if the user
  may be at the PC.
- **Use the user's graphics settings, your key bindings.** `eden-cli` reads `sdl2-config.ini`, whose
  graphics settings differ from the Qt app's (async shaders, accuracy, docked). Make a scratch copy
  of `~/.config/eglantine/qt-config.ini` (default Linux path). Merge in the SDL file's
  `player_0_button_*`, `lstick`, `rstick` and `motion` lines and pass `launch --config`.
- **Never `cd` into dumps or run scripts from them.** Pass paths as arguments, and run Python
  with `-I`.
- Check `pgrep -af 'eden-cli|eglantine'` before launching. Start a lab instance only when the
  user's emulator is out of the way. Stop only the instance you launched. When the user starts
  testing manually, use `lab_ctl stop` on your lab directory.
- **Lab-run every port before the user gets the build**, at least boot-to-save plus a walk.
  Sync and fence-timing changes from forks can deadlock in this tree's GPU split even when they
  work in the fork; the user should never be the first to boot an untested change.
- **A "capped at 30 fps" report is often `gpu_fence_behavior=Balanced` (value 2) in the user's
  `qt-config.ini`, not a code regression.** Balanced costs ~11 ms of front-thread time here:
  same 50.4 ms GPU-bound lab frame, front 16.7 ms with Default (fence 33.8 ms) against 28.2 ms with
  Balanced (fence 20–24 ms), which is exactly what turns a ~17 ms frame into two-vblank 33 ms
  frames. Check the setting before bisecting commits; the lab is GPU-bound, so measure the front as
  `frame - fence - pacing` rather than by frame time.

## Tooling (`tools/nx2/lab_ctl.py`)

Run from the repository root with Python 3. The launcher and stopper use POSIX process groups
and a `/bin/sh` watchdog. On Windows, use the live debug adapter for an existing session or a
Windows runner with a bounded lifetime and owned-process cleanup.

Prepare a fresh scratch directory in `D` with the merged `qt-copy.ini` and set `GAME` to the
local game path. Launch in Bash:

```bash
L=(python3 -I tools/nx2/lab_ctl.py -d "${D:?Set D to a fresh scratch directory}")
test ! -e "$D/emulator.pid" || exit 1
trap 'if [ -f "$D/emulator.pid" ]; then "${L[@]}" stop; fi' EXIT
"${L[@]}" launch --hidden --config "$D/qt-copy.ini" --game "${GAME:?Set GAME}" --max-seconds 300 || exit
```

When the title or scene is ready, replay the relevant input and capture the result:

```bash
"${L[@]}" press a --hold 200
"${L[@]}" capture --settle 5 --timeout 15 --out "$D/scene.png"
"${L[@]}" stop
```

- `drawlist` shows the latest capture's render targets, and `state --kind c` shows recent compute
  programs. Use `p` for pixel or `v` for vertex programs.
- `ablate add`, `remove`, and `set` take native program keys. `ablate clear` clears the lab list.
  `bisect --kind p --bad 'b < 0.3 * g'` tests a screenshot statistic during ablation.
- Live keys: `eden-cli` polls `<EDEN_NX2_DIAG_DIR>/keys.txt` (`<scancode> [ms]` lines; it renames
  and deletes the file). `EDEN_NX2_KEY_SCRIPT` is the timed, at-launch alternative;
  `EDEN_NX2_KEY_RECORD=<file>` records a session in the same format. A `gpu low` / `gpu high`
  line sets the GPU accuracy mid-game and applies it, like the Qt app's Change GPU Mode hotkey.
  For the default right-stick bindings, use numeric scancodes with `press`. I/J/K/L are
  12/13/14/15 for up/left/down/right.
- `EDEN_NX2_HIDE_WINDOW=1` (set by `--hidden`) creates the window hidden and unfocusable.
- Screenshots are 1280x720 TGA, the presented frame. `capture` converts to PNG without Pillow; look
  at it with vision, not just the stats.
- Image dumps: `--env EDEN_NX2_FRAME_CAPTURE=<dir> --env EDEN_NX2_DUMP=<present>:<addr>:<size>,...`
  write raw guest bytes at that present (flushed from the host first). Addresses come from
  `drawlist` and the log.
- Viewing a dumped program: `python3 -I tools/nx2/nvd_cubin.py <dump.bin>` disassembles Ampere SASS.

## Procedure

1. Build the `yuzu-cmd` target (`cmake --build build --target yuzu-cmd`) to produce `build/bin/eden-cli`.
2. Prepare `$D/qt-copy.ini` as above, once per session.
3. Launch, wait for the title (title-specific timing in references), and press through to the
   scene. Write the sequence as a small script in `$D` so later runs repeat it exactly.
4. `capture` for numbers and a PNG. Compare against the user's reference screenshot.
5. Change one variable per run (env switch, ablation, build) and capture again.
6. Use `stop` and check `$D/emulator.log`. On Linux, check `coredumpctl list` if the process
   ended early. Record the build, config and overrides, reached scene, run duration, capture
   evidence and how the process exited.
7. **Fix checks are short walks, not one capture.** Loop `press <dir> --hold 2000` over a few
   directions for about a minute, then count `signal 11/SEGV` in `journalctl`, `owner check` lines
   and stalls in the log. To provoke a reported freeze, replay what the user did (mode switch,
   camera pan) rather than walking longer.
8. **One-frame artifacts (flashing): pan and burst-capture.** Write `15 900` (camera right) to
   `keys.txt`, `capture --settle 1` right after, repeat ~12 times; paste the PNGs into one contact
   sheet (PIL) and compare neighbours with vision, then zoom (`region`) into any patch that is
   only in one frame. Single captures while walking miss them.
9. **Flashes with a still camera: burst-capture every present.** Launch with
   `--env EDEN_NX2_FRAME_CAPTURE=<dir>` and write N to `<dir>/burst`: the next N presents are saved
   as `nx2-frame-<present>.tga` (the asynchronous readback can skip presents). The local OoT
   helper `zsh .claude/skills/nx2-lab-automation/scripts/oot_burst.sh <tag> <N> [launch args]`
   boots, bursts and stops. Invoke `python3 -I .claude/skills/nx2-lab-automation/scripts/flash_scan.py
   <dir> <sheet.png>` with NumPy and Pillow installed. It finds
   pixels >0.3 brighter than the median of the 8 frames around them and makes a sheet of the worst
   event. 400-600 frames (20-30 s in the lab) caught ~1 flash per run in OoT.
10. **A/B a fix against a baseline build** when the symptom is intermittent. Use binaries from
   separate build directories or a previously saved baseline. Run each with
   `launch --binary <path>` and the same script, then compare verifier differences, luma spread,
   or stall counts.

### A crash the user reports from the Qt app

Read the log timeline before any stack. The default current log is
`~/.local/share/eglantine/log/eden_log.txt` on Linux or
`%APPDATA%\eglantine\log\eden_log.txt` on Windows. Use the active profile path for portable and
custom setups. The previous log is `eden_log.txt.old.txt`.

```bash
tr -d '\000' < "$LOG" | grep -a -E "LogSettings: M- Renderer.gpu_accuracy|GPU thread split|Waited [0-9]+ s|Shutdown requested|Userspace PANIC|failed to stop|SIGSEGV"
```

- `Waited N s for the GPU thread`, then `Shutdown requested`, then `Userspace PANIC ... 2010-0212`,
  then `Emulation thread failed to stop` and a SEGV is a **hang**. The user closed it, the forced
  stop made the guest abort, and the SEGV is teardown. Debug the first stall line, not the SEGV.
- A core written at teardown has no emulation threads left and often won't symbolize. Only a SEGV
  without a preceding stall has a useful `coredumpctl info` stack.
- Several `Channel N waits on semaphore` lines at once, or a looping `Waited N s for the GPU
  thread`, is a fence/split deadlock: see `references/gpu-split-hangs.md` for the signatures,
  the split invariants and how to get live stacks.
- `GPU thread split: stopped` / `engine methods run on a second thread` mid-log means the user
  switched GPU accuracy during the run. Reproduce that sequence (`gpu high`/`gpu low` in
  `keys.txt`) before theorising, and ask the user what they did around the event if the log
  doesn't show it.

## Pitfalls

- **No input reaches the game:** the config's player 1 is a gamepad. Merge the keyboard bindings
  (rule above). Hold presses at least 200 ms; the title may ignore a 120 ms tap.
- **`capture` times out:** the game isn't presenting (loading, paused, crashed), or a previous
  screenshot is still pending in the GSV capture hook. Check `state` (does the frame count advance?)
  and capture again.
- **Stale results across runs:** capture numbers restart per process. `launch` clears the old
  `capture-*.tga`/`draw-list-*.tsv`, and `capture` matches by mtime.
- **Hidden runs still use the GPU fully:** keep `--max-seconds`.
- Stop runs with `lab_ctl stop` for your own directory. Verify ownership before using a PID for
  recovery.
- **Fixed boot sleeps waste most of a run.** Time a title once with a capture every 10 s (luma
  shows black/title/load/gameplay), then press at those points and poll `capture` luma for the
  first gameplay frame instead of sleeping.
- **Reproducing a hang after a mid-game settings switch:** accurate GPU mode stops the GPU split
  and fast mode restarts it. Write `gpu high`, then `gpu low`, to `keys.txt`, and run with
  `--env EDEN_NX2_SPLIT_VERIFY=1`: "Split front register" warnings right after the restart mean the
  front's macro state went stale. The verifier resyncs what it finds, so verified runs don't hang;
  compare the difference counts instead.
- The shader lab browser UI (`EDEN_NX2_LAB=open`) and `lab_ctl` share the same files and can run
  together. The lab shows each program's shader catalogue entry and saves new names to the local
  catalogue (`tools/nx2/catalog.py`); `catalog.py status` / `merge` before a commit (AGENTS.md).
- The debug server works in `eden-cli` too. `input_sequence` replays frame-timed moves and
  `capture_frame` dumps the frame's passes. [NX2 debug server](../nx2-debug-server/SKILL.md)

## References

- `references/ocarina-of-time.md`: OOT paths, timings, key sequence and findings (network
  programs, stand-in behaviour, colour-cast status).
- `scripts/to_gameplay.sh`: boot OoT hidden and load the newest save. Use Zsh and see the
  [OoT setup](references/ocarina-of-time.md) for required local paths.
  Extra args go to `launch`; a later `--config`/`--binary` overrides the script's.
- `references/gpu-split-hangs.md`: freeze signatures, GPU split invariants, fence-behaviour modes,
  lab configs per fence mode, live stacks under gdb.
- `scripts/eden-gdb.sh`: `eden-cli` under gdb for `launch --binary` (stacks on SIGINT).
