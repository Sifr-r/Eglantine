# ARCHITECTURE.md — Eglantine

Ledger of the core domains, their public contracts and their build dependencies.

Baseline generated 2026-11-16 against `upstream/nx2` = `cf6cfe9caa` (v0.1.0-rc4).

---

## 1. What this project is

Eglantine is a Switch / Switch 2 emulator forked from Eden, which descends from Yuzu.
The `nx2` branch adds native Switch 2 (Ampere / NVN2) support on top of the Switch 1 (Maxwell)
code base. The two coexist in one tree rather than in separate ports: a title is "native"
when its GPU runs the Ampere macro engine (`Maxwell3D::GetNativeMacroEngine().native_turing`).

## 2. Domains and build dependencies

The table lists domain ownership. The `CMakeLists.txt` files in each domain define target
dependencies. For example, `core` and `video_core` link to each other, and `video_core` links
to `shader_recompiler` for shader translation.

| # | Domain | Path | Responsibility |
|---|---|---|---|
| 1 | `dynarmic` | `src/dynarmic` | JIT recompiler for the guest ARM CPU |
| 2 | `common` | `src/common` | Types, settings, filesystem, logging, math, path helpers |
| 3 | `network` | `src/network` | Multiplayer rooms, room clients and packet serialization |
| 4 | `core` | `src/core` | The emulated machine: CPU, memory, services (HLE), the system loop |
| 5 | `audio_core` | `src/audio_core` | Audio output and mixing |
| 6 | `video_core` | `src/video_core` | GPU state, memory manager, texture cache, shader lowering, renderers |
| 7 | `hid_core` | `src/hid_core` | Controller/console HID protocols |
| 8 | `input_common` | `src/input_common` | Input device drivers, maps them onto emulated controllers |
| 9 | `frontend_common` | `src/frontend_common` | Frontend-shared config, paths, play time, firmware |
| 10 | `shader_recompiler` | `src/shader_recompiler` | Maxwell/Ampere shader translation to SPIR-V |
| 11 | `mcp` | `src/mcp` | Built-in MCP server for AI agents (tools over the emulator) |
| 12 | `yuzu_cmd` | `src/yuzu_cmd` | SDL frontend, built as `yuzu-cmd` with executable name `eden-cli` |
| 13 | `qt_common` | `src/qt_common` | Qt-shared helpers (game list, paths, per-game settings) |
| 14 | `yuzu` | `src/yuzu` | Qt frontend, built as `yuzu` with executable name `eglantine` |
| 15 | `updater` | `src/updater` | The self-update helper |
| 16 | `web_service` | `src/web_service` | Telemetry and other web services |
| 17 | `android` | `src/android` | The Android port (JNI glue over `core`) |
| — | `tests` | `src/tests` | Catch2 unit tests; links against the layers it tests |

`externals/` holds third-party code (FFmpeg, SDL3, Vulkan headers, libusb, FSR, …), mostly
fetched through CPM into `.cache/cpm/` at configure time. `dist/` holds packaging assets.
`tools/nx2/` holds the Switch 2 debugging toolchain; `tools/windows/` holds the Vulkan SDK
bootstrap.

The standalone Rust Ampere prototype under `rust/crates/ampere_core/` exposes its lowering
and data-format operations through `rust/include/eglantine_ampere.h`. The emulator still
uses the C++ shader recompiler. The [polyglot migration guide](POLYGLOT_IMPLEMENTATION_PLAN.md)
tracks the proposed integration and its parity checks.

## 3. Core domain contracts

These are the seams that matter when changing behaviour. Each is a real interface, not a
convention.

### `core` — the machine

* `Core::System` owns the CPU, memory and the service dispatcher. Everything else reads the
  machine through it.
* `Core::LaunchParams` / `ParseLaunchParams` / `ApplyLaunchParams` (`src/core/launch_params.{h,cpp}`)
  parse the command line (`-c` config, `-g` game, `-u` user, `-d` gdbstub, `--mcp`, NUP
  pack/unpack/edit options, Windows `argv_w` UTF-16 args) and apply launch settings; covered by
  `src/tests/core/launch_params.cpp`.
* Services live in `src/core/hle/service/`. The Switch 2 additions are `nvdrv/` (GPU
  device), `nx2_gsv.h` (graphics server, frame capture) and `nx2_stub_device.h`.
* `Core::g_nx2_present_count` is the frame counter the NX2 diagnostics and key scripts use.
* The intrusive kernel object registry unlinks slab-backed objects before their storage is
  released. Initialization independently determines whether resource finalization runs.
  `src/tests/core/process_load.cpp` exercises failed application loads and slab-slot reuse.
* **The kernel object list is emptied while the guest RAM is still alive.** The registered
  objects (threads, sessions, shared memory) live in the guest RAM arena, and `System::Impl`
  frees that arena before it destroys the kernel.
  `KAutoObjectWithListContainer::Finalize()` unlinks every registered object and refuses
  further registrations; `KernelCore::ClearObjectList()` runs it from `System::Impl`'s
  destructor, so a system that is destroyed without a shutdown empties the list as well.
  A container that walked its list after the arena was freed read freed memory.
