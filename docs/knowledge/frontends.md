# `frontends` — GPUI Shell, SDL Host, `frontend_common`, and Sentry

Entry points: `rust/apps/eglantine_gpui/src/main.rs`, `src/yuzu_cmd/yuzu.cpp`, `src/frontend_common/config.cpp`.

## Targets & Responsibilities

| CMake Target | Output (`build/bin/`) | Role |
|---|---|---|
| `eglantine` (`eglantine-gpui` alias) | `eglantine` | Default GPUI desktop shell (`ENABLE_GPUI=ON`) |
| `yuzu-cmd` | `eden-cli` | SDL gameplay host for Switch and Game Boy, GPUI metadata/content backend, NX2 lab runner |

## GPUI Desktop Frontend (`rust/apps/eglantine_gpui/`)

GPUI owns the desktop shell (`--safe-theme`, `-f`/`--fullscreen`, `-g`/`--game`, `-u`/`--user`) in an isolated Cargo workspace (`rust/apps/eglantine_gpui/Cargo.toml`) and launches `eden-cli` for gameplay, input, and emulation.

| Module(s) | Responsibility |
|---|---|
| `main.rs`, `shell.rs`, `views.rs`, `navigation.rs` | Startup/CLI, shell state/actions/async tasks, views/overlays, spatial focus navigation |
| `console_navigation.rs`, `console_views.rs` | Catalog and per-console selection, filter and scroll state, console rail and family library layouts |
| `controller.rs` | Controller shell navigation (`gilrs` + `eden-cli --gpui-controller` Switch 2 BLE/USB bridge) + remappable Home fallback + Windows `os_text_scale()` |
| `appearance.rs`, `theme.rs` | GPUI theme editor and the shared theme API re-export ([theme guide](../content/user/GpuiThemes.md)) |
| `rust/crates/eglantine_library/src/lib.rs` | Stable console IDs, catalog order, and default layout families |
| `rust/crates/eglantine_theme/src/lib.rs`, `components.rs` | Theme manifest validation, component style resolution, and console palettes |
| `rust/crates/eglantine_theme/src/theme_package.rs`, `theme_tests.rs` | Atomic theme archive I/O and package compatibility tests |
| `library.rs`, `metadata.rs`, `emulator_config.rs` | Library roots/recency, offline metadata cache (`metadata-cache.json`) and local cover art matching, settings (`HomeKey`/`HomeButton`/`UserProfile`), one-time `qt-config.ini` import, launch overrides |
| `session.rs`, `host_tool.rs`, `content.rs` | Supervises `eden-cli` pipes (`gpui_session.{h,cpp}`, `gpui_controller.{h,cpp}`) and one-shot metadata/content ops (`gpui_library.{h,cpp}`, `gpui_content.{h,cpp}`) |

- **Library discovery:** The shell requests asynchronous `eden-cli --gpui-library` scans. C++ owns traversal, file validation, and metadata discovery. The bounded `emulation/classic/cartridge` probe identifies GB, GBC, and GBA library entries and extracts ROM header metadata (internal title, game code, publisher, region, version, MBC hardware, file size). The native loader extracts Switch/Switch 2 NACP metadata (title, publisher, version, languages) and tags Switch generations when program metadata confirms them, with unclassified Switch entries reachable from the combined library. GPUI updates each folder from a completed scan, retains entries for folders whose scans fail, resolves local covers (sidecars, `covers`/`boxart`/`art`/`images` subfolders, cover cache by program ID or game code, and embedded icons), caches extracted info in `metadata-cache.json` (configurable via **Auto-extract game metadata** and **Refresh metadata**), and applies played order and the saved selection.
- **In-window gameplay (Windows):** When `EGLANTINE_GPUI_PARENT_HWND` and `EGLANTINE_GPUI_VIEWPORT` are set, `eden-cli` attaches its Switch or classic SDL3 window as a `WS_CHILD` viewport inside the Eglantine GPUI window (`src/yuzu_cmd/emu_window/embedded_window.{h,cpp}`) and resizes via the `set_viewport` session command, keeping an in-window top bar with Emulation settings, Pause/Resume, Fullscreen, and Close Game.
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

## Game Boy runtime

| Path | Responsibility |
|---|---|
| `src/emulation/gba/` | First-party C++20 GBA emulator core (ARM7TDMI, PPU, APU, DMA, timers, SRAM/EEPROM/Flash/RTC, save states, rewind, cheats, LCD color correction, interframe blending, HLE/external BIOS) |
| `src/emulation/classic/cartridge.{h,cpp}` | Bounded header and checksum validation shared by discovery and launch |
| `src/emulation/classic/runtime.{h,cpp}` | Routes GBA cartridges to `gba_core` (`src/emulation/gba/`) and GB/GBC to mGBA, providing frames, 48 kHz audio, battery saves, save states, rewind, cheats, and video enhancements |
| `src/emulation/classic/battery_save.{h,cpp}` | Size-checked battery reads and atomic replacement of save files |
| `src/emulation/classic/CMakeLists.txt` | Independent cartridge and runtime targets |
| `src/yuzu_cmd/classic_session.{h,cpp}` | Desktop SDL window, audio, input, hotkeys (`F1`–`F3`, `F5`–`F9`, `Tab`), frame pacing, GPUI pipe events, BIOS/state/save paths |
| `externals/mgba/CMakeLists.txt`, `cpmfile.json` | Pinned mGBA build configuration, archive digest, and notice staging |
| `dist/classic_runtime_sources.txt` | Sources and rebuild instructions shipped beside the upstream license notices |

- Desktop builds enable `ENABLE_CLASSIC_RUNTIME`. Use `-DENABLE_CLASSIC_RUNTIME=OFF` for a desktop build without embedded Game Boy gameplay. `eden-cli` sends raw GB, GBC, and GBA files to the classic session before Switch initialization.
- Saves use `<EdenDir>/classic/saves/<profile>/<system>/<rom-sha256>.sav`. The host snapshots battery state during play and on pause, then flushes final battery state after stopping. ROM files are opened read-only.
- Windows archives include `licenses/classic` from the staged binary folder. Linux tarballs include the same directory, and AppImages and installs use `share/licenses/eglantine/classic`. The sources file records how to rebuild with a replacement mGBA checkout.
- Tests use generated cartridge fixtures in `src/tests/emulation` and the GPUI host protocol harness. Android remains on its native Switch session path.

## Sentry Crash Reporting (`ENABLE_SENTRY=ON`, default `OFF`)

- Static Sentry Native 0.17.5 SDK (`SENTRY_BACKEND=inproc`) wired through `CMakeLists.txt`, `src/frontend_common/sentry_crash_reporter.{h,cpp}`, and `src/yuzu_cmd/yuzu.cpp` (`EDEN_NX2_CRASH_DUMP` takes precedence on Windows).
- Starts only if `SENTRY_DSN` is set (`SENTRY_ENVIRONMENT` defaults to `production`; release `eglantine@<commit>`, tag `frontend=sdl`, queue at `sentry/<frontend>`). Sends crash stacks + loaded modules only (no logs, accounts, game metadata, or attachments); upload matching PDB/ELF/dSYM via `sentry-cli debug-files upload`.
