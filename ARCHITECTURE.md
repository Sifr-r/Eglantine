# ARCHITECTURE.md — Eglantine

Eglantine is a Switch / Switch 2 emulator forked from Eden (Yuzu lineage). Native Switch 2
(Ampere / NVN2) and Switch 1 (Maxwell) coexist in one tree; a title runs the native path when
`Maxwell3D::GetNativeMacroEngine().native_turing` is active.

> **Token-lean docs:** Read only the single [Knowledge Card](#knowledge-cards) for your task,
> then verify behavior at the named entry point. Do not append dated verification logs here;
> record verification in PR/commit descriptions.

## Task map

| Task | Knowledge Card | Start in source | Verification |
| --- | --- | --- | --- |
| Boot, platform, HLE, saves, NUP, audio, network | [core](docs/knowledge/core.md) | [`Core::System`](src/core/core.cpp), [`PlatformProfile`](src/core/platform_profile.h), [`Nx2SessionState`](src/core/nx2_session_state.h) | `[platform-profile]`, `[platform-sizing]`, `[shutdown-ordering]`, `[nx2-session]` |
| Device behaviour the guest can observe (operation mode, applets, catalogue, saves, profiles) | [device_behaviour](docs/knowledge/device_behaviour.md) | [`AppletManager`](src/core/hle/service/am/applet_manager.cpp), [`ISystemSettingsServer`](src/core/hle/service/set/system_settings_server.cpp), [`Settings::values.use_docked_mode`](src/common/settings.h) | `[save-data]`, `[set]`, `[ldn]`, `[sockets]`, `[key_code_map]` |
| File recognition, library scans, frontends | [frontends](docs/knowledge/frontends.md) | [Loader](src/core/loader/loader.cpp), [Shared discovery](src/frontend_common/game_list_discovery.h), [Android JNI](src/android/app/src/main/jni/game_metadata.cpp), [GPUI shell](rust/apps/eglantine_gpui/src/main.rs) | `[loader-formats]`, `[config_persistence]`, `gpui_session.py`, `cargo test` |
| Host memory, GPU readbacks, textures, Vulkan | [video_core](docs/knowledge/video_core.md) | [Host memory](src/common/host_memory.cpp), [Buffer cache](src/video_core/buffer_cache/buffer_cache.h), [Texture code](src/video_core/textures) | `[host_memory]`, `[buffer-download-policy]`, `[textures]`, `[draw_skip_policy]` |
| Native shaders (SM86 lowering & direct SPIR-V) | [shader_recompiler](docs/knowledge/shader_recompiler.md) | [Ampere lowering](src/shader_recompiler/frontend/ampere/lower_to_maxwell.h), [Pipeline cache](src/video_core/renderer_vulkan/vk_pipeline_cache.cpp), [Shader triage](.claude/skills/nx2-shader-triage/SKILL.md) | `[ampere]`, `[ampere-lazy-lowering]`, controlled frame comparison |
| Rust FFI crates, language ownership, ISPC | [rust_ffi](docs/knowledge/rust_ffi.md) | [Workspace](rust/Cargo.toml), [FFI crate](rust/crates/eglantine_ffi/src/lib.rs), [AddRustCrate](CMakeModules/AddRustCrate.cmake) | `cargo test --manifest-path rust/Cargo.toml --locked --offline` |
| Controllers and Switch 2 protocol | [input_common](docs/knowledge/input_common.md) | [Switch 2 protocol](src/input_common/helpers/switch2_protocol), [Shared config](src/frontend_common/config.cpp) | `[switch2]`, hardware transport checks |
| Build, CI, release checks, live debug | [build_ci](docs/knowledge/build_ci.md) | [CMakeLists](CMakeLists.txt), [Source checks](.github/workflows/source-checks.yml), [Debug server](src/core/debugger/nx2_debug_server.cpp), [Tool index](tools/README.md) | `test_check_release_paths.py`, `test_workflows.py`, [live workflow](.claude/skills/nx2-debug-server/SKILL.md) |
| Tree conventions & active known gaps | [known_gaps](docs/knowledge/known_gaps.md) | [Nx2Perf](src/video_core/nx2_perf.h), [DrawSkip](src/video_core/renderer_vulkan/draw_skip_policy.h), [Provenance](ASSET_PROVENANCE.md) | Verify against open items before changing shared contracts |

## Build & test commands

```sh
cmake --build build --target eglantine yuzu-cmd tests --parallel
./build/bin/tests "[tag]"                                                 # tests.exe on Windows
cargo test --manifest-path rust/Cargo.toml --locked --offline             # Core Rust crates (std-only)
cargo test --locked --manifest-path rust/apps/eglantine_gpui/Cargo.toml   # GPUI desktop shell
```

## Domains & ownership

| Domain | Path | Responsibility |
| --- | --- | --- |
| `dynarmic` | `src/dynarmic` | ARM guest CPU JIT recompiler |
| `common` | `src/common` | Types, settings, VFS, logging, Windows on-demand `HostMemory` |
| `network` | `src/network` | ENet multiplayer rooms and packet serialization |
| `core` | `src/core` | Emulated machine: CPU, memory, HLE services, kernel, NUP |
| `audio_core` | `src/audio_core` | Audio output, mixing, Opus decoding, sink recovery |
| `video_core` | `src/video_core` | GPU state, memory manager, texture/buffer/pipeline caches, renderers |
| `hid_core` | `src/hid_core` | Console/controller HID protocols |
| `input_common` | `src/input_common` | Host input drivers and Switch 2 BLE/USB protocol |
| `frontend_common` | `src/frontend_common` | Shared config, discovery/metadata, play time, firmware, Sentry |
| `shader_recompiler` | `src/shader_recompiler` | Maxwell/Ampere shader translation to SPIR-V |
| `mcp` | `src/mcp` | Built-in MCP server for AI agents |
| `yuzu_cmd` | `src/yuzu_cmd` | SDL host (`eden-cli`), GPUI backend, NX2 lab runner |
| `eglantine_gpui` | `rust/apps/eglantine_gpui` | GPUI desktop shell (`eglantine`, separate Cargo workspace) |
| `updater` / `web_service` | `src/updater`, `src/web_service` | Self-update helper and web/telemetry services |
| `android` | `src/android` | Android Kotlin/Gradle UI + JNI over `core` |
| `simd_kernels` | `src/simd_kernels` | Multi-ISA ISPC texture kernels (`AddIspcKernel.cmake`) |
| `eglantine_rs` | `rust/` | `std`-only Cargo FFI bundle (`eglantine_ffi` -> `eglantine_rs`) |
| `tests` | `src/tests` | Catch2 unit tests mirroring `src/` |

C++ owns emulator state, `eden-cli`, desktop game discovery, Ampere-to-Maxwell lowering, NUP
archives, and the Switch 2 controller protocol. GPUI owns the desktop shell, Kotlin/JNI owns the
shipping Android frontend, and Rust owns direct SM86 compute SPIR-V. See the
[language ownership table](docs/knowledge/rust_ffi.md#first-party-language-and-domain-ownership)
for the crate dependencies and runtime switches.

## Knowledge cards

- [core.md](docs/knowledge/core.md) — `Core::System`, `PlatformProfile`, `Nx2SessionState`, kernel teardown, `FspSrv`/saves, `HostMemory`, `audio_core`, `network`
- [device_behaviour.md](docs/knowledge/device_behaviour.md) — what the console tells a running game: operation mode, applet/HOME lifecycle, catalogue and updates, save mounting, profiles, language and clock, amiibo, network; parity against the recomp.gg recompilation runtimes
- [video_core.md](docs/knowledge/video_core.md) — `Maxwell3D`, `GpuSplit`, `MemoryManager` lock order, `BufferCache`, `TextureCache`, `PipelineCache`, `Nx2Perf`, upscalers
- [shader_recompiler.md](docs/knowledge/shader_recompiler.md) — Lazy SM86 lowering, direct SM86 compute, warm precache, geometry parity, IR edges
- [rust_ffi.md](docs/knowledge/rust_ffi.md) — Language ownership, direct SM86 Rust crates, CMake/Cargo dependencies, Android NDK, ISPC kernels, `EDEN_NX2_*` switches
- [input_common.md](docs/knowledge/input_common.md) — `InputSubsystem`, SDL driver, Switch 2 BLE/USB protocol, `Switch2AutoPair`
- [frontends.md](docs/knowledge/frontends.md) — GPUI shell, `eden-cli`, `frontend_common` persistence, Sentry crash reporting
- [build_ci.md](docs/knowledge/build_ci.md) — `tools/nx2/` toolchain, MCP, `ignore/` boundary, CI/release checks
- [known_gaps.md](docs/knowledge/known_gaps.md) — Tree conventions and open P0/provenance/benchmark gaps
- [Historical Archive](docs/archive/README.md) — Dated bring-up logs, migration proposals, and audit snapshots (do not treat as live spec)