* **Shutdown order.** `System::Impl::ShutdownMainProcess()` keeps the timing thread paused for
  the whole teardown (`core_timing.SyncPause(true)`), and resets the cheat engine before the
  kernel releases the guest processes. `CoreTiming::Advance()` copies the event it is about to
  run into locals, so a callback that unschedules the event cannot leave a dangling
  reference. The cheat callbacks resolve the guest process through the kernel on every use
  instead of caching a pointer the kernel released.
* **NX2 present shutdown.** The emulation stop token wakes the guest fence event used by
  `INx2Vi2Session`, so the applet service can exit while guest timing is paused. The canceled
  present exits the handler before queuing a composite or a buffer release.
* The NUP metadata editor in `src/core/file_sys/nup_writer.cpp` keeps stored file extents at
  their original offsets when metadata must move to the end of the archive.
* **Save data ownership.** `FileSys::SaveDataFactory` (`src/core/file_sys/savedata_factory.cpp`)
  owns the on-disk save data layout; `Service::FileSystem::SaveDataController`
  (`src/core/hle/service/filesystem/save_data_controller.cpp`) is the only thing the HLE layer
  talks to. The factory is the single place that knows the layout — system saves live in
  `save/<save id>/<user id>`, account and device saves in `save/0/<user id>/<title id>`, cache
  saves in `save/cache/<title id>` (or `save/cache/<title id>_<index>` when indexed), temporary saves in `<save id>/<user id>/<title id>` — and it exposes
  it through `Enumerate`, `Delete`, `DeleteMatching`, `DeleteCacheStorage`, `GetFreeSpace`,
  `GetCommitId`, `OpenMetaFile` and `QueryTotalSize`. The service layer must not reach into the
  layout itself: `ISaveDataInfoReader` delegates save enumeration to
  `SaveDataController::EnumerateSaveData` and the `fsp-srv` handlers go through the controller.
  The save data factory derives stable IDs for non-system saves from their attributes and
  resolves IDs for deletion and commit queries only when the match is unambiguous.
  A title id of zero in an attribute means "the title of the calling process", resolved by
  `SaveDataFactory::GetFullPath`.
* **Deleting save data is idempotent.** Deleting a save that does not exist returns
  `FileSys::ResultTargetNotFound` and changes nothing on disk; deleting one that does removes
  the directory tree. A delete never removes the space root and never escapes the space root.
* **The `fsp-srv` command surface.** `Service::FileSystem::FspSrv`
  (`src/core/hle/service/filesystem/fsp/fsp_srv.cpp`) is the only place that answers the
  filesystem proxy commands, and it answers them from `SaveDataController` — never from the
  layout directly. Eighteen commands are implemented: 11 `OpenBisFileSystem`, 12
  `OpenBisStorage`, 13 `InvalidateBisCache`, 21 `DeleteSaveDataFileSystem`, 24
  `RegisterSaveDataFileSystemAtomicDeletion`, 25 `DeleteSaveDataFileSystemBySaveDataSpaceId`,
  28 `DeleteSaveDataFileSystemBySaveDataAttribute`, 33 `DeleteCacheStorage`, 60
  `OpenSaveDataInfoReader`, 68 `OpenSaveDataInfoReaderBySaveDataFilter`, 80
  `OpenSaveDataMetaFile`, 100 `OpenImageDirectoryFileSystem`, 110
  `OpenContentStorageFileSystem`, 601 `QuerySaveDataTotalSize`, 612
  `GetFreeSpaceSizeForSaveData`, 615 `QuerySaveDataInternalStorageTotalSize`, 616
  `GetSaveDataCommitId` and 1001 `SetSaveDataSize`. Two of them deliberately do less than the
  console: `InvalidateBisCache` is a no-op because this tree has no partition cache to
  invalidate, and `RegisterSaveDataFileSystemAtomicDeletion` (24) deletes immediately instead
  of deferring to the next boot. Both are commented at their handler. The command ids follow
  `ISaveDataInfoReader` and the Atmosphère headers, and a command that is not implemented still
  reports a typed result rather than a silent success.
* **GPU callback and session lifetime.** A callback queued on the GPU thread (`GPU().RunInOrder`,
  `GPU().MapInOrder`, a rasterizer fence) must not capture the session that queued it, and
  must not capture a raw `Kernel::KEvent*` or a raw `Memory*`. Sessions keep shared callback
  state with an `alive` flag (`INx2SyncptSession::m_shared`, `INx2Vi2Session::m_frame_waiter`,
  `INx2GraphicsSystemSession::m_syncpt_state`), destructors unregister listeners/mirrors and
  drain queued GPU work before closing events or releasing process refs, and callbacks capture
  the shared state rather than `this`. `INx2GraphicsSystemSession` subscribes via keyed
  `SyncpointManager::RegisterGuestMirror`/`UnregisterGuestMirror` and skips timer syncpoint
  scans when neither wait bits nor summary changed. `nvhost_as_gpu::~nvhost_as_gpu()` removes
  its `Nx2GpuMemoryRegistry` listener before `DrainGpuCallbacks()`, and `Nx2Map` /
  `Nx2BackSparse` / `Nx2SparseTile` read session state through `Container::WithSession` under
  `Nx2GpuMemoryRegistry::mutex -> nvhost_as_gpu::mutex -> Container::session_guard ->
  Nx2GpuMemoryRegistry::device_mutex -> smmu` (never calling registry mutations inside
  `WithSession`). `TryNvdrvSyncpoints()` guards `INx2Vi2Session` / `INx2VideoSession` against
  null NVDRV during shutdown (`~INx2Vi2Session()` uses wrap-safe `s32` syncpoint delta),
  `INx2GpuSession` uses non-recursive `std::mutex`/`std::condition_variable` with eager
  `AllocateGpuOnlyBacking` outside `m_region_mutex`, and `DrainGpuCallbacks()` /
  `ReleaseBackstop()` honor `Core::System::GetEmulationStopToken()`.
