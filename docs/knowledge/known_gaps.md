# Tree Conventions & Open Known Gaps

## Tree Conventions

- **Env switches vs `Nx2Perf`:** Behaviour-changing switches use standalone `EDEN_NX2_*` env vars. `VideoCore::Nx2Perf` (`src/video_core/nx2_perf.h`) is strictly for performance switches (all on by default; `EDEN_NX2_PERF_SAFE=1` turns off only non-visual-changing experimental items).
- **Skipped draws must return:** `EvaluateDrawSkip` (`src/video_core/renderer_vulkan/draw_skip_policy.h`) enforces one contract across `BuiltPipeline`, `CanSkipDraw`, `DeferRuntimeBuild`, and `CanSkipForDiskReplay`. Native skips require depth testing, an active color target, and `RedrawnEveryFrame` (`EDEN_NX2_NATIVE_REDRAW=0` restores legacy check; `EDEN_NX2_PERF_SAFE=1` disables all 5 draw-skip/replay flags).
- **Versioned caches:** `CACHE_VERSION` guards pipeline/shader caches; `IsOwnPipelineCache` / `LoadVulkanPipelineCache` delete mismatched cache files rather than reading them.
- **Test layout:** `src/tests/` mirrors `src/` (`src/tests/video_core/...` for `src/video_core/...`).
- **Opt-in experimental geometry:** `EDEN_NX2_EXPERIMENTAL_GEOMETRY=1` enables it; off by default (`src/video_core/shader_environment.cpp`).

## Active Known Gaps

- **Asset provenance:** [ASSET_PROVENANCE.md](../../ASSET_PROVENANCE.md) tracks one remaining redistribution confirmation for Eglantine artwork (`dist/Eglantine.png`, `dist/eglantine.icns`). Keyboard firmware tables (`src/core/hle/service/set/key_code_map.h`, `[key_code_map]`) are resolved.
- **Public vs quarantined tests:** 31 title-derived cases are omitted from the public suite (stored locally in `ignore/fixtures/`); 8 independently encoded cases cover priority DMA and Ampere regressions.
- **`network` (open P0 items):**
  - `std::vector<std::string>` reads are bounded by remaining packet bytes (~8x expansion possible); an explicit element-count cap is still open.
  - Received packets are copied once before applying the relay size cap.
  - `RoomMember::GetNickname()` returns a reference to network-thread state (safe today only because its sole write precedes thread start).
- **`core` HLE (`DrainGpuCallbacks` & kernel threads):**
  - `CoreTiming::UnscheduleEvent` defaults to `Wait` (`advance_lock`). `DrainGpuCallbacks` wakes via CV + `GetEmulationStopToken()` on stop, but a wedged GPU thread mid-session remains bounded at up to 5 seconds.
  - Kernel-created threads (core, idle, shutdown, host) stay unclosed in the intrusive object list until teardown; `ClearObjectList()` prevents use-after-free, but the slab slots leak during the run.
- **`video_core` (`IsFullyMappedRange`):** Counts a reserved small page as covered by long-standing contract (native-macro paths depend on it; documented by unit test).
- **Pending cross-platform measurements:** ARM64 ISPC vs C++ texture benchmarks (after x86-64 C++ beat ISPC by 1.4x–5.7x) and an on-device Snapdragon Android APK smoke run remain open.
