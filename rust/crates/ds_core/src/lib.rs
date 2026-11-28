// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

//! Headless direct boot for ARM9 and ARM7 payloads from an NDS image.

mod cpu;
mod error;
mod loader;
mod machine;
mod memory;

pub use cpu::Cpu;
pub use error::{Access, ExecutionError, LoadError, MemoryError};
pub use machine::{Machine, RunOutcome, StopReason};