* **Guest-controlled sizes.** A count or a range that comes from the guest is bounded and
  range-checked before it is used to allocate, to loop, or to compute an address. The
  checks live next to the service that reads them. `nx2:gpu` command 214 validates page
  alignment, the advertised GPU address range, and an 8 GiB per-region limit before
  updating mappings or allocating backing metadata.
* **NVMap pin failures.** `NvMap::PinHandle` rejects handles whose session has closed
  before allocating a new mapping. GPU and display callers check the pin result before
  using its address or publishing a preallocated buffer.

### `common` — host memory

* **Windows on-demand DRAM commit (`src/common/host_memory.{h,cpp}`).** `Common::HostMemory`
  reserves backing sections with `CreateFileMapping2(..., SEC_RESERVE)` and fallback buffers with
  `VirtualAlloc(..., MEM_RESERVE)` instead of committing the full guest DRAM (up to ~14 GiB) up
  front. First read/write faults into registered `HostMemory` regions commit 64 KiB granules via a
  vectored exception handler (`HostMemoryFaultHandler`, `SRWLOCK`-synchronized), while unmapped
  placeholders (`MEM_PRIVATE`) and `PAGE_READONLY`/`PAGE_NOACCESS` faults chain to Dynarmic SEH.
* **`Protect`, zeroing, external import, and cleanup contracts.** `HostMemory::Impl::Protect`
  leaves untouched `MEM_RESERVE` pages uncommitted when restoring `PAGE_READWRITE`, and explicitly
  commits views with target `PAGE_READONLY`/`PAGE_NOACCESS` flags so backing commits never drop
  view traps. `ClearBackingRegion(..., 0)` skips uncommitted pages, `Common::EnsureHostMemoryCommitted`
  pre-commits ranges passed to external drivers (`Vulkan::HostImporter`), `Release()` is idempotent
  on partial `Init()` failure, move operations transfer/clear fallback ownership, and Windows fallback
  teardown calls `VirtualFree(backing_base, 0, MEM_RELEASE)`.

### `video_core` — the GPU

* `VideoCommon::Maxwell3D` is the register file. `GetNativeMacroEngine().native_turing`
  selects the Switch 2 path.
* `VideoCore::MemoryManager` maps guest memory, including the sparse regions Switch 2
  titles use (`Tegra::Nx2Sparse*`).
* `VideoCore::TextureCache` owns every host image. `ImageBase` carries the per-image
  bookkeeping; `first_written_frame`, `last_written_frame` and `cleared_frame` are the
  frame counters the render-target predicates read (`RedrawnEveryFrame`,
  `HasRecentPriorFrameOutput`).
* `VideoCore::PipelineCache` (`src/video_core/renderer_vulkan/vk_pipeline_cache.cpp`) owns
  the graphics pipeline cache, its disk format (`CACHE_VERSION`, currently 34) and the
  asynchronous shader build. `src/video_core/renderer_vulkan/draw_skip_policy.h`
  (`EvaluateDrawSkip`) defines the single skip contract shared by `BuiltPipeline`,
  `CanSkipDraw` (async translation), `DeferRuntimeBuild` (build budget), and
  `CanSkipForDiskReplay` / `in_flight_disk_replays` (waiting on in-flight disk replays for
  unskippable draws). Geometry consistency preserves depth-only draws that count occlusion
  samples or may write shader memory. Non-indexed draws without vertex stream 0 are excluded
  from geometry matching.
* Renderers: `renderer_vulkan/` (the default), `renderer_opengl/`, `renderer_null/`.
* `src/video_core/gpu_split.h` separates front-thread ring bookkeeping and the back-thread
  engine pointer at 128-byte boundaries. The shared atomics retain their own alignment.
* `VideoCore::Nx2Perf` (`src/video_core/nx2_perf.h`) is the switchboard for the NX2
  optimisations. Every switch is **on by default**; `EDEN_NX2_PERF_OPT=0` turns all of
  them off, `EDEN_NX2_PERF_SAFE=1` turns off the experimental set (`DefaultOffMask`),
  `EDEN_NX2_PERF_ON=`/`EDEN_NX2_PERF_OFF=` pick individual ones, and
  `EDEN_NX2_PERF_VERIFY=1` computes each optimised answer the old way too and logs the
  differences.
