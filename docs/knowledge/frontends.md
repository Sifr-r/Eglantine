# `frontends` — GPUI Shell, SDL Host, `frontend_common`, and Sentry

Entry points: `rust/apps/eglantine_gpui/src/main.rs`, `src/yuzu_cmd/yuzu.cpp`, `src/frontend_common/config.cpp`.

## Targets & Responsibilities

| CMake Target | Output (`build/bin/`) | Role |
|---|---|---|
| `eglantine` (`eglantine-gpui` alias) | `eglantine` | Default GPUI desktop shell (`ENABLE_GPUI=ON`) |
| `yuzu-cmd` | `eden-cli` | SDL gameplay host, headless GPUI metadata/content backend, NX2 lab runner |

## GPUI Desktop Frontend (`rust/apps/eglantine_gpui/`)

GPUI owns the desktop shell (`--safe-theme`, `-f`/`--fullscreen`, `-g`/`--game`, `-u`/`--user`) in an isolated Cargo workspace (`rust/apps/eglantine_gpui/Cargo.toml`) and launches `eden-cli` for gameplay, input, and emulation.

| Module(s) | Responsibility |
|---|---|
| `main.rs`, `shell.rs`, `views.rs`, `navigation.rs` | Startup/CLI, shell state/actions/async tasks, views/overlays, spatial focus navigation |
| `controller.rs` | Controller shell navigation + remappable Home fallback + Windows `os_text_scale()` |
| `appearance.rs`, `theme.rs`, `theme_package.rs`, `theme_tests.rs` | Theme editor, manifest/validation, atomic `.theme` archive I/O ([theme guide](../content/user/GpuiThemes.md)) |
| `library.rs`, `emulator_config.rs` | Library roots/recency, settings (`HomeKey`/`HomeButton`/`UserProfile`), one-time `qt-config.ini` import, launch overrides |
| `session.rs`, `host_tool.rs`, `content.rs` | Supervises `eden-cli` pipes (`gpui_session.{h,cpp}`) and one-shot metadata/content ops (`gpui_library.{h,cpp}`, `gpui_content.{h,cpp}`) |

- **Tests:** `cargo test --locked --manifest-path rust/apps/eglantine_gpui/Cargo.toml` (add `--no-default-features --lib` for GPUI-free unit tests); `python -I src/yuzu_cmd/tests/gpui_session.py build/bin/eden-cli.exe`.

## SDL-hosted Android prototype

`src/android/native-prototype/` is an opt-in ARM64 APK with package ID `dev.eden.eden_emulator.nativeprototype`. It launches the upstream SDL activity directly. SDL supplies the managed platform code, and the module contains no first-party Java or Kotlin app classes.

| Path | Responsibility |
|---|---|
| `src/android/native-prototype/build.gradle.kts` | Package the upstream SDL activity and the two native libraries. |
| `src/android/native-prototype/src/main/AndroidManifest.xml` | Declare the upstream SDL activity as the prototype launcher. |
| `src/android/native-prototype/.gitignore` | Exclude local Android CMake outputs. |
| `src/android/native-prototype/native/CMakeLists.txt` | Build `libmain.so` and repackage the pinned SDL archive as `libSDL3.so`. |
| `src/android/native-prototype/native/sdl_shared.c` | Anchor the shared SDL target while the linker retains the upstream JNI symbols. |
| `src/android/native-prototype/native/main.cpp` | Adapt SDL input, lifecycle, and document-picker events to Rust. Recreate the SDL window when switching between the menu renderer and the core’s Vulkan surface. |
| `src/android/native-prototype/native/core_session.{h,cpp}` | Own the C++ core session, Vulkan window, app-private data paths, and emulation lifecycle. |
| `src/android/native-prototype/native/document_vfs.{h,cpp}` | Expose one seekable Android document to the core as a read-only VFS file without copying it. |
| `src/common/android/ffmpeg.cpp` | Supply Android’s process Java VM to FFmpeg for both Android frontends. |
| `rust/apps/eglantine_android/Cargo.toml` and `Cargo.lock` | Define and lock the standalone Rust library build. |
| `rust/apps/eglantine_android/src/lib.rs` | Expose the frontend C ABI. |
| `rust/apps/eglantine_android/src/state.rs` | Own document selection, launch transitions, and frontend status messages. |

- **Build:** From `src/android/`, run `./gradlew -PnativeAndroidPrototype :native-prototype:assembleDebug` (`gradlew.bat` on Windows). The project uses the same Android SDK, NDK, and Rust target as the shipping Android build.
- **Tests:** `cargo test --manifest-path rust/apps/eglantine_android/Cargo.toml --locked --offline`.
- Pick a single game file and choose **Start emulation**. Tap with three fingers or press Escape to stop. The game pauses in the background and resumes if Android retains the same surface. If Android replaces that surface, the frontend returns to the menu so the game can be reopened.
- The prototype uses Dynarmic with fastmem disabled when Android runs the ARM64 app through x86 translation. ARM64 devices use the normal core backend defaults.
- The prototype uses separate app-private data. Library folders, settings and save migration, and physical controller support still depend on the shipping frontend.

## `frontend_common` & SDL Host (`yuzu-cmd`)

- **Discovery & formats:** `src/core/loader/loader.cpp` owns extension recognition + separate desktop/Android game/external-content lists (`GameMetadata.kt` caches via JNI; `[loader-formats]`). Native NUP publishes a program ID once ExeFS has both `main` and `main.npdm`. `src/frontend_common/game_list_{discovery.h,metadata.{h,cpp}}` provide shared metadata.
- **INI & play time:** `Config::WriteToIni()` (`config.cpp`) writes via unique temp file + flush/sync (`_commit`/`fsync`) + atomic rename (`[config_persistence]`). `PlayTimeManager` (`play_time_manager.{h,cpp}`) joins its worker before checking `modified` on destruction (`src/tests/frontend_common/play_time.cpp`).
- **Auto-pair:** `eden-cli` polls a single `Switch2AutoPair` instance ~1/s (`switch2_auto_pair=false` disables).

## Sentry Crash Reporting (`ENABLE_SENTRY=ON`, default `OFF`)

- Static Sentry Native 0.17.5 SDK (`SENTRY_BACKEND=inproc`) wired through `CMakeLists.txt`, `src/frontend_common/sentry_crash_reporter.{h,cpp}`, and `src/yuzu_cmd/yuzu.cpp` (`EDEN_NX2_CRASH_DUMP` takes precedence on Windows).
- Starts only if `SENTRY_DSN` is set (`SENTRY_ENVIRONMENT` defaults to `production`; release `eglantine@<commit>`, tag `frontend=sdl`, queue at `sentry/<frontend>`). Sends crash stacks + loaded modules only (no logs, accounts, game metadata, or attachments); upload matching PDB/ELF/dSYM via `sentry-cli debug-files upload`.
