# First-party Rust engines for Nintendo DS and Nintendo 3DS

## Approach

Eglantine will own both engines in Rust. Dust is a reference for DS accuracy and debugging. Citra is a reference for 3DS high-level emulation, and its successor Azahar helps define the feature targets. The user chose first-party development over integrating an existing core. Integrating Dust could shorten the path to playable DS games, while reusing a Citra-derived core would bring a C++ guest runtime.

[Dust](https://github.com/kelpsyberry/dust) · [Citra](https://github.com/neobrain/citra) · [Azahar](https://github.com/azahar-emu/azahar) · [melonDS](https://melonds.kuribo64.net/index.php)

## Existing integration

The Rust library catalogue already identifies DS and 3DS. The C++ SDL classic runtime accepts GB, GBC, and GBA and exposes one video surface, a button mask, audio, and persistent saves. Its GBA implementation shows how to build first-party Rust alongside the C++ host. A dual-screen session will need two independently sized surfaces and touch coordinates. Start with headless execution so CPU and loader behavior can be verified independently of the host.

## First milestone

Build both cores in parallel. Each core must load and execute a small independently authored guest program from its native file format. Guest code must change a known RAM value, and a fixed instruction budget must bound every run. Playable games, firmware boot, display output, and frontend launch belong to later milestones.

- DS will directly load the ARM9 and ARM7 payloads from an NDS image. Both CPUs can access 4 MiB of main RAM at `0x02000000`. ARM7 can also access 64 KiB of private RAM at `0x03800000`. Reject overlapping load destinations in main RAM, out-of-bounds file ranges, and entry points outside the corresponding payload. Both entry points must be word-aligned. Give each CPU its own register state and step ARM9 then ARM7 in each scheduling round. The fixture will demonstrate that both processors execute distinct code and write distinct RAM words. [GBATEK memory map](https://mgba-emu.github.io/gbatek/)
- 3DS will load a restricted 3DSX profile with a 32-byte header, eight-byte relocation headers, version and flags zero, and zero relocation counts. Begin execution at the word-aligned start of the code segment at `0x00100000`. Read segments consecutively from the file, align their guest bases on 4 KiB boundaries, and clear the BSS tail of data. Limit the mapped image to 16 MiB. Check file sizes and guest ranges before mapping, validate code and read-only data permissions, and return explicit errors for unsupported format features. The fixture will demonstrate ARM11 execution and a RAM write. [3DSX format](https://3dbrew.org/wiki/3DSX_Format)
- Expose a Rust step operation and a bounded run operation. A successful step retires one instruction on 3DS or one instruction per CPU on DS. A bounded run accepts a host-supplied stop condition and a maximum step count. Report a distinct stop reason when the condition is met or the budget is exhausted. A guest fault remains a typed error.
- Implement only the ARM instructions required by the fixtures. Verify execution results with assertions on instruction state and memory, and return a contextual error for unsupported instructions or addresses. Fixtures will be generated from independently authored instruction words in the test source.
- Keep guest emulation in safe, dependency-free Rust and reuse the existing Cargo workspace. Shared code should be extracted only after the two cores demonstrate a common need. ARM7, ARM9, and ARM11 behavior must remain individually testable.

## Files for the first milestone

Each core uses the same module layout with its own machine behavior.

| Path | Responsibility |
| --- | --- |
| `rust/crates/ds_core/Cargo.toml`, `rust/crates/ctr_core/Cargo.toml` | Define the first-party DS and 3DS packages and inherit workspace metadata. |
| `rust/crates/{ds_core,ctr_core}/src/lib.rs` | Declare modules and expose each core's safe API. |
| `rust/crates/{ds_core,ctr_core}/src/loader.rs` | Parse and validate NDS or 3DSX images and produce validated load segments. |
| `rust/crates/{ds_core,ctr_core}/src/cpu.rs` | Execute the supported guest ARM instructions and maintain register state. |
| `rust/crates/{ds_core,ctr_core}/src/memory.rs` | Model the supported guest RAM regions and check access width, address, and alignment. |
| `rust/crates/{ds_core,ctr_core}/src/machine.rs` | Own session state, map validated segments, and manage bounded execution. |
| `rust/crates/{ds_core,ctr_core}/src/error.rs` | Define typed load and execution errors with guest context. |
| `rust/crates/{ds_core,ctr_core}/tests/boot.rs` | Generate guest fixtures and verify image validation and CPU effects through the public API. |
| `rust/Cargo.toml`, `rust/Cargo.lock` | Register and lock the two workspace members. |
| `ARCHITECTURE.md`, `docs/knowledge/rust_ffi.md` | Record core ownership, supported entry points, and verification commands when the milestone lands. |

The DS and 3DS assignments own their crate directories. The lead integrates their Cargo and documentation changes after both assignments pass focused verification.

## Later milestones

1. Advance DS toward playable titles with ARM and Thumb coverage, cartridge and save memory, scheduling, interrupts, DMA, timers, video, audio, touch, input, and RTC. Advance 3DS toward playable homebrew with 3DSX relocations, ARM11 kernel and service calls, HID, graphics, and audio. Validate homebrew before adding support for additional 3DS title formats.
2. Add a host session once a core can deliver timed top and bottom framebuffers and accept guest input. The host owns layout, scaling of display coordinates, controller mapping, and save-file I/O. The engines own guest timing and input state. Introduce an FFI boundary through `eglantine_ffi` when the SDL host needs it. Publish runtime availability per console after a supported application can boot through its normal entry point.
3. Prioritize versioned save states and fast-forward for both systems. Evaluate DS microphone, lid, debugging, and 3D upscaling, and 3DS internal resolution scaling and debugging once the underlying devices are available. Treat later features such as DSi mode and multiplayer as separate compatibility milestones.

## Verification

- Build and test both packages through the Rust workspace on Windows with `cargo test --manifest-path rust/Cargo.toml --locked --offline`. Check new code formatting and lint each package.
- Exercise a valid load and deterministic guest effects, truncated and out-of-range segments, invalid entry alignment, an unsupported opcode, and instruction-budget exhaustion. Verify that a rejected load yields no runnable session.
- Keep the existing frontend availability flags intact for this milestone. Verify the full Rust workspace to catch integration regressions. Future host work must verify the affected frontends through their normal launch paths.
