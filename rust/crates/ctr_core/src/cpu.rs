// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use crate::memory::Memory;
use crate::ExecutionError;

const PC: usize = 15;

pub(crate) struct Arm11 {
    registers: [u32; 16],
}

impl Arm11 {
    pub(crate) fn new(entry: u32) -> Self {
        let mut registers = [0; 16];
        registers[PC] = entry;
        Self { registers }
    }

    pub(crate) fn pc(&self) -> u32 {
        self.registers[PC]
    }

    pub(crate) fn register(&self, index: u8) -> Option<u32> {
        self.registers.get(usize::from(index)).copied()
    }

    pub(crate) fn step(&mut self, memory: &mut Memory) -> Result<(), ExecutionError> {
        let pc = self.pc();
        let memory_fault = |source| ExecutionError::Memory { pc, source };
        let opcode = memory.fetch_word(pc).map_err(memory_fault)?;
        let mut next_pc = pc.wrapping_add(4);
        let rd = ((opcode >> 12) & 15) as usize;
        let rn = ((opcode >> 16) & 15) as usize;

        match opcode {
            // MOV (immediate), without a CPSR update.
            opcode if opcode & 0xffff_0000 == 0xe3a0_0000 && rd != PC => {
                let rotate = ((opcode >> 8) & 15) * 2;
                self.registers[rd] = (opcode & 0xff).rotate_right(rotate);
            }
            // LDR (literal), with the A32 pipeline offset.
            opcode if opcode & 0xff7f_0000 == 0xe51f_0000 && rd != PC => {
                let address = offset_address(pc.wrapping_add(8), opcode);
                self.registers[rd] = memory.read_word(address).map_err(memory_fault)?;
            }
            // STR (immediate), pre-indexed without writeback.
            opcode if opcode & 0xff70_0000 == 0xe500_0000 && rd != PC && rn != PC => {
                let address = offset_address(self.registers[rn], opcode);
                memory
                    .write_word(address, self.registers[rd])
                    .map_err(memory_fault)?;
            }
            // B, sign-extending imm24 and then scaling to a byte displacement.
            opcode if opcode & 0xff00_0000 == 0xea00_0000 => {
                let displacement = (((opcode << 8) as i32) >> 6) as u32;
                next_pc = pc.wrapping_add(8).wrapping_add(displacement);
            }
            _ => return Err(ExecutionError::UnsupportedInstruction { pc, opcode }),
        }
        self.registers[PC] = next_pc;
        Ok(())
    }
}

fn offset_address(base: u32, opcode: u32) -> u32 {
    let magnitude = opcode & 0xfff;
    if opcode & (1 << 23) == 0 {
        base.wrapping_sub(magnitude)
    } else {
        base.wrapping_add(magnitude)
    }
}
