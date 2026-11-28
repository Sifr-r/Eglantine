// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

mod cpu;
mod error;
mod loader;
mod machine;
mod memory;

pub use error::{Access, ExecutionError, LoadError, MemoryError, MemoryErrorKind, Segment};
pub use machine::{Machine, RunOutcome, StopReason};