* **Page table locking.** `MemoryManager` owns the GPU page table, and CPU service threads
  write it while GPU threads read it. One mutex, `page_table_mutex`, covers
  `entries`, `page_table`, `big_entries`, `big_page_table_dev` and
  `big_page_continuous`. The primitives (`GetEntry`, `SetEntry`,
  `IsBigPageContinuous`, `SetBigPageContinuous`) are lock-free and are called with the
  lock held; they are not public API. `FinishPageTableOpLocked` acquires
  `page_table_mutex -> runs_guard -> guard` to update `UpdateContinuousRunsLocked`,
  `kind_map`, and a single `map_serial` increment per page-table operation before releasing
  all locks prior to calling `rasterizer->ModifyGPUMemory`. No `MemoryManager` lock is ever
  held across a rasterizer, cache or callback call — the GPU thread holds the cache
  mutexes and then reads the table, so the only safe order is `cache mutexes ->
  page_table_mutex -> runs_guard -> guard`. A walk that has to call into the rasterizer
  collects its segments under the lock and acts on them after releasing it. A walk merges
  the segments that cover one host-contiguous range after it released the lock, so the list
  grows with the number of mappings in the range and not with its size in pages.
* **Shared state between the GPU and the CPU.** `GpuSplit::active` is an atomic, not a plain
  `bool`: the front thread (`GpuSplit::OnGpuThread()`), the back thread
  (`GpuSplit::OnBackThread()`) and callers asking whether the split is running read it across
  threads. `MemoryManager::RunRasterizerWork` batches `{DAddr, u64}` regions from
  `WriteBlockImpl` and `InvalidateRegion` by value onto the split back thread (`DeferFunction`
  / `Drain` on `OnGpuThread()`, `GPU::RunInOrderSync` on other threads), and
  `ThreadManager::FlushAndInvalidateRegion` delegates invalidation to `InvalidateRegion`.
  `DeviceMemoryManager::CanTrapCpuReads()` (updated in `RegisterProcess`/`UnregisterProcess`
  via `MaxwellDeviceMethods::HasFastmemArena` and `Memory::HasFastmemArena`) gates
  `BufferCache::AsyncBufferDownloads()` so NCE, non-fastmem, and reactive-flushing paths
  fall back to buffer downloads. `Tegra::g_nx2_sparse_backing_mutex` (with in-flight handler
  drain in `Nx2SetSparseBacking`) and `QueryCacheLegacy::mutex` use non-recursive
  `std::mutex`. The page-table walk does not share its scratch buffers between calls
  (`MemoryManager::Unmap` and `MemoryManager::FlushCaching` collect their ranges in local
  vectors), so two threads that unmap or flush at the same time do not overwrite each other's
  list.
* **ETC2 and EAC decoding lives in `src/video_core/textures/etc2.cpp`.** `Tegra::Texture::ETC2`
  decodes the Switch's ETC2 and EAC block formats on the host when the GPU cannot do it
  natively. The decoder follows the Khronos data format specification, not a port of another
  emulator: the blocks are read big endian, the five block modes (individual, differential, T,
  H, planar) are selected by the specification's own rules, and the modifier tables are the
  ones the specification tabulates — including the two *separate* punchthrough tables, where
  the non-opaque variant zeroes the two inner entries. The RGBA alpha channel and the EAC
  channels use different reconstruction, so they are decoded by different functions:
  `clamp255(base + modifier * multiplier)` for the RGBA alpha and
  `clamp(base * 8 + 4 + modifier * multiplier * 8)` for EAC, where the EAC result keeps eleven
  bits. `ConvertImage` in `src/video_core/texture_cache/util.cpp` calls the decoder when the
  device reports no optimal ETC2 support; the decoder itself owns no GPU state, so it is
  covered by the unit tests in `src/tests/video_core/etc2.cpp` alone.


### `network` — the multiplayer room

`src/network` implements the ENet room (host side, `room.cpp`) and the room member
(client side, `room_member.cpp`). Both sides of the protocol live in this repository, so
a change to the wire format has to land in both.

* **Peer-controlled reads are bounded before they allocate.** `Packet::Read` checks the
  bytes it still has before it resizes a vector, before it copies a string, and before it
  writes into a caller buffer (`Read(char*, size)`). A truncated packet invalidates the
  `Packet`; it never resizes to a count the peer chose.
* **A peer has to be a member.** Every message except the join request is ignored unless
  the sender is in `members`; moderation rights come from the authenticated user data
  (`user_data.moderator`) or from the authenticated username matching the host's, never
  from the nickname, which the peer chooses.
* **The room owns the relayed source address.** A relayed proxy or LDN packet has its
  source address (bytes 2-5 of the packet) rewritten to the fake IP the room assigned to
  the sender, so a member cannot make traffic appear to come from another member. The
  relayed payload is capped before it is copied, and only members ever receive it.


* `Shader::Ampere::LowerToMaxwell` turns a native Switch 2 program into a Maxwell one.
* `Shader::Ampere::ProgramCaptureState` captures the guest's shader instructions.

### `input_common` — controllers

* `InputCommon::InputSubsystem` owns the devices; `GetKeyboard()` is the path the NX2 key
  scripts use, and `GetSwitch2()` exposes `InputCommon::Switch2Controllers`.
* `src/input_common/main.{h,cpp}` compares Player 1's keyboard bindings with the frontend's
  factory defaults. `src/common/param_package.h` compares binding keys and values independently
  of their serialized order.
* `src/input_common/drivers/sdl_driver.cpp` owns SDL joystick and gamepad handles. It checks
  native driver preference before opening a gamepad handle and ignores removal events for
  joysticks SDL has no open handle for.
