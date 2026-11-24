# Phase 4 — Controller transport, NUP archives and a console frontend

This document records the original Phase 4 proposals and links to the components that were integrated. See the [ownership ledger](../ARCHITECTURE.md#first-party-language-and-domain-ownership) for selected backends and remaining parity requirements.

## 4A. Add a portable Switch 2 transport

**Status:** The optional [Rust crate](../rust/crates/switch2_io/) implements protocol codecs. Platform scanners and transports remain in C++. Run `[switch2]` for the [in-tree protocol tests](../src/tests/input_common/switch2_protocol.cpp). The btleplug and nusb transport described below remains a proposal.

The [transport interfaces](../src/input_common/helpers/switch2_protocol/switch2_transport.h) separate scanning and device operations from the driver. The [WinRT BLE implementation](../src/input_common/helpers/switch2_protocol/switch2_ble_win.cpp) and [USB scanner](../src/input_common/helpers/switch2_protocol/switch2_usb.cpp) provide the reference behavior.

Prototype a Rust `switch2_io` crate using btleplug for BLE and nusb for USB. Keep the new backend opt-in. Connect it through a thin C++ adapter and a C ABI with explicit ownership, contextual failures and callback lifetimes. Run only one scanner/transport per device. Drain outstanding callbacks before freeing their context and preserve the distinction between an explicit disconnect and a lost device.

Use synthetic packets to compare parsing, calibration and vibration encoding against the [protocol tests](../src/tests/input_common/switch2_protocol.cpp). On each supported host, exercise adapter absence, permission denial, pairing, input, rumble, reconnect and shutdown. Record physical-device coverage separately from codec tests. Switch back to the existing transport and repeat discovery before advancing.

## 4B. Preserve the NUP format and resume contract

**Status:** The optional [Rust reader and writer](../rust/crates/nup_fs/) are integrated. C++ remains the default. Use the [NUP suite](../src/tests/core/nup_writer.cpp) with `[nup]` before comparing an alternate backend. The [writer API](../src/core/file_sys/nup_writer.h) defines collision handling, cancellation, the raw-size threshold and low-storage journal recovery. Both backends must preserve member ordering, metadata encoding, compression parameters and offsets to reproduce an archive byte for byte.

- Begin with read-only parsing. Validate offsets, lengths and arithmetic before mapping extents. Compare metadata and member bytes with the C++ reader on synthetic archives, including truncated inputs.
- Pack the same fixture into separate temporary destinations with both writers and source deletion disabled. Compare full archive bytes, then extract each archive with both readers and compare the output tree. Exercise threshold boundaries, pre-existing destinations and cancellation.
- Test low-storage mode on disposable copies. The current writer can delete each source after syncing and journaling its extent. Interrupt and resume with the same arguments, and verify final content and surviving files. Preserve existing journals across backend changes. Route an in-progress archive to its compatible writer until cross-backend resume has passed.
- Keep archive creation, extraction and metadata edits on the existing backend until each operation passes its own compatibility checks. Differential verification must never run two mutating operations against a shared source or destination.

## 4C. Prototype a Slint console frontend

**Proposal:** The existing desktop entry points remain [Qt and SDL](../ARCHITECTURE.md#4-frontends).

Build the proposed `eglantine-console` as an optional target alongside [the SDL frontend](../src/yuzu_cmd/CMakeLists.txt). Reuse the existing core, input and renderer interfaces. Start with controller navigation, game launch and return to the library. Add pairing, graphics controls and an in-game quick menu as their backing features become available.

Choose and validate a Slint renderer that can share or composite frames with the Vulkan presentation path. Test Home/Guide input ownership, opening and closing the overlay during play, swapchain recreation and shutdown with frames in flight. Build and launch the existing frontends with the shell disabled. Keep the shell optional until its host dependencies and offline packaging are verified.
