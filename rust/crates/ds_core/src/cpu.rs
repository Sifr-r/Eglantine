// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use crate::error::{Access, ExecutionError};
use crate::memory::Memory;

const PC: usize = 15;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Cpu {
    Arm9,
    Arm7,
}

pub(crate) struct CpuState {
    cpu: Cpu,
    registers: [u32; 16],
}

impl CpuState {
    pub(crate) fn new(cpu: Cpu, entry: u32) -> Self {
        let mut registers = [0; 16];
        registers[PC] = entry;
        Self { cpu, registers }
    }

    pub(crate) fn pc(&self) -> u32 {
        self.registers[PC]
    }

    pub(crate) fn register(&self, index: u8) -> Option<u32> {
        self.registers.get(usize::from(index)).copied()
    }

    pub(crate) fn step(&mut self, memory: &mut Memory) -> Result<(), ExecutionError> {
        let cpu = self.cpu;
        let pc = self.pc();
        let fault = |access| {
            move |source| ExecutionError::Memory {
                cpu,
                pc,
                access,
                source,
            }
        };
        let opcode = memory.read_word(cpu, pc).map_err(fault(Access::Fetch))?;
        let mut next_pc = pc.wrapping_add(4);
        let operand_pc = pc.wrapping_add(8);
        let rd = ((opcode >> 12) & 0xf) as usize;
        let rn = ((opcode >> 16) & 0xf) as usize;

        if opcode & 0xffff_0000 == 0xe3a0_0000 && rd != PC {
            // MOV (immediate), with flags preserved.
            let rotate = ((opcode >> 8) & 0xf) * 2;
            self.registers[rd] = (opcode & 0xff).rotate_right(rotate);
        } else if opcode & 0xff7f_0000 == 0xe51f_0000 && rd != PC {
            // LDR (literal) uses the ARM pipeline PC as its base.
            let address = immediate_address(operand_pc, opcode);
            self.registers[rd] = memory
                .read_word(cpu, address)
                .map_err(fault(Access::Read))?;
        } else if opcode & 0xff70_0000 == 0xe500_0000 && rn != PC && rd != PC {
            // STR (immediate) uses a pre-indexed word address without writeback.
            let address = immediate_address(self.registers[rn], opcode);
            memory
                .write_word(cpu, address, self.registers[rd])
                .map_err(fault(Access::Write))?;
        } else if opcode & 0xff00_0000 == 0xea00_0000 {
            let displacement = ((opcode << 8) as i32) >> 6;
            next_pc = operand_pc.wrapping_add_signed(displacement);
        } else {
            return Err(ExecutionError::UnsupportedInstruction { cpu, pc, opcode });
        }

        self.registers[PC] = next_pc;
        Ok(())
    }
}

fn immediate_address(base: u32, opcode: u32) -> u32 {
    let offset = opcode & 0xfff;
    if opcode & (1 << 23) != 0 {
        base.wrapping_add(offset)
    } else {
        base.wrapping_sub(offset)
    }
}
