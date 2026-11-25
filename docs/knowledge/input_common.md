# `input_common` — Controllers & Switch 2 Protocol

Entry points: `src/input_common/main.{h,cpp}`, `src/input_common/drivers/switch2.{h,cpp}`, `src/input_common/helpers/switch2_protocol/`.

## Core & SDL Input

- `InputCommon::InputSubsystem` owns devices; `GetKeyboard()` serves NX2 key scripts and `GetSwitch2()` exposes `InputCommon::Switch2Controllers`.
- `src/input_common/main.{h,cpp}` compares Player 1's keyboard bindings against factory defaults using order-independent comparison in `src/common/param_package.h`.
- `src/input_common/drivers/sdl_driver.cpp` checks native driver preference before opening handles, skips Switch 2 PIDs when enabled, and ignores removal events for unowned joysticks.

## Native Switch 2 Controllers

- **Protocol & framing (`src/input_common/helpers/switch2_protocol/`):**
  - `switch2_types.{h,cpp}`: BLE `0x0553` adv parser, 12-bit stick unpacking/calibration, 60+ byte report parser, 40-bit LRA vibration encoder, command/memory builders, IMU converter, `GyroMouseProcessor`, `OpticalMouseProcessor`.
  - With `ENABLE_RUST_SWITCH2_IO` enabled, `rust/crates/switch2_io` handles product ID support, controller type, and display name lookups. Builds with the option off use the C++ lookups. The runtime Rust and parity flags still control the other migrated codecs.
  - `switch2_transport.h`: `ISwitch2Transport` and `ISwitch2Scanner` + `CreateBleScanner()`/`CreateUsbScanner()`, implemented in `switch2_ble_win.cpp` (WinRT BLE GATT + non-Windows stub) and `switch2_usb.cpp` (`libusb`). Dependency direction: `Switch2Controllers` -> `Switch2Driver` -> `ISwitch2Transport` <- backends.
  - `switch2_driver.{h,cpp}`: protocol state machine, calibration/LED/rumble, mouse routing.
  - **Wire invariants:** Feature flags, common report, battery/IMU layouts, and rumble frames match `linux-switch2/PROTOCOL.md`. USB strips the report id before the parser; BLE announces the startup feature mask before the normal one (`Buttons | AnalogSticks | Motion | Rumble`, plus `Mouse` for Joy-Con).
- **Driver & slots (`drivers/switch2.{h,cpp}`):** `Switch2Controllers` (`InputEngine` `"switch2"`) manages 8 player slots, Joy-Con 2 auto-combine, runtime `SplitSlot` / `MergeSlots` / `CalibrateGyro`, and mouse interpolation. `GetDeviceBindings()` returns connected controllers + parameter packages carrying stable `device_id` (serial or transport address) so assignments survive slot re-sorts. Disconnect callbacks fire once, call `UnregisterDriver`, and prevent disconnected partners from Joy-Con combining.
- **Auto-pair (`drivers/switch2_auto_pair.{h,cpp}`):**
  - `ApplyToPlayers()` pure policy: first new controller can claim Player 1 if on factory keyboard bindings; otherwise claims lowest unconnected slot not owned by another Switch 2 controller (reusing disconnected Switch 2 slots before active non-Switch 2 slots), preserving user remaps and cleared bindings.
  - `ApplyDevices()` adds multi-pass tracking (`MissingPassesBeforeDisconnect`). **Critical:** `eden-cli` (`CheckSwitch2AutoPair()` in `src/yuzu_cmd/yuzu.cpp`) must retain **one** `Switch2AutoPair` instance for the whole session — per-poll instances never detect disconnects. Controlled by `switch2_auto_pair` (`src/common/settings.{h,cpp}`, `settings_enums.h`).

## Verification

- `./build/bin/tests "[switch2]"` (`src/tests/input_common/switch2_protocol.cpp`, `src/tests/input_common/switch2_auto_pair.cpp`).
- Check USB/BLE hardware reconnect and rumble when modifying transport/protocol layers.
