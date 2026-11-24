# GPU split hangs and fence-mode freezes

Use when a run freezes at 0 fps, or the log loops on a wait, with the GPU thread split on
(fast GPU accuracy). The split runs engine methods on a back thread ("GPUBack"); the front
parses command lists and, with `EDEN_NX2_SPLIT=2`, runs macros.

## Signatures in the log

| Log line | Meaning | First suspect |
|---|---|---|
| `WaitSubmitted: Waited N s for the GPU thread to submit tick T` (repeating) | A non-back thread (fencing thread, CPU reading command lists) waits for a tick the back deferred (`Scheduler::DeferFlush`) and never submits | The back is blocked waiting on that same thread, or the back's state is stale |
| `CanResumeAcquire: Channel N waits on semaphore ... (op 1, X, now Y)` on several channels, each a few releases behind | Semaphore releases are queued behind fences that never pop | A fence change that delays the release (`delay_fence`, download-delayed fences), or an acquire recheck that never fences again (invariant below) |
| `GPU thread split: stopped` then `engine methods run on a second thread` mid-run | The user switched GPU accuracy (accurate stops the split, fast restarts it) | State the front kept across the stop |
| SEGV after `Shutdown requested` / `failed to stop` | Teardown of a hung run | Ignore; debug the first stall line |

## Invariants the split relies on

- **Whoever defers a submission must never block on work that needs it.** Only the back's idle
  hook submits deferred work. Any wait the back does on the fencing thread
  (`WaitPendingFences(true)` from `ReleaseFences`) must first call `scheduler.FlushDeferred()`
  (the fence manager's `SubmitDeferred` hook). With Default fences the callback runs inside
  `SignalFence`, so the bug only shows under Balanced/Accurate fences or delayed-download fences.
- **Back-owned state is touched only on the back.** Front code that records into the scheduler or
  the caches (fence signal, cache flush/invalidate, memory unmap) goes through `GpuSplit::Run`
  (inline on the back, else defer + drain). A drained back still submits from its idle hook, so
  "drain then call it on the front" races.
- **A suspended acquire must keep releasing fences while operations are delayed.** Under
  Balanced/Accurate fences a semaphore release is a payload query held in the query cache until
  the next fence. An acquire that fenced once and then only re-reads the word waits forever if
  the release arrived after its fence and every channel is suspended (no new fences). The
  `CanResumeAcquire` recheck releases fences again, rate-limited (1 ms), only when delayed.
- **Anything copied to the front on split start must be handed back on stop.** Engines' front
  macro state (`Front()`, `front_macros`) is released on the first engine method that runs off
  the back (`ReleaseFront`), and re-copied on restart.

## Fence behaviour setting (`gpu_fence_behavior`)

`delay_fence` = Default ? accurate GPU : (Balanced || Accurate). With fast GPU accuracy,
Immediate and Default take identical paths in this tree (fence delay, query sync and buffer-cache
sync all evaluate the same), so a difference between them is not a code-path difference: check
the first-boot shader rebuild or the log before theorising.

Lab config for a fence mode: copy `$D/qt-copy.ini` and set `gpu_fence_behavior\default=false`,
`gpu_fence_behavior=<0 Default|1 Immediate|2 Balanced|3 Accurate>`. Pass it as an extra
`--config` after `to_gameplay.sh` (argparse keeps the last). `eden-cli` doesn't log the settings
dump, so record the selected config and the resulting fence behaviour.

## Getting live stacks of a hang

If Linux ptrace policy blocks attaching, launch your owned run under GDB. Set `REPO` for the
wrapper and add `--binary .claude/skills/nx2-lab-automation/scripts/eden-gdb.sh` to the normal
bounded `lab_ctl.py launch` command. The wrapper passes Dynarmic's guest-memory SIGSEGV through.
Get approval for the SIGINT before launching. On the freeze, identify the `eden-cli` inferior of
your wrapper and signal that process. GDB prints every thread to `emulator.log` under
`=====GDB-THREADS=====`. Finish with the usual lab cleanup. If stacks are unavailable, record
the first stalled thread, the event it needs, and the thread responsible for making progress.
