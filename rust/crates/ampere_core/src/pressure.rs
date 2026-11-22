// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use crate::types::{native_register_copy, Instruction};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RegisterPressure {
    pub instruction: Instruction,
    pub prefix: Vec<u64>,
    pub suffix: Vec<u64>,
    pub valid: bool,
}

impl Default for RegisterPressure {
    #[inline]
    fn default() -> Self {
        Self {
            instruction: Instruction::default(),
            prefix: Vec::new(),
            suffix: Vec::new(),
            valid: true,
        }
    }
}

#[inline]
const fn mem_width_count(width: u32) -> u32 {
    if width == 6 {
        4
    } else if width == 5 {
        2
    } else {
        1
    }
}

pub fn collect_register_operands<F: FnMut(u32, u32)>(
    input: &Instruction,
    mut operand: F,
) -> bool {
    let opcode = input.bits(0, 12);
    match opcode {
        0x802 | 0xa02 | 0xb05 | 0xb08 | 0xd05 | 0xd08 => {
            operand(16, 1);
        }
        0x804 | 0x80b | 0x80c | 0xa0b | 0xa0c | 0xc0b | 0xc0c => {
            operand(24, 1);
        }
        0x805 => {
            operand(16, 2);
        }
        0x305 | 0x306 | 0x308 => {
            // F2I, I2F, MUFU
            operand(16, 1);
            operand(32, 1);
        }
        0x433 | 0x633 | 0x80a | 0xa0a | 0xc0a => {
            operand(16, 1);
            operand(24, 1);
        }
        0x20a | 0x220 | 0x221 => {
            operand(16, 1);
            operand(24, 1);
            operand(32, 1);
        }
        0x421 | 0x817 | 0x820 | 0x81a => {
            operand(16, 1);
            operand(24, 1);
        }
        0x919 => {
            operand(16, 1);
        }
        0x20b | 0x20c => {
            // FSETP/ISETP: predicate results
            operand(24, 1);
            operand(32, 1);
        }
        0x208 => {
            operand(16, 1);
            operand(24, 1);
            operand(32, 1);
        }
        0x419 => {
            // SHF immediate form: Rd, Rb
            operand(16, 1);
            operand(64, 1);
        }
        0x823 => {
            operand(16, 1);
            operand(24, 1);
            operand(64, 1);
        }
        0x245 => {
            operand(16, 1);
            operand(32, 1);
        }
        0x810 => {
            operand(16, 1);
            operand(24, 1);
            operand(64, 1);
        }
        // SHF, register form: Rd, low word, shift amount, high word
        0x219 | 0x210 | 0x223 | 0x243 => {
            operand(16, 1);
            operand(24, 1);
            operand(32, 1);
            operand(64, 1);
        }
        0x233 => {
            operand(16, 1);
            operand(24, 1);
            operand(32, 1);
        }
        0xa20 | 0x621 | 0x808 | 0x923 => {
            operand(16, 1);
            operand(24, 1);
        }
        0x209 => {
            operand(16, 1);
            operand(24, 1);
            operand(32, 1);
        }
        // IMAD with a uniform multiplier: Rd, Ra and the addend Rc are registers
        0xc24 | 0xa23 | 0x623 | 0x423 => {
            operand(16, 1);
            operand(24, 1);
            operand(64, 1);
        }
        0x321 => {
            operand(16, input.bits(74, 2) + 1);
            operand(24, 1);
            operand(32, 1);
        }
        0x322 => {
            operand(24, 1);
            operand(32, input.bits(74, 2) + 1);
        }
        0x927 => {
            operand(24, 1);
            operand(32, 1);
        }
        0x9a8 => {
            // ATOMG: result in Rd, data operand at bit 32.
            operand(16, 1);
            operand(32, 1);
        }
        0xb6c | 0xf6c => {
            // TXD: results in Rd/Rd+1 and the second destination, coordinates in Ra, gradients in Rb..Rb+3.
            let channels = input.bits(72, 4).count_ones();
            operand(16, channels.clamp(1, 2));
            // ARRAY_2D also reads the layer
            operand(24, if input.bits(61, 3) == 5 { 3 } else { 2 });
            operand(32, 4);
            if channels > 2 {
                operand(64, channels - 2);
            }
        }
        0xb82 => {
            operand(16, if input.bits(73, 3) == 5 { 2 } else { 1 });
            operand(24, 1);
        }
        0x202 => {
            operand(16, 1);
            operand(32, 1);
        }
        // LOP3.LUT, constant buffer and uniform B: no register in B's field
        0xa12 | 0xc12 | 0x812 | 0x824 | 0x831 | 0x424 | 0x624 => {
            operand(16, 1);
            operand(24, 1);
            operand(64, 1);
        }
        0x230 | 0x231 | 0x211 | 0x212 | 0x224 => {
            operand(16, 1);
            operand(24, 1);
            operand(32, 1);
            operand(64, 1);
        }
        0x819 | 0xa11 | 0xa19 => {
            operand(16, 1);
            operand(24, 1);
            operand(64, 1);
        }
        0xc10 => {
            // IADD3 with a uniform second source (bit 32 is a UR)
            operand(16, 1);
            operand(24, 1);
            operand(64, 1);
        }
        0x225 | 0x625 | 0x825 => {
            operand(16, 2);
            operand(24, 1);
            if opcode == 0x225 {
                operand(32, 1);
                operand(64, 2);
            }
            if opcode == 0x625 {
                operand(64, 1);
            }
            if opcode == 0x825 {
                operand(64, 2);
            }
        }
        0x83b => {
            operand(16, 1u32 << input.bits(72, 2));
            operand(24, 1);
        }
        0x23c => {
            let deep = input.bits(75, 1) != 0; // .16816
            operand(16, 2);
            operand(24, if deep { 4 } else { 2 });
            operand(32, if deep { 2 } else { 1 });
            operand(64, 2);
        }
        0x388 | 0x984 | 0x988 => {
            let count = mem_width_count(input.bits(73, 3));
            operand(if opcode == 0x984 { 16 } else { 32 }, count);
            operand(24, 1);
        }
        0x387 | 0x983 => {
            let count = mem_width_count(input.bits(73, 3));
            operand(if opcode == 0x387 { 32 } else { 16 }, count);
            operand(24, 1);
        }
        0x386 | 0x981 | 0x986 => {
            let count = mem_width_count(input.bits(73, 3));
            operand(if opcode == 0x981 { 16 } else { 32 }, count);
            let wide_addr = if opcode == 0x386 {
                input.bits(72, 1)
            } else {
                input.bits(90, 1)
            } != 0;
            operand(24, if wide_addr { 2 } else { 1 });
        }
        0x59f => {
            let dimension = input.bits(61, 3);
            operand(
                24,
                if dimension <= 1 {
                    1
                } else if dimension <= 3 {
                    2
                } else {
                    3
                },
            );
            operand(32, 1);
        }
        0x98e => {
            operand(24, 2);
            operand(32, 1);
        }
        0x381 => {
            // LDG with a 64-bit address
            let count = mem_width_count(input.bits(73, 3));
            operand(16, count);
            operand(24, 2);
        }
        _ => return false,
    }
    true
}