* `src/yuzu/util/controller_navigation.cpp` converts Player 1 and handheld button and stick
  input into Qt navigation key events. It reads each controller's input state independently.
* Native Nintendo Switch 2 controller support (`src/input_common/drivers/switch2.{h,cpp}`,
  `src/input_common/helpers/switch2_protocol/`):
  * `switch2_types.{h,cpp}` — BLE `0x0553` advertisement parser, 12-bit stick unpacking and
    calibration, 60+ byte input report parser, 40-bit LRA vibration encoder, command/memory
    builders, IMU converter, `GyroMouseProcessor`, and `OpticalMouseProcessor`.
  * `switch2_transport.h` — `ISwitch2Transport` and `ISwitch2Scanner` interfaces plus
    `CreateBleScanner()` / `CreateUsbScanner()` factories, implemented by
    `switch2_ble_win.cpp` (Windows WinRT BLE GATT + non-Windows stub) and `switch2_usb.cpp`
    (`libusb` wired transport). Dependency direction: `yuzu` / `Switch2Controllers` ->
    `Switch2Driver` -> `ISwitch2Transport` <- transport backends.
  * `switch2_driver.{h,cpp}` — `Switch2Driver` protocol state machine, calibration/LED/rumble
    commands, and mouse output routing.
  * `drivers/switch2.{h,cpp}` — `Switch2Controllers` (`InputEngine` `"switch2"`): 8 player slots,
    Joy-Con 2 auto-combine, runtime `SplitSlot` / `MergeSlots` / `CalibrateGyro`, and
    mouse interpolation thread; `drivers/sdl_driver.cpp` skips Switch 2 PIDs when enabled.
    `GetDeviceBindings()` returns every connected controller together with the parameter
    package that addresses it; `GetSlotDeviceId()` returns the controller's stable identity
    (its serial number, or the transport address). The identity travels in the parameter
    package under the `device_id` key, so a controller keeps its player assignment when the
    driver re-sorts its slots.
  * `drivers/switch2_auto_pair.{h,cpp}` — `Switch2AutoPair`: binds the controllers the driver
    found to emulated player slots. `ApplyToPlayers()` is the pure policy (a new controller
    can claim Player 1 when it uses the factory keyboard bindings. Otherwise it claims the
    lowest unconnected slot that no other Switch 2 controller owns, reusing a disconnected
    Switch 2 slot before falling back to an active non-Switch 2 slot. A saved slot that already
    points at the controller is reused; a controller that moved to another driver slot is
    re-bound; a user remap and explicitly cleared bindings survive while the controller type
    stays the same). `ApplyDevices()` is the same policy plus the state of the controllers
    between the passes: a controller that is missing for `MissingPassesBeforeDisconnect`
    passes in a row has its player slot marked as disconnected. `Apply()` runs the policy
    against the driver, honours `Settings::values.switch2_auto_pair`, writes
    `Settings::values.players` (active and global) and reports the slots that changed.
    Reloading the emulated controllers and persisting the configuration belong to the caller.
    Qt saves the global player bindings without changing the active per-game selection:
    `src/yuzu/main_window.cpp` polls it once a second and checks again before opening a
    single-player controller selector. `src/yuzu_cmd/yuzu.cpp` polls from
    `CheckSwitch2AutoPair()`. Both frontends keep **one** `Switch2AutoPair` instance for the
    whole session: the state of the controllers is
    what makes a disconnect visible, and a fresh instance per poll would never report one.
  * **The wire protocol follows the reference implementation.** The feature flags, the common
    report, the battery and IMU layouts and the rumble frames follow
    `linux-switch2/PROTOCOL.md`. The USB transport strips the report id before the parser
    sees the frame, the BLE transport announces the startup feature mask before the normal
    one, and the feature mask of a controller is
    `Buttons | AnalogSticks | Motion | Rumble` (plus `Mouse` for a Joy-Con). The parser is
    fed the native report layout; the driver never sees a report id.
  * **A controller that is gone is unregistered.** The transport fires its disconnect callback
    exactly once, the driver turns it into `Switch2Controllers::UnregisterDriver`, and a
    controller that is still in a slot but no longer connected is never used as a Joy-Con
    combine partner.
  * Public config & UI boundaries: `src/common/settings.{h,cpp}` (`Settings::Values` out-of-line
    ctor/dtor contract + Switch 2 settings) and `src/common/settings_enums.h`;
    `src/yuzu/applets/qt_controller.{h,cpp,ui}` (Controller Applet Switch 2 bar);
    `src/yuzu/configuration/configure_input_advanced.{cpp,ui}` (Advanced Input Switch 2 panel);
    unit tests in `src/tests/input_common/switch2_protocol.cpp` and
    `src/tests/input_common/switch2_auto_pair.cpp`.

### `mcp` — the agent interface

* With `ENABLE_MCP_SERVER`, `src/mcp/tools/` supplies emulator tools for both desktop
  frontends. `src/yuzu_cmd/mcp_host_cli.cpp` and `src/yuzu/mcp_host_qt.cpp` integrate the
  server into the SDL and Qt frontends, respectively.

## 4. Frontends

Two frontends share `frontend_common`:

