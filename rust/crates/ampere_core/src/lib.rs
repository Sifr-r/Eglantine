// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![deny(warnings)]

pub mod address_map;
pub mod capture;
pub mod ffi;
pub mod lower;
pub mod override_codec;
pub mod pressure;
pub mod types;

pub use address_map::{
    append_native_address_map, has_native_address_map, native_program_address,
    NATIVE_ADDRESS_MAP_MAGIC,
};
pub use capture::{CaptureResult, ProgramCaptureState};
pub use lower::{lower_to_maxwell, lower_to_maxwell_with};
pub use override_codec::{
    decode_program_override, encode_program_override, native_program_key,
    program_override_source_size,
};
pub use pressure::{collect_register_operands, prepare_register_pressure, RegisterPressure};
pub use types::{
    native_register_copy, Instruction, LoweredProgram, LoweringOptions, TensorModeOverride,
};
