// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use std::error::Error;
use std::fmt;

use crate::cpu::Cpu;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LoadError {
    TruncatedHeader {
        actual: usize,
    },
    InvalidPayloadFileRange {
        cpu: Cpu,
        offset: u32,
        size: u32,
    },
    InvalidPayloadAddress {
        cpu: Cpu,
        address: u32,
        size: u32,
    },
    InvalidEntryPoint {
        cpu: Cpu,
        entry: u32,
        address: u32,
        size: u32,
    },
    OverlappingPayloads,
}

impl fmt::Display for LoadError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match *self {
            Self::TruncatedHeader { actual } => write!(f, "truncated NDS header ({actual} bytes)"),
            Self::InvalidPayloadFileRange { cpu, offset, size } => {
                write!(
                    f,
                    "invalid {cpu:?} payload file range (offset {offset:#x}, size {size:#x})"
                )
            }
            Self::InvalidPayloadAddress { cpu, address, size } => {
                write!(
                    f,
                    "invalid {cpu:?} payload destination (base {address:#010x}, size {size:#x})"
                )
            }
            Self::InvalidEntryPoint {
                cpu,
                entry,
                address,
                size,
            } => {
                write!(f, "invalid {cpu:?} entry {entry:#010x} (payload base {address:#010x}, size {size:#x})")
            }
            Self::OverlappingPayloads => f.write_str("ARM9 and ARM7 payloads overlap in main RAM"),
        }
    }
}

impl Error for LoadError {}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Access {
    Fetch,
    Read,
    Write,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MemoryError {
    UnalignedWord { address: u32 },
    UnmappedWord { address: u32 },
}

impl fmt::Display for MemoryError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match *self {
            Self::UnalignedWord { address } => write!(f, "unaligned word at {address:#010x}"),
            Self::UnmappedWord { address } => write!(f, "unmapped word at {address:#010x}"),
        }
    }
}

impl Error for MemoryError {}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ExecutionError {
    UnsupportedInstruction {
        cpu: Cpu,
        pc: u32,
        opcode: u32,
    },
    Memory {
        cpu: Cpu,
        pc: u32,
        access: Access,
        source: MemoryError,
    },
}

impl fmt::Display for ExecutionError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match *self {
            Self::UnsupportedInstruction { cpu, pc, opcode } => {
                write!(
                    f,
                    "{cpu:?} at {pc:#010x}: unsupported instruction {opcode:#010x}"
                )
            }
            Self::Memory {
                cpu,
                pc,
                access,
                source,
            } => {
                write!(f, "{cpu:?} at {pc:#010x}: {access:?} failed: {source}")
            }
        }
    }
}

impl Error for ExecutionError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        match self {
            Self::Memory { source, .. } => Some(source),
            Self::UnsupportedInstruction { .. } => None,
        }
    }
}