| CMake target | Executable in the build's `bin/` directory | Frontend |
| --- | --- | --- |
| `yuzu-cmd` | `eden-cli` | SDL frontend used by the NX2 lab for unattended runs |
| `yuzu` | `eglantine` | Qt frontend |

The executables have an `.exe` suffix on Windows.

Both read `qt-config.ini` / `sdl2-config.ini` from the config directory. The Qt file
usually binds a gamepad; the SDL file binds the keyboard by default.

`src/qt_common/config/qt_config.cpp` persists shortcuts under their group and action names.
An empty binding can be an explicit override of the default. Android's game scanner and
launch-by-URI handler share the extension allowlist in `model/Game.kt`. The native NUP
loader publishes a program ID once the selected ExeFS contains both `main` and `main.npdm`.

Qt's legacy-data migration runs before frontend initialization. `src/yuzu/migration_worker.cpp`
validates source and destination directories, transfers data, and rewrites local storage paths
in copied or moved Qt configuration files. `src/yuzu/user_data_migration.cpp` waits for the
worker's result before startup proceeds.

The Qt frontend opens one modeless **Help → Diagnostics** window through `MainWindow`.
While visible, it refreshes **Actionable issues** every two seconds. The view groups repeated
critical errors, native shader rejections, and Vulkan graphics pipeline failures from the
recent emulator log. The 256 KiB tail bounds the results. The live view and manual log report
each have a clipboard action.

| Path | Responsibility |
| --- | --- |
| `src/yuzu/diagnostics_log.h` | Define the snapshot returned to the diagnostics dialog. |
| `src/yuzu/diagnostics_log.cpp` | Read bounded log excerpts, extract renderer details, and group actionable failures. |
| `src/yuzu/diagnostics_dialog.h` | Declare the modeless window and its refresh and copy actions. |
| `src/yuzu/diagnostics_dialog.cpp` | Refresh actionable issues while visible and display and copy diagnostic reports. |
| `ignore/diagnostics-dialog-plan.md` | Record the research review scope and acceptance checks for the dialog. |
| `ignore/live-diagnostics-plan.md` | Record the live issue filter, window behavior, and acceptance checks. |

Both frontends poll `InputCommon::Switch2::Switch2AutoPair` about once a second, so a
native Switch 2 controller that is plugged in or paired while the app runs is bound to a
player slot without the user configuring it by hand. The first unassigned controller can
take Player 1 from the factory keyboard mapping. Other assignments prefer the lowest
unconnected slot, reusing a disconnected Switch 2 slot before replacing an active
non-Switch 2 mapping. Set `switch2_auto_pair=false` in the config file to keep the manual
behaviour.

## 5. The Switch 2 debugging toolchain (`tools/nx2/`)

| Tool | Purpose |
|---|---|
| `lab_ctl.py` | Drives a hidden `eden-cli` run: launch, press, capture, state, ablate, bisect. POSIX process handling. |
| `shader_lab.py` | Browser UI over the shader catalogue. |
| `catalog.py` | The per-game shader catalogue. `status` before a commit, `merge` to fold the local copy into the repo. |
| `debug_mcp.py` | Adapter for the live debug server (`EDEN_NX2_DEBUG_SERVER`). |
| `nvd_cubin.py` | Disassembles a dumped Ampere program. |

The skills that describe how to use them live in `.claude/skills/`
(`nx2-lab-automation`, `nx2-shader-triage`, `nx2-debug-server`).

### Local lab and fixture storage

Entries under `ignore/` describe one local archive. Their availability depends on the checkout.

| Path | Responsibility |
| --- | --- |
| `ignore/.lab/` | Local Windows bisect harness, tool binaries, configurations and run artifacts. Run `ignore\.lab\build.cmd tests` from the repository root to reconfigure the existing bisect build for the relocated shader compiler. |
| `ignore/.lab/nx2_gameplay_baseline.py` | Local Windows runner for bounded gameplay checks with separate portable profiles and cold or warm emulator shader caches. |
| `ignore/BISECT_RC3_RC4.md` | Results and reproduction steps for the local rc3–rc4 bisect. |
| `ignore/UPSTREAM_WORKLIST.md` | Local diagnostics and follow-up work for upstream fixes. |
| `ignore/fixtures/src/tests/video_core/` | Original GPU test sources preserved for local review of title-derived shader and macro fixtures. The normal test target uses only the remaining public sources in `src/tests/`. |
| `ignore/cleanup-plan.md` | Scope and destination map for the repository cleanup. |
| `ignore/targeted-fixes-plan.md` | Scope and acceptance checks for the five audit fixes. |
| `ignore/research-review-first-pass.md` | Scope and acceptance checks for the initial fixes from the research review. |
| `ignore/provenance/Assets.car` | Quarantined legacy macOS asset catalog. |
| `ignore/RESEARCH_REVIEW.md` | Point-in-time research review snapshot archived out of root. |
| `ignore/RYUJINX_COMPARISON.md` | Point-in-time Ryubing comparative study snapshot archived out of root. |
| `ignore/UPSTREAM_ISSUES.md` | Point-in-time upstream issue snapshot archived out of root. |
| `ignore/UPSTREAM_EASY_WINS.md` | Scope and verification record for the first upstream fixes archived out of root. |
| `ignore/UPSTREAM_NEXT_BATCH.md` | Ownership and acceptance checks for the next upstream fixes archived out of root. |
| `.claude/skills/nx2-shader-triage/scripts/relower_words.cpp` | Standalone offline lowering helper used by the shader-triage workflow. |

