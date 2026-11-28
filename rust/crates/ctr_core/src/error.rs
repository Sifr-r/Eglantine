// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use std::error::Error;
use std::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Segment {
    Code,
    Rodata,
    Data,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LoadError {
    TruncatedHeader {
        available: usize,
    },
    InvalidMagic([u8; 4]),
    UnsupportedHeaderSize(u16),
    UnsupportedRelocationHeaderSize(u16),
    UnsupportedVersion(u32),
    UnsupportedFlags(u32),
    TruncatedRelocationHeaders {
        available: usize,
    },
    UnsupportedRelocations {
        segment: Segment,
        absolute: u32,
        relative: u32,
    },
    InvalidCodeSize(u32),
    InvalidBssSize {
        data_size: u32,
        bss_size: u32,
    },
    ImageTooLarge {
        mapped_size: u64,
    },
    TruncatedSegment {
        segment: Segment,
        expected: usize,
        available: usize,
    },
    TrailingData {
        bytes: usize,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Access {
    Fetch,
    Read,
    Write,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MemoryErrorKind {
    Unaligned,
    Unmapped,
    PermissionDenied,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MemoryError {
    pub address: u32,
    pub access: Access,
    pub kind: MemoryErrorKind,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ExecutionError {
    Memory { pc: u32, source: MemoryError },
    UnsupportedInstruction { pc: u32, opcode: u32 },
}

impl fmt::Display for LoadError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match *self {
            Self::TruncatedHeader { available } => {
                write!(f, "truncated 3DSX header ({available} bytes available)")
            }
            Self::InvalidMagic(magic) => write!(f, "invalid 3DSX magic {magic:02x?}"),
            Self::UnsupportedHeaderSize(size) => write!(f, "unsupported 3DSX header size {size}"),
            Self::UnsupportedRelocationHeaderSize(size) => {
                write!(f, "unsupported 3DSX relocation header size {size}")
            }
            Self::UnsupportedVersion(version) => write!(f, "unsupported 3DSX version {version}"),
            Self::UnsupportedFlags(flags) => write!(f, "unsupported 3DSX flags {flags:#010x}"),
            Self::TruncatedRelocationHeaders { available } => {
                write!(f, "truncated 3DSX relocation headers ({available} bytes available)")
            }
            Self::UnsupportedRelocations {
                segment,
                absolute,
                relative,
            } => write!(
                f,
                "unsupported 3DSX {segment:?} relocations ({absolute} absolute, {relative} relative)"
            ),
            Self::InvalidCodeSize(size) => write!(f, "3DSX code size {size} cannot hold an ARM word"),
            Self::InvalidBssSize { data_size, bss_size } => {
                write!(f, "3DSX BSS size {bss_size} exceeds data size {data_size}")
            }
            Self::ImageTooLarge { mapped_size } => {
                write!(f, "3DSX mapped size of {mapped_size} bytes exceeds 16 MiB")
            }
            Self::TruncatedSegment {
                segment,
                expected,
                available,
            } => write!(
                f,
                "truncated 3DSX {segment:?} segment ({expected} bytes expected, {available} available)"
            ),
            Self::TrailingData { bytes } => write!(f, "unsupported 3DSX trailing data ({bytes} bytes)"),
        }
    }
}

impl Error for LoadError {}

impl fmt::Display for MemoryError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let reason = match self.kind {
            MemoryErrorKind::Unaligned => "unaligned word access",
            MemoryErrorKind::Unmapped => "unmapped word",
            MemoryErrorKind::PermissionDenied => "permission denied",
        };
        write!(
            f,
            "ARM11 {:?} at {:#010x} failed ({reason})",
            self.access, self.address
        )
    }
}

impl Error for MemoryError {}

impl fmt::Display for ExecutionError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Memory { pc, source } => write!(f, "at PC {pc:#010x} {source}"),
            Self::UnsupportedInstruction { pc, opcode } => {
                write!(
                    f,
                    "unsupported ARM11 instruction {opcode:#010x} at PC {pc:#010x}"
                )
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
