# Rust and C++ consolidation plan

Keep one implementation for each shared code path. Preserve caller interfaces, supported build targets, file formats, and regression cases from the current worktree.

| Area | Owner | File responsibilities |
| --- | --- | --- |
| Ampere lowering, capture, address maps, overrides | C++ | The existing headers in `src/shader_recompiler/frontend/ampere/` own these four paths. `src/tests/video_core/ampere_program.cpp` checks the retained behavior. `rust/crates/ampere_spirv/` and `rust/include/eglantine_ampere_spirv.h` own direct compute translation and its ABI. Remove `rust/crates/ampere_core/`, its header, and its differential tooling. |
| NUP archives | C++ | `src/core/file_sys/nup_container.cpp` owns archive reads and `nup_writer.cpp` owns writing, extraction, and recovery. Preserve behavior exercised by the current Rust parity work in `src/tests/core/nup_writer.cpp` before removing `rust/crates/nup_fs/` and its ABI header. |
| Switch 2 controller protocol | C++ | `src/input_common/helpers/switch2_protocol/` owns codecs, transport, and controller lifetime. Preserve the behavior of the Rust runtime default and its recent regression cases in `src/tests/input_common/switch2_protocol.cpp` before removing `rust/crates/switch2_io/` and its ABI header. This also preserves builds without a Rust protocol dependency. |
| Desktop game discovery | C++ | `src/frontend_common/game_list_discovery.h` and `src/yuzu_cmd/gpui_library.cpp` own file discovery and metadata. `rust/apps/eglantine_gpui/src/library.rs` stores folder preferences and recent play order, then receives results from the existing C++ host. The GPUI shell applies results from the latest asynchronous request. |
| Build and ownership | Existing entry points | Root and domain CMake files select the remaining Cargo features. `rust/Cargo.toml`, `rust/Cargo.lock`, and `rust/crates/eglantine_ffi/` describe and export the remaining crate graph. `docs/knowledge/rust_ffi.md` records current ownership and build switches. |

Run the Rust workspace and GPUI library tests, the Windows Catch2 `[ampere]`, `[nup]`, and `[switch2]` suites, and the GPUI host integration tests against the rebuilt targets. Check that former backend switches and symbols have no live callers.