Git and the Nix source filter exclude the root `ignore/` and `.lab/` directories. The CPack
template applies the same exclusions when a separate CPack configuration uses it.

### Source and release checks

| Path | Responsibility |
| --- | --- |
| `ASSET_PROVENANCE.md` | Record source evidence, attribution and outstanding redistribution questions for audited assets. |
| `CPackSourceConfig.cmake.in` | Source-package exclusion template used when `CPack` is configured with the repository root on `CMAKE_MODULE_PATH`; preserves CPack’s generated defaults and excludes the repository’s root `ignore/` and `.lab/` from source packages. |
| `tools/ci/check_release_paths.py` | Reject high-confidence private artifact paths in a Git path list or in ZIP and TAR.GZ members. |
| `tools/ci/license-header.sh` | Check (exit 1 on failure) and optionally update SPDX headers on branch diffs against `nx2` (excluding `externals/FidelityFX-SDK` and other third-party trees). |
| `tools/ci/test_check_release_paths.py` | Exercise accepted and rejected scanner inputs with synthetic files. |
| `tools/ci/test_license_header.py` | Verify `license-header.sh` CLI/stanza contracts, repaired SPDX headers, and unmodified FidelityFX-SDK MIT headers. |
| `tools/ci/test_workflows.py` | Parse `.github/workflows` / `.github/actions` YAML and verify triggers, timeouts, cache wiring, and script hygiene. |
| `.github/workflows/test-linux.yml` | Reusable workflow running script/Python checks and cached headless Linux Catch2 tests (`ccache` + `.cache/cpm`). |
| `.github/workflows/source-checks.yml` | Check the candidate tree, run `license-header.sh` on pull requests, and call `test-linux.yml` on `push` (`nx2`, `nx2-experimental`), `pull_request`, and `workflow_dispatch`. |
| `.github/workflows/obsolete-on-release.yml` | Mark unpinned/unassigned/unmilestoned open issues obsolete after stable releases (called from `ci.yml`, `release`, or `workflow_dispatch`). |

The release workflow uses its trusted checkout to scan the exact build commit and final ZIP and TAR.GZ files. It attaches the project and Apache licenses and the asset record when that record is present in the build commit. Linux AppImage staging is scanned on the build runner. The macOS ZIP and DMG are assembled from the same app bundle. The macOS bundle uses `dist/eglantine.icns` for its app icon.

## 6. Conventions this tree follows

* **Behaviour switches are standalone `EDEN_NX2_*` environment variables**; the
  `Nx2Perf` framework is for *performance* switches. A switch that changes what is drawn
  belongs outside `Nx2Perf`, because `Nx2Perf`'s contract is that everything in it is on
  by default and safe mode turns off only the ones that cannot change the output.
* **A skipped draw must come back.** A draw dropped during compilation can leave a
  permanent hole. `EvaluateDrawSkip` (`src/video_core/renderer_vulkan/draw_skip_policy.h`)
  applies one skip contract across `BuiltPipeline`, `CanSkipDraw` (asynchronous translation),
  `DeferRuntimeBuild` (shader build budget), and `CanSkipForDiskReplay` /
  `in_flight_disk_replays` (waiting on in-flight disk replays for unskippable draws). For
  native draws, skipping requires depth testing, an active color target, and a color target
  redrawn every frame (`RedrawnEveryFrame`); `EDEN_NX2_NATIVE_REDRAW=0` restores the older
  historical-write rule for comparisons, and `EDEN_NX2_PERF_SAFE=1` (`DefaultOffMask`) turns
  off all five draw-skipping and replay switches (`ShaderBuildBudget`, `StaleTargetSkip`,
  `GeometryConsistency`, `BuildOrderPriority`, `ReplayDiskRecordings`).
* **Cache files are versioned and named per build.** `CACHE_VERSION` guards them, and
  `IsOwnPipelineCache` / `LoadVulkanPipelineCache` delete a file of another version
  rather than read it.
* **`src/tests/` mirrors `src/`** (`src/tests/video_core/…` for `src/video_core/…`).
* **Experimental geometry stage is opt-in.** `EDEN_NX2_EXPERIMENTAL_GEOMETRY=1` enables it;
  leaving it unset keeps it off by default (`src/video_core/shader_environment.cpp`).

## 7. Known gaps

* [Asset provenance](ASSET_PROVENANCE.md) records the resolved clean-room replacement for
  the firmware keyboard tables (`src/core/hle/service/set/key_code_map.h`, verified by
  `src/tests/core/key_code_map.cpp`) and one remaining redistribution confirmation for
  Eglantine artwork (`dist/Eglantine.png` and `dist/eglantine.icns`). Earlier revisions still
  contain the quarantined game-derived fixtures and macOS catalog.
* The public suite omits 31 cases containing or depending on title-derived instructions. Eight
  independently encoded cases now cover priority DMA and Ampere regressions. The Windows
  comparison passed 445 of 450 cases before the fixes and 453 of 458 afterwards, with identical
  calibration, debug-server and networking failures. The Linux pull-request job still needs a
  run on GitHub.