// Native registers at or above `reserved` (the lowering's uniform mirrors and scratch) live in
// bank 1. For one instruction, each aligned quartet of them it uses is copied into a low quartet
// the instruction doesn't use (whose own values wait in bank 2), and copied back afterwards.
// The copies are SSA renames: they cost nothing in the host shader.
pub fn prepare_register_pressure(input: &Instruction, reserved: u32) -> RegisterPressure {
    let mut result = RegisterPressure {
        instruction: *input,
        prefix: Vec::new(),
        suffix: Vec::new(),
        valid: true,
    };
    let mut operands = Vec::new();
    if !collect_register_operands(input, |bit, count| {
        operands.push((bit, count));
    }) {
        result.valid = false;
        return result;
    }

    let mut used = [false; 256];
    let mut groups = [false; 64];
    let mut paired = [false; 64];

    for &(bit, count) in &operands {
        let reg = input.bits(bit, 8);
        if reg == 255 {
            continue;
        }
        if reg + count > 255 {
            result.valid = false;
            return result;
        }
        let overlaps_reserved = reg + count > reserved;
        for component in 0..count {
            used[(reg + component) as usize] = true;
            if overlaps_reserved {
                groups[((reg + component) / 4) as usize] = true;
            }
        }
        if overlaps_reserved {
            let first_group = reg / 4;
            let last_group = (reg + count - 1) / 4;
            for g in first_group..last_group {
                paired[g as usize] = true;
            }
        }
    }

    for (group, &active) in groups.iter().enumerate() {
        if !active {
            continue;
        }
        for component in 0..4 {
            used[group * 4 + component] = true;
        }
    }

    let aligned_reserved = reserved & !3u32;
    let mut mapped = [0u32; 64];
    for group in 0..groups.len() {
        if !groups[group] {
            continue;
        }
        if group > 0 && paired[group - 1] {
            mapped[group] = mapped[group - 1] + 4;
        } else {
            let mut run_length = 1u32;
            while group + (run_length as usize) < groups.len()
                && paired[group + (run_length as usize) - 1]
            {
                run_length += 1;
            }
            let needed = run_length * 4;
            if needed > aligned_reserved {
                result.valid = false;
                return result;
            }
            let mut borrow: Option<u32> = None;
            let mut candidate = aligned_reserved - needed;
            loop {
                let mut free = true;
                for offset in 0..needed {
                    if used[(candidate + offset) as usize] {
                        free = false;
                        break;
                    }
                }
                if free {
                    borrow = Some(candidate);
                    break;
                }
                if candidate < 4 {
                    break;
                }
                candidate -= 4;
            }
            let Some(chosen) = borrow else {
                result.valid = false;
                return result;
            };
            mapped[group] = chosen;
            for offset in 0..needed {
                used[(chosen + offset) as usize] = true;
            }
        }
        for component in 0..4u32 {
            let low = mapped[group] + component;
            let high = (group as u32) * 4 + component;
            let high_bank = if high >= reserved { 1u32 } else { 0u32 };
            result.prefix.push(native_register_copy(2, low, 0, low));
            result
                .prefix
                .push(native_register_copy(0, low, high_bank, high));
            result
                .suffix
                .push(native_register_copy(high_bank, high, 0, low));
            result.suffix.push(native_register_copy(0, low, 2, low));
        }
    }

    for &(bit, _) in &operands {
        let reg = input.bits(bit, 8);
        if reg == 255 || !groups[(reg / 4) as usize] {
            continue;
        }
        let replacement = mapped[(reg / 4) as usize] + (reg % 4);
        let word = (bit / 32) as usize;
        let shift = bit % 32;
        result.instruction.words[word] =
            (result.instruction.words[word] & !(255u32 << shift)) | (replacement << shift);
    }

    result
}
