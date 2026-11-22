// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::sync::OnceLock;

/// A 128-bit Ampere (SM86) SASS instruction.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Instruction {
    pub words: [u32; 4],
}

impl Instruction {
    #[inline]
    pub const fn new(words: [u32; 4]) -> Self {
        Self { words }
    }

    #[inline]
    pub const fn bits(&self, offset: u32, count: u32) -> u32 {
        if count == 0 {
            return 0;
        }
        let idx = (offset / 32) as usize;
        let mut bits = self.words[idx] as u64;
        if idx + 1 < self.words.len() {
            bits |= (self.words[idx + 1] as u64) << 32;
        }
        let mask = if count >= 64 {
            u64::MAX
        } else {
            (1u64 << count) - 1
        };
        ((bits >> (offset % 32)) & mask) as u32
    }
}

/// Output of SM86-to-Maxwell lowering (mirrors `Shader::Ampere::LoweredProgram`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LoweredProgram {
    pub instructions: Vec<u64>,
    pub source_instruction_offsets: Vec<u32>,
    pub source_size: u32,
    pub required_texture_buffer: u32,
    pub high_uniform_registers: u32,
    pub error_offset: u32,
    pub error: String,
}

impl Default for LoweredProgram {
    fn default() -> Self {
        Self {
            instructions: Vec::new(),
            source_instruction_offsets: Vec::new(),
            source_size: 0,
            required_texture_buffer: u32::MAX,
            high_uniform_registers: 0,
            error_offset: 0,
            error: String::new(),
        }
    }
}

impl LoweredProgram {
    #[inline]
    pub fn is_ok(&self) -> bool {
        self.error.is_empty()
    }
}

/// Optional deterministic override for `EDEN_NX2_TENSOR` (used in tests).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TensorModeOverride {
    Disabled,
    Inline,
    Compact,
    Stub,
}

/// Lowering options (mirrors `Shader::Ampere::LoweringOptions`, with optional env overrides for tests).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LoweringOptions {
    pub geometry: bool,
    pub tensor: bool,
    pub compact_tensor: bool,
    pub native_code_address: u64,
    pub lower_extended_override: Option<bool>,
    pub tensor_mode_override: Option<TensorModeOverride>,
}

/// A MOV form the Maxwell frontend reads, for lowered native programs only, as a copy between
/// register banks of 256 (bank 0 = R0-R254; banks 1 and 2 exist only in the IR).
#[inline]
pub const fn native_register_copy(
    destination_bank: u32,
    destination: u32,
    source_bank: u32,
    source: u32,
) -> u64 {
    0x5c98_0000_0007_0000u64
        | (10u64 << 44)
        | (destination as u64)
        | ((destination_bank as u64) << 8)
        | ((source_bank as u64) << 10)
        | ((source as u64) << 20)
}

#[inline]
pub fn lower_extended_env() -> bool {
    match std::env::var_os("EDEN_NX2_LOWER_EXTENDED") {
        None => true,
        Some(value) => value.as_encoded_bytes().first().copied() != Some(b'0'),
    }
}

#[inline]
pub fn tensor_forced_mode_env() -> Option<&'static str> {
    static FORCED_MODE: OnceLock<Option<String>> = OnceLock::new();
    FORCED_MODE
        .get_or_init(|| std::env::var("EDEN_NX2_TENSOR").ok())
        .as_deref()
}