### Closed by the full-codebase audit pass

The audit of the whole tree (not only of a diff) reported nine high-severity findings. They are
closed now; the table names the contract each fix restores.

| Area | Finding | Fix |
| --- | --- | --- |
| `core` HLE | The graphics server trusted guest-controlled sizes, counts and offsets. | `nx2_gsv.h` validates every range before it allocates, loops or computes an address; `nvdrv`'s heap mapper range check is bounds- and wrap-safe. |
| `input_common` | The Switch 2 wire protocol did not match the reference implementation. | The feature flags, the common report, the battery and IMU layouts and the rumble frames follow `PROTOCOL.md`; the USB transport strips the report id before the parser sees the frame. |
| `input_common` | The controller lifecycle lost its state: the auto-pair pass was built per poll, so a controller that went away was never reported. | Both frontends keep one `Switch2AutoPair`; `ApplyDevices()` keeps the state between the passes and marks a controller as gone after it was missing for several passes in a row. |
| `video_core` | `GpuSplit::active` was a plain `bool` that three threads read. | It is an atomic with an acquire/release protocol. |
| `video_core` | The translation serial moved before the page table changed, so a cache entry could hold a translation the table no longer had. | The serial moves after the table changed and while the page table lock is held. |
| `network` | `Room::GetRoomInformation()` and `RoomMember::GetUsername()` handed out references to state the network thread writes. | Both return by value; the room guards `room_information` with its own mutex and reads the member and host slots before it takes the member lock. |
| `network` | `RoomImpl::StartLoop` ignored its stop token, so destroying a room that was created but not destroyed hung. | The loop honours the stop token. |
| `core` | The teardown destroyed the services, the guest processes and the cheat engine while the timing thread was still running their callbacks. | The timing thread stays paused for the whole teardown, and the cheat engine is reset before the kernel releases the processes. |
| `core` | `CoreTiming::Advance()` used a reference to an event that its own callback could unschedule. | The event is copied into locals before the callback runs. |

The pass also closed a teardown crash that the fixes above exposed: the kernel's intrusive object
list holds objects that live in the guest RAM arena, and the arena is freed before the kernel is
destroyed. `KAutoObjectWithListContainer::Finalize()` empties the list while the arena is alive,
and `KernelCore::ClearObjectList()` runs it from `System::Impl`'s destructor as a safety net.

### Left open by the P0 security and concurrency pass

These are known and deliberately not fixed by the P0 pass; each one names its owner area.

* **`network`**: a `std::vector<std::string>` read is bounded by the bytes left in the packet,
  but a malicious room server can still make the room allocate about eight times that many
  bytes; an absolute element-count cap would close it. The room also copies a whole received
  packet before the relay size cap is applied, so the cap bounds what is relayed, not what is
  transiently copied. `GetNickname()` still hands out a reference to state the network thread
  writes; its only write happens before the network thread starts, which is why it was left
  alone.
* **`core` HLE**: `CoreTiming::UnscheduleEvent` defaults to `Wait`, which takes `advance_lock`
  after removing the event and waits for any in-progress callback. `DrainGpuCallbacks` in
  session destructors wakes immediately via `std::condition_variable` +
  `GetEmulationStopToken()` when emulation stops, though a wedged GPU thread mid-session
  remains bounded at up to five seconds.
* **The kernel leaks its own threads.** The threads the kernel creates (the core threads, the
  idle threads, the shutdown threads and the host threads) are registered in the object list
  and never closed, so their slab slots are never released. The list is emptied safely now, so
  this is a leak and not a crash, but the teardown still does not close them.

* **`IsFullyMappedRange` counts a reserved small page as covered.** That is its long-standing
  contract and the native-macro paths depend on it; the unit test documents it rather than
  changing it.
* The Qt frontend is built against a **system Qt** here: the bundled Qt 6.11.1 is a
  static-CRT build and cannot be linked into a `/MD` binary (`__std_rotate` is unresolved).
  CI installs Qt 6.8.3 the same way.

### Recorded Wave 2 verification

* The Windows MSVC build produced `tests.exe`, `eden-cli.exe` and `eglantine.exe`.
  That run passed 587 Catch2 test cases (75,251,757 assertions) and all seven tests in
  `tools/ci/test_check_release_paths.py`.

### Recorded Test Coverage Expansion Verification

* Expanded unit test coverage across `core` (`src/tests/core/launch_params.cpp`,
  `src/tests/core/nx2_gsv_bounds.cpp`, `src/tests/core/shutdown_ordering.cpp`), `input_common`
  (`src/tests/input_common/switch2_protocol.cpp`), and `video_core` / Ampere lowering
  (`src/tests/video_core/ampere_compute_lowering.cpp`).
* After rebuilding `tests.exe` with `src/tests/CMakeLists.txt` updated for `core/launch_params.cpp`,
  all five targeted Catch2 runs passed: `[launch_params]` (6 cases, 202 assertions), `[nx2_gsv]`
  (12 cases, 114 assertions), `[shutdown-ordering]` (5 cases, 25 assertions), `[switch2]`
  (30 cases, 574 assertions), and `[ampere]` (70 cases, 60,262 assertions).
