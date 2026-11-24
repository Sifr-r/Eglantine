# Splitting the GPU thread

This design and the measurements below record the October 2026 investigation. The current
[split implementation](src/video_core/gpu_split.cpp) defaults to mode 2 for eligible Ampere
sessions using Vulkan and can disengage when GPU or DMA accuracy settings require the front
thread to own the caches. `EDEN_NX2_SPLIT=0` selects a single thread. Use the
[split write tests](src/tests/video_core/split_write_tracker.cpp) with `[split_writes]`, then
repeat a bounded gameplay comparison using the [baseline procedure](migration_docs/00_PREREQUISITES_AND_BASELINE.md#compare-a-gameplay-profile).

In the recorded run, one host GPU thread limited title 1 (Mario Kart World). It was ~100% busy while the
host GPU is 20-40% busy and the game's own CPU thread mostly polls the counter. Per second of
3D scenes (profiled, 2026-10-29): command processing and register writes ~335 ms, macros
~200 ms (1.3 million calls), draw setup ~345 ms (55,000 draws), compute ~70 ms. The changes in
[investigation rows 123–139](NX2_NOTES.md#blockers-hit-in-order) reduced frame times from
~22–38 ms to ~18–26 ms. The investigation then targeted work across multiple cores to reach
60 fps on the heavy courses.

## Original split point: method calls

- **Front thread** (the current GPU thread): takes the CPU's command queue, parses GPFIFO
  entries and command lists (DmaPusher), runs macros (compiled, macro_turing_jit.h), and keeps
  what macros read: a mirror of each engine's registers, its macro code and data RAM.
- **Back thread**: executes the resulting stream of method writes in order: Maxwell3D /
  KeplerCompute / Fermi2D / DMA engines' CallMethod and everything behind them (register
  state, dirty flags, constant buffer and inline uploads, draws, dispatches, queries, fences,
  the rasterizer and its caches). Only the back thread touches the rasterizer.
- The stream: an SPSC ring of {engine, method, argument} plus copied data for multi-word
  methods (command lists are often guest memory read in place, which the guest reuses).
  CPU-side commands (flush/invalidate requests, RunInOrder actions, presentation) are
  forwarded into the same ring so they keep their order.

## What the front needs without waiting

- Register values macros read (MME_READ): the front mirrors every register write it sends.
  Back-side changes to registers it must reproduce: constant buffer offset (+4 per
  const_buffer.buffer word), shadow_scratch[0] = 1 (falcon[4]), shadow RAM control (Track /
  Replay change the effective value), macro upload pointers (the front handles load_mme
  itself). Compute: native_registers (0xad is changed temporarily by the inline QMD path).
- Macro code uploads and binds (load_mme.*, compute 0x45-0x48): handled by the front.

## Where the front waits for the back (sync points)

- Macro DMA reads (MME_DMA_READ / _FIFOED, ~450 a second): read guest memory as of their
  position, so the back must have executed everything before them.
- Semaphore acquires that are not satisfied yet (~10,500 a second, cross-channel waits on QMD
  releases): the release is executed by the back, so the front waits until the semaphore
  holds (or parks the channel and runs another one, as the pending-channel code does now).
- Syncpoint waits, WaitForIdle-style methods, and anything else that reads guest memory or
  host state on the front.
- Expected cost: each wait drains the back only up to the dependency, then the back idles
  until the front sends the next method (a few microseconds), so even ~10,000 waits a second
  leave most of the overlap. Ceiling: front ~35-40% of the work, back ~60-65% -> ~1.4-1.6x.

## Original rollout stages

1. Instrument: count sync points per second and the front/back share precisely (zones).
2. Ring + back thread executing method writes; the front still does everything else.
   Correctness first: an EDEN_NX2_SPLIT=0 switch runs both on one thread.
3. Move macros to the front with the register mirrors; verify mirrors against the back's
   registers in a debug mode (compare on every MME_READ).
4. Sync points; crash and screenshot tests across courses; then title 4.

## Recorded status (2026-10-29)

Stages 1–3 landed with the split initially disabled (`babd395`, `gpu_split.h`).
`EDEN_NX2_SPLIT=1` ran engine methods on the back. Mode 2 also ran the 3D and compute macros on the
front against register copies. Title 1 measured ~44,000 draws a second on one thread, ~47,400 with
stage 2 and 50,000–66,500 with stage 3. The front waited ~4,000–5,000 times a second for semaphore
acquires and MME DMA reads. The back spent ~200 ms a second outside its measured zones, including
ring dispatch and the method sink. The A/B in [investigation rows 142–145](NX2_NOTES.md#blockers-hit-in-order)
found 1.46× throughput and 17% shorter frame times, after which the split was enabled by default.
The back was ~92% busy, with ~53% of that time spent on draw setup, which became the next target.
