// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use crate::cpu::Arm11;
use crate::loader::{self, CODE_BASE};
use crate::memory::Memory;
use crate::{ExecutionError, LoadError, MemoryError};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StopReason {
    ConditionMet,
    BudgetExhausted,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RunOutcome {
    pub steps: usize,
    pub reason: StopReason,
}

/// Headless ARM11 session for the restricted 3DSX profile.
pub struct Machine {
    cpu: Arm11,
    memory: Memory,
}

impl Machine {
    pub fn load(image: &[u8]) -> Result<Self, LoadError> {
        let segments = loader::parse(image)?;
        Ok(Self {
            cpu: Arm11::new(CODE_BASE),
            memory: Memory::new(segments),
        })
    }

    /// Retires one supported ARM instruction.
    pub fn step(&mut self) -> Result<(), ExecutionError> {
        self.cpu.step(&mut self.memory)
    }

    /// Checks the stop condition before executing and after each completed step.
    pub fn run_until(
        &mut self,
        max_steps: usize,
        mut stop: impl FnMut(&Self) -> bool,
    ) -> Result<RunOutcome, ExecutionError> {
        for steps in 0..=max_steps {
            if stop(self) {
                return Ok(RunOutcome {
                    steps,
                    reason: StopReason::ConditionMet,
                });
            }
            if steps < max_steps {
                self.step()?;
            }
        }
        Ok(RunOutcome {
            steps: max_steps,
            reason: StopReason::BudgetExhausted,
        })
    }

    /// Address of the next instruction to execute.
    pub fn pc(&self) -> u32 {
        self.cpu.pc()
    }

    /// Returns the stored value of an ARM register. R15 holds the next instruction address.
    pub fn register(&self, index: u8) -> Option<u32> {
        self.cpu.register(index)
    }

    pub fn read_word(&self, address: u32) -> Result<u32, MemoryError> {
        self.memory.read_word(address)
    }
}
