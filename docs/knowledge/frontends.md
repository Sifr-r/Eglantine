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

- **Library discovery:** The shell requests asynchronous `eden-cli --gpui-library` scans. C++ owns traversal, file validation, and metadata discovery. GPUI updates each folder from a completed scan, retains entries for folders whose scans fail, and applies sidecar covers, played order, and the saved selection.
- **Tests:** `cargo test --locked --manifest-path rust/apps/eglantine_gpui/Cargo.toml` (add `--no-default-features --lib` for GPUI-free unit tests); `python -I src/yuzu_cmd/tests/gpui_session.py build/bin/eden-cli.exe`.

## Android Frontend (SDL + Rust)

`src/android/app/` builds the shipping ARM64 APK with package ID `dev.eden.eden_emulator`. It launches the upstream SDL activity directly. SDL supplies the managed platform code, and the module contains zero first-party Kotlin or Java app classes (Gradle scripts also use Groovy).

| Path | Responsibility |
|---|---|
| `src/android/app/build.gradle` | Package the upstream SDL activity and the two native libraries. |
| `src/android/app/src/main/AndroidManifest.xml` | Declare the upstream SDL activity as the launcher. |
| `src/android/app/.gitignore` | Exclude local Android CMake outputs. |
| `src/android/app/native/CMakeLists.txt` | Build `libmain.so` and repackage the pinned SDL archive as `libSDL3.so`. |
| `src/android/app/native/sdl_shared.c` | Anchor the shared SDL target while the linker retains the upstream JNI symbols. |
| `src/android/app/native/main.cpp` | Adapt SDL input, gamepads/sensors, lifecycle, and file/folder picker events to Rust, and replay Rust draw commands through SDL. Recreate the SDL window when switching between the menu renderer and the core’s Vulkan surface. |
| `src/android/app/native/core_session.{h,cpp}` | Own the C++ core session, in-place Vulkan surface recreation, `adrenotools` custom Vulkan driver loading, `VirtualGamepad` routing, unified Android storage (`SDL_GetAndroidExternalStoragePath` with internal fallback), `.keys`/`.so` installation, and emulation lifecycle. |
| `src/android/app/native/native_config.{h,cpp}` | Implement `Config` (`frontend_common`) INI persistence for Android, overlay, and GPU driver settings without JNI. |
| `src/android/app/native/document_vfs.{h,cpp}` | Expose seekable Android documents to the core as read-only VFS files and scan SAF folder trees (`DocumentsContract`) with `Loader` metadata inspection. |
| `src/common/android/android_settings.{h,cpp}` | Shared Android settings schema compiled into `common`. |
| `src/common/android/ffmpeg.cpp` | Supply Android’s process Java VM to FFmpeg. |
| `rust/apps/eglantine_android/Cargo.toml` and `Cargo.lock` | Define and lock the standalone Rust library build. |
| `rust/apps/eglantine_android/include/eglantine_android.h` | Declare the C ABI (`EglAndroidDrawCmd`, `EglAndroidGameEntry`, `EglAndroidOverlayEvent`, `EglAndroidSettings`, actions, keys, viewport, lifecycle) shared with `main.cpp`. |
| `rust/apps/eglantine_android/src/lib.rs` | Expose the frontend C ABI. |
| `rust/apps/eglantine_android/src/library.rs` | Persist scanned SAF folders and discovered games (`android_library.tsv`). |
| `rust/apps/eglantine_android/src/overlay.rs` | Own multi-touch virtual gamepad controls, controller auto-hide, and `android_overlay.tsv` persistence. |
| `rust/apps/eglantine_android/src/settings.rs` | Own settings snapshot normalization, toggle/cycle transitions, and display labels. |
| `rust/apps/eglantine_android/src/state.rs` | Own document/folder/keys/driver selection, view switching, launch transitions, key readiness, overlay state, and frontend status messages. |
| `rust/apps/eglantine_android/src/ui.rs` | Own logical viewport layout, library/settings views, pointer/key hit-testing, text wrapping, and draw-list generation. |

- **Build:** From `src/android/`, run `./gradlew :app:assembleMainlineRelWithDebInfo` (`gradlew.bat` on Windows).
- **Tests:** `cargo test --manifest-path rust/apps/eglantine_android/Cargo.toml --locked --offline`.
- Pick a single game file or add a game folder (and optionally install `prod.keys` or a custom Turnip `.so` driver under **Settings**) and choose **Start**. Physical gamepads and sensors route via SDL3 (`VirtualGamepad`), touch controls auto-hide when a controller is active, and backgrounding recreates the Vulkan surface in-place on resume. Tap the top-center pause zone, tap with three fingers, or press Escape/Back to stop.
- Uses Dynarmic with fastmem disabled when Android runs the ARM64 app through x86 translation. ARM64 devices use the normal core backend defaults.
- Shares the app-external storage layout (`keys/`, `nand/`, `sdmc/`, `config/`); Android uses `Core::System`'s built-in default C++ applets (`SetDefaultAppletsIfMissing()`, with controller configuration handled in-app and no custom software keyboard, web browser, or NFC/amiibo cabinet applets).

## `frontend_common` & SDL Host (`yuzu-cmd`)

- **Discovery & formats:** `src/core/loader/loader.cpp` owns extension recognition + separate desktop/Android game/external-content lists (`document_vfs.cpp` filters SAF scans with `Loader::GetFileExtensions(AndroidGame)`; `[loader-formats]`). Native NUP publishes a program ID once ExeFS has both `main` and `main.npdm`. `src/frontend_common/game_list_{discovery.h,metadata.{h,cpp}}` provide shared metadata.
- **INI & play time:** `Config::WriteToIni()` (`config.cpp`) writes via unique temp file + flush/sync (`_commit`/`fsync`) + atomic rename (`[config_persistence]`). `PlayTimeManager` (`play_time_manager.{h,cpp}`) joins its worker before checking `modified` on destruction (`src/tests/frontend_common/play_time.cpp`).
- **Auto-pair:** `eden-cli` polls a single `Switch2AutoPair` instance ~1/s (`switch2_auto_pair=false` disables).

## Sentry Crash Reporting (`ENABLE_SENTRY=ON`, default `OFF`)

- Static Sentry Native 0.17.5 SDK (`SENTRY_BACKEND=inproc`) wired through `CMakeLists.txt`, `src/frontend_common/sentry_crash_reporter.{h,cpp}`, and `src/yuzu_cmd/yuzu.cpp` (`EDEN_NX2_CRASH_DUMP` takes precedence on Windows).
- Starts only if `SENTRY_DSN` is set (`SENTRY_ENVIRONMENT` defaults to `production`; release `eglantine@<commit>`, tag `frontend=sdl`, queue at `sentry/<frontend>`). Sends crash stacks + loaded modules only (no logs, accounts, game metadata, or attachments); upload matching PDB/ELF/dSYM via `sentry-cli debug-files upload`.
