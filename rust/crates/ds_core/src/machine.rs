// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use crate::cpu::{Cpu, CpuState};
use crate::error::{ExecutionError, LoadError, MemoryError};
use crate::{loader, memory::Memory};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum StopReason {
    ConditionMet,
    BudgetExhausted,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RunOutcome {
    /// Number of completed ARM9 and ARM7 rounds in this call.
    pub steps: usize,
    pub reason: StopReason,
}

pub struct Machine {
    arm9: CpuState,
    arm7: CpuState,
    memory: Memory,
    fault: Option<ExecutionError>,
}

impl Machine {
    /// Validates the direct-boot payloads and starts both CPUs at their image entry points.
    pub fn load(image: &[u8]) -> Result<Self, LoadError> {
        let (arm9, arm7) = loader::load(image)?;
        let mut memory = Memory::new();
        memory.load(arm9.mapping, arm9.bytes);
        memory.load(arm7.mapping, arm7.bytes);
        Ok(Self {
            arm9: CpuState::new(Cpu::Arm9, arm9.entry),
            arm7: CpuState::new(Cpu::Arm7, arm7.entry),
            memory,
            fault: None,
        })
    }

    /// Retires one ARM9 instruction followed by one ARM7 instruction.
    ///
    /// If ARM7 faults, the ARM9 instruction in that round remains retired. Any guest fault
    /// stops both CPUs and is returned by subsequent execution calls. Inspection remains available.
    pub fn step(&mut self) -> Result<(), ExecutionError> {
        if let Some(error) = self.fault {
            return Err(error);
        }
        let result = self
            .arm9
            .step(&mut self.memory)
            .and_then(|()| self.arm7.step(&mut self.memory));
        if let Err(error) = result {
            self.fault = Some(error);
        }
        result
    }

    /// Checks `stop` before the first round and after each completed round, up to `max_steps`.
    /// A faulted machine returns its stored fault before calling `stop`.
    pub fn run_until(
        &mut self,
        max_steps: usize,
        mut stop: impl FnMut(&Self) -> bool,
    ) -> Result<RunOutcome, ExecutionError> {
        if let Some(error) = self.fault {
            return Err(error);
        }
        if stop(self) {
            return Ok(RunOutcome {
                steps: 0,
                reason: StopReason::ConditionMet,
            });
        }
        for steps in 1..=max_steps {
            self.step()?;
            if stop(self) {
                return Ok(RunOutcome {
                    steps,
                    reason: StopReason::ConditionMet,
                });
            }
        }
        Ok(RunOutcome {
            steps: max_steps,
            reason: StopReason::BudgetExhausted,
        })
    }

    /// Returns the next instruction address for the selected CPU.
    pub fn pc(&self, cpu: Cpu) -> u32 {
        self.cpu(cpu).pc()
    }

    /// Returns R0 through R15, with R15 reported as the next instruction address.
    pub fn register(&self, cpu: Cpu, index: u8) -> Option<u32> {
        self.cpu(cpu).register(index)
    }

    /// Reads an aligned little-endian word in the selected CPU's address space.
    pub fn read_word(&self, cpu: Cpu, address: u32) -> Result<u32, MemoryError> {
        self.memory.read_word(cpu, address)
    }

    fn cpu(&self, cpu: Cpu) -> &CpuState {
        match cpu {
            Cpu::Arm9 => &self.arm9,
            Cpu::Arm7 => &self.arm7,
        }
    }
}
