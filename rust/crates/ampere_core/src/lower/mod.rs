// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

mod control_flow;
mod fp_half;
mod int_logic;
mod memory_tex;
mod tensor;

use crate::pressure::{collect_register_operands, prepare_register_pressure};
use crate::types::{
    lower_extended_env, tensor_forced_mode_env, Instruction, LoweredProgram, LoweringOptions,
    TensorModeOverride,
};

#[derive(Clone, Copy, Debug)]
pub(super) struct Relocation {
    pub output_index: u32,
    pub target: i64,
    pub call: bool,
    pub output_target: u32,
}

impl Relocation {
    #[inline]
    pub(super) const fn new(output_index: u32, target: i64) -> Self {
        Self {
            output_index,
            target,
            call: false,
            output_target: u32::MAX,
        }
    }

    #[inline]
    pub(super) const fn new_call(output_index: u32, target: i64) -> Self {
        Self {
            output_index,
            target,
            call: true,
            output_target: u32::MAX,
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub(super) struct HalfSource {
    pub reg: u32,
    pub swizzle: u32,
    pub absolute: bool,
    pub negate: bool,
}

pub(super) enum StepAction {
    Continue,
    Finish(u32),
    Fail(u32, String),
}

pub(super) struct LowerCtx<'a> {
    pub code: &'a [Instruction],
    pub high_uniform_base: u32,
    pub high_uniforms: u32,
    pub shadow_boundary: u32,
    pub options: LoweringOptions,
    pub uses_async_copy: bool,
    pub result: LoweredProgram,
    pub relocations: Vec<Relocation>,
    pub barrier_tokens: Vec<u32>,
    pub call_targets: Vec<u32>,
    pub call_links: Vec<u32>,
    pub main_exited: bool,
    pub computed_branch: bool,
    // Per-instruction state
    pub index: u32,
    pub insn: Instruction,
    pub opcode: u32,
    pub native_predicate: u32,
    pub predicate: u64,
    pub dest: u64,
    pub rewritten_uniform: bool,
    pub guarded_uniform: bool,
    pub guard_slot: u32,
    pub operand_error: Option<&'static str>,
}

impl<'a> LowerCtx<'a> {
    #[inline]
    pub(super) const fn address_of(index: u32) -> u32 {
        0x50u32 + index * 8 + ((index + 1) / 3) * 8
    }

    #[inline]
    pub(super) fn lower_extended(&self) -> bool {
        self.options
            .lower_extended_override
            .unwrap_or_else(lower_extended_env)
    }

    #[inline]
    pub(super) fn tensor_mode(&self) -> Option<&'static str> {
        if let Some(override_mode) = self.options.tensor_mode_override {
            return match override_mode {
                TensorModeOverride::Disabled => None,
                TensorModeOverride::Inline => Some("1"),
                TensorModeOverride::Compact => Some("compact"),
                TensorModeOverride::Stub => Some("stub"),
            };
        }
        if let Some(forced) = tensor_forced_mode_env() {
            return Some(forced);
        }
        if !self.options.tensor {
            None
        } else if self.options.compact_tensor {
            Some("compact")
        } else {
            Some("1")
        }
    }

    #[inline]
    fn fail_with(mut self, index: u32, reason: String) -> LoweredProgram {
        self.result.instructions.clear();
        self.result.source_instruction_offsets.clear();
        self.result.error_offset = index * (std::mem::size_of::<Instruction>() as u32);
        self.result.error = reason;
        self.result
    }

    #[inline]
    fn fail(self, index: u32, reason: &str) -> LoweredProgram {
        self.fail_with(index, reason.to_owned())
    }

    #[inline]
    pub(super) fn finish(mut self, index: u32) -> LoweredProgram {
        for relocation in &self.relocations {
            if relocation.target > index as i64 {
                return self.fail(index, "native branch or call leaves captured program");
            }
            let target = if relocation.output_target != u32::MAX {
                relocation.output_target
            } else {
                self.result.source_instruction_offsets[relocation.target as usize]
            };
            let relative = (Self::address_of(target) as i64)
                - (Self::address_of(relocation.output_index) as i64)
                - 8;
            if !(-0x80_0000..=0x7f_ffff).contains(&relative) {
                return self.fail(index, "lowered branch distance");
            }
            self.result.instructions[relocation.output_index as usize] |=
                ((relative as u64) & 0xff_ffff) << 20;
        }
        self.result.source_size = (index + 1) * (std::mem::size_of::<Instruction>() as u32);
        self.result
    }

    #[inline]
    pub(super) fn emit(&mut self, word: u64) {
        self.result.instructions.push(word);
    }

    #[inline]
    pub(super) const fn base(&self, opcode_bits: u64) -> u64 {
        opcode_bits | self.predicate | self.dest
    }

    #[inline]
    pub(super) const fn bank(&self) -> u32 {
        let value = self.insn.bits(54, 5);
        if value == 24 {
            17
        } else {
            value
        }
    }

    #[inline]
    pub(super) const fn cbuf(&self) -> u64 {
        (((self.insn.bits(38, 16) / 4) as u64) << 20) | ((self.bank() as u64) << 34)
    }

    #[inline]
    pub(super) const fn valid_cbuf(&self) -> bool {
        self.insn.bits(91, 1) == 0 && self.bank() < 18 && (self.insn.bits(38, 16) % 4) == 0
    }

    pub(super) fn uniform_register(&mut self, reg: u32) -> u32 {
        if reg < 22 {
            return 216 + reg;
        }
        if reg < 28 {
            return 218 + reg;
        }
        if reg == 63 {
            return 255;
        }
        if reg < 63 && (reg - 28) < self.high_uniforms {
            return self.high_uniform_base + (reg - 28);
        }
        if reg < 63 {
            self.result.high_uniform_registers = self
                .result
                .high_uniform_registers
                .max((reg - 26) & !1u32);
        }
        u32::MAX
    }

    #[inline]
    pub(super) fn bind_texture_buffer(&mut self, binding: u32) -> bool {
        if self.result.required_texture_buffer != u32::MAX
            && self.result.required_texture_buffer != binding
        {
            return false;
        }
        self.result.required_texture_buffer = binding;
        true
    }

    #[inline]
    pub(super) fn save_predicate(&mut self, mask: u32) {
        self.emit(
            0x38e8_0000_0007_0000u64 | 253 | (255u64 << 8) | ((mask as u64) << 20),
        );
    }

    #[inline]
    pub(super) fn restore_predicate(&mut self, mask: u32) {
        self.emit(0x38f0_0000_0007_0000u64 | (253u64 << 8) | ((mask as u64) << 20));
    }

    #[allow(clippy::too_many_arguments)]
    pub(super) fn predicate_lut(
        &mut self,
        mut lut: u32,
        a: u32,
        b: u32,
        c: u32,
        target: u32,
        uniform_c: bool,
        second_target: u32,
        mut second_lut: u32,
    ) {
        let inputs = [a, b, c];
        for (i, &inp) in inputs.iter().enumerate() {
            if i == 2 && uniform_c {
                if (c & 7) == 7 {
                    self.emit(
                        0x0100_0000_0000_0000u64
                            | self.predicate
                            | 252
                            | (15u64 << 12)
                            | ((u32::MAX as u64) << 20),
                    );
                } else {
                    self.emit(
                        0x3800_0000_0000_0000u64
                            | self.predicate
                            | 252
                            | (239u64 << 8)
                            | (((6 - (c & 7)) | (1u32 << 8)) as u64) << 20,
                    );
                    self.emit(
                        0x5c10_0000_0000_0000u64
                            | self.predicate
                            | 252
                            | (255u64 << 8)
                            | (252u64 << 20)
                            | (1u64 << 48),
                    );
                }
                if (c & 8) != 0 {
                    lut = ((lut & 0x55) << 1) | ((lut & 0xaa) >> 1);
                    second_lut = ((second_lut & 0x55) << 1) | ((second_lut & 0xaa) >> 1);
                }
                continue;
            }
            self.emit(
                0x38a0_0000_0000_0000u64
                    | self.predicate
                    | ((250 + i) as u64)
                    | (255u64 << 8)
                    | (0x7ffffu64 << 20)
                    | (1u64 << 56)
                    | (((inp ^ 8u32) as u64) << 39),
            );
        }
        let write_lut = |ctx: &mut Self, table: u32, destination: u32| {
            if destination == 7 {
                return;
            }
            ctx.emit(
                0x5be0_0000_0000_0000u64
                    | ctx.predicate
                    | 255
                    | (250u64 << 8)
                    | (251u64 << 20)
                    | ((table as u64) << 28)
                    | (3u64 << 36)
                    | (252u64 << 39)
                    | ((destination as u64) << 48),
            );
        };
        if ((self.predicate >> 16) & 7) as u32 == target {
            write_lut(self, second_lut, second_target);
            write_lut(self, lut, target);
        } else {
            write_lut(self, lut, target);
            write_lut(self, second_lut, second_target);
        }
    }

    #[inline]
    pub(super) fn mov_imm(&mut self, reg: u32, value: u32) {
        self.emit(
            0x0100_0000_0000_0000u64
                | self.predicate
                | (reg as u64)
                | (15u64 << 12)
                | ((value as u64) << 20),
        );
    }

    pub(super) fn materialize(
        &mut self,
        kind: u32,
        mut reg: u32,
        scratch: u32,
        absolute: bool,
        negate: bool,
    ) -> u32 {
        if kind == 3 {
            let mapped = self.uniform_register(reg);
            if mapped == u32::MAX {
                self.operand_error = Some("native uniform source register");
                return 255;
            }
            reg = mapped;
        } else if kind == 1 {
            self.mov_imm(scratch, self.insn.words[1]);
            reg = scratch;
        } else if kind == 2 {
            if !self.valid_cbuf() {
                self.operand_error = Some("native constant buffer address");
                return 255;
            }
            let cbuf_bits = self.cbuf();
            self.emit(
                0x4c98_0000_0000_0000u64
                    | self.predicate
                    | (scratch as u64)
                    | (15u64 << 39)
                    | cbuf_bits,
            );
            reg = scratch;
        }
        if absolute || negate {
            if reg != scratch {
                self.emit(
                    0x5c98_0000_0000_0000u64
                        | self.predicate
                        | (scratch as u64)
                        | ((reg as u64) << 20)
                        | (15u64 << 39),
                );
            }
            if absolute {
                self.emit(
                    0x0400_0000_0000_0000u64
                        | self.predicate
                        | (scratch as u64)
                        | ((scratch as u64) << 8)
                        | (0x7fff_ffffu64 << 20),
                );
            }
            if negate {
                self.emit(
                    0x0400_0000_0000_0000u64
                        | self.predicate
                        | (scratch as u64)
                        | ((scratch as u64) << 8)
                        | (0x8000_0000u64 << 20)
                        | (2u64 << 53),
                );
            }
            reg = scratch;
        }
        reg
    }

    pub(super) fn operand(&mut self, which: u32, floating: bool) -> u32 {
        let form = self.insn.bits(9, 3);
        let mut reg = 0u32;
        let mut kind = 0u32;
        let mut abs = 0u32;
        let mut neg = 0u32;
        if which == 0 {
            reg = self.insn.bits(24, 8);
            abs = self.insn.bits(73, 1);
            neg = self.insn.bits(72, 1);
        } else {
            if !(1..=7).contains(&form) {
                self.operand_error = Some("native ALU source form");
                return 255;
            }
            let wide = if which == 1 {
                matches!(form, 4..=6)
            } else {
                matches!(form, 2 | 3 | 7)
            };
            if wide {
                kind = if form == 6 || form == 7 {
                    3
                } else if form == 4 || form == 2 {
                    1
                } else {
                    2
                };
                if kind == 3 {
                    reg = self.insn.bits(32, 8);
                }
                if kind == 2 || kind == 3 {
                    abs = self.insn.bits(62, 1);
                    neg = self.insn.bits(63, 1);
                }
            } else {
                let upper = if which == 1 {
                    matches!(form, 2 | 3 | 7)
                } else {
                    true
                };
                reg = self.insn.bits(if upper { 64 } else { 32 }, 8);
                abs = self.insn.bits(if upper { 74 } else { 62 }, 1);
                neg = self.insn.bits(if upper { 75 } else { 63 }, 1);
            }
        }
        self.materialize(
            kind,
            reg,
            250 + which,
            floating && abs != 0,
            floating && neg != 0,
        )
    }

    pub(super) fn half_source(&mut self, which: u32) -> HalfSource {
        if which == 0 {
            return HalfSource {
                reg: self.insn.bits(24, 8),
                swizzle: self.insn.bits(74, 2),
                absolute: self.insn.bits(73, 1) != 0,
                negate: self.insn.bits(72, 1) != 0,
            };
        }
        let form = self.insn.bits(9, 3);
        let wide = if which == 1 {
            form == 4 || form == 5
        } else {
            form == 2 || form == 3
        };
        if wide {
            let immediate = form == 2 || form == 4;
            let reg = self.materialize(if immediate { 1 } else { 2 }, 255, 250 + which, false, false);
            return HalfSource {
                reg,
                swizzle: if immediate { 0 } else { self.insn.bits(60, 2) },
                absolute: !immediate && self.insn.bits(62, 1) != 0,
                negate: !immediate && self.insn.bits(63, 1) != 0,
            };
        }
        let upper = which == 2 || form == 2 || form == 3;
        HalfSource {
            reg: self.insn.bits(if upper { 64 } else { 32 }, 8),
            swizzle: self.insn.bits(if upper { 81 } else { 60 }, 2),
            absolute: self.insn.bits(if upper { 83 } else { 62 }, 1) != 0,
            negate: self.insn.bits(if upper { 84 } else { 63 }, 1) != 0,
        }
    }

    pub(super) fn extract_half(&mut self, source: HalfSource, lane: u32, target: u32) {
        let high = source.swizzle == 3 || (source.swizzle == 0 && lane == 1);
        self.emit(
            0x5ca8_0000_0000_0000u64
                | self.predicate
                | (target as u64)
                | (2u64 << 8)
                | ((if source.swizzle == 1 { 2u64 } else { 1u64 }) << 10)
                | ((source.reg as u64) << 20)
                | ((high as u64) << 41)
                | ((source.negate as u64) << 45)
                | ((source.absolute as u64) << 49),
        );
    }

    #[allow(clippy::too_many_arguments)]
    pub(super) fn xmad(
        &mut self,
        target: u32,
        a: u32,
        b: u32,
        c: u32,
        a_high: bool,
        b_high: bool,
        mode: u32,
        immediate: bool,
    ) {
        let b_field = if immediate {
            (b >> if b_high { 16 } else { 0 }) & 0xffff
        } else {
            b
        };
        self.emit(
            (if immediate {
                0x3600_0000_0000_0000u64
            } else {
                0x5b00_0000_0000_0000u64
            }) | self.predicate
                | (target as u64)
                | ((a as u64) << 8)
                | ((b_field as u64) << 20)
                | (((!immediate && b_high) as u64) << 35)
                | ((c as u64) << 39)
                | ((mode as u64) << 50)
                | ((a_high as u64) << 53),
        );
    }

    pub(super) fn multiply_low(&mut self, a: u32, b: u32, c: u32, immediate: bool) {
        self.xmad(250, a, b, c, false, false, 0, immediate);
        self.xmad(251, a, b, 255, true, false, 0, immediate);
        self.xmad(251, a, b, 251, false, true, 0, immediate);
        self.emit(
            self.base(0x5c18_0000_0000_0000u64) | (251u64 << 8) | (250u64 << 20) | (16u64 << 39),
        );
    }

    pub(super) fn right_shift_u64_low(&mut self, target: u32, low: u32, high: u32, shift: u32) {
        self.mov_imm(250, 32);
        self.emit(
            0x5c10_0000_0000_0000u64
                | self.predicate
                | 251
                | (250u64 << 8)
                | ((shift as u64) << 20)
                | (1u64 << 48),
        );
        self.emit(
            0x5c28_0000_0000_0000u64 | self.predicate | 250 | ((low as u64) << 8) | ((shift as u64) << 20),
        );
        self.emit(
            0x5c48_0000_0000_0000u64 | self.predicate | 251 | ((high as u64) << 8) | (251u64 << 20),
        );
        self.emit(
            0x5c40_0000_0000_0000u64
                | self.predicate
                | 250
                | (250u64 << 8)
                | (251u64 << 20)
                | (1u64 << 41)
                | (7u64 << 48),
        );
        self.emit(
            0x1c00_0000_0000_0000u64
                | self.predicate
                | 251
                | ((shift as u64) << 8)
                | (0xffff_ffe0u64 << 20),
        );
        self.emit(
            0x5c28_0000_0000_0000u64 | self.predicate | 251 | ((high as u64) << 8) | (251u64 << 20),
        );
        self.emit(
            0x5c40_0000_0000_0000u64
                | self.predicate
                | (target as u64)
                | (250u64 << 8)
                | (251u64 << 20)
                | (1u64 << 41)
                | (7u64 << 48),
        );
    }
}

#[inline]
fn instruction_uses_uniform_bank(instruction: &Instruction) -> bool {
    let op = instruction.bits(0, 12);
    matches!(
        op,
        0x4a4
            | 0x2bf
            | 0x499
            | 0x3c4
            | 0x2bd
            | 0x896
            | 0xf60
            | 0xc11
            | 0xd08
            | 0xc0c
            | 0x986
            | 0x9a8
            | 0xc12
            | 0xc17
            | 0xe24
            | 0xc24
            | 0xd00
            | 0xd09
            | 0x9c3
            | 0xc45
            | 0x2a4
            | 0x891
            | 0x890
            | 0x290
            | 0x28c
            | 0x88c
            | 0x287
            | 0x887
            | 0x958
            | 0xab9
            | 0xabb
            | 0x899
            | 0xc20
            | 0xe21
            | 0xe23
            | 0xc02
            | 0x886
            | 0x3c2
            | 0xc82
            | 0x882
            | 0x292
            | 0x892
            | 0x291
            | 0x981
            | 0xc13
            | 0xc10
            | 0xc23
            | 0x526
            | 0x2be
            | 0x8a4
            | 0xd05
            | 0x89c
            | 0x883
            | 0x2a5
            | 0xd06
            | 0xf63
            | 0xf66
            | 0xf69
            | 0xc0b
            | 0x8a5
            | 0x299
            | 0xc09
            | 0xc07
            | 0xf6c
            | 0xc0a
    ) || ((op == 0x984 || op == 0x988) && instruction.bits(91, 1) != 0)
        || (op == 0x81c && instruction.bits(67, 1) != 0)
        || (op == 0x83b && instruction.bits(91, 1) != 0)
        || (op == 0x98e && instruction.bits(91, 1) != 0)
}

pub fn lower_to_maxwell_with(
    code: &[Instruction],
    high_uniforms: u32,
    options: LoweringOptions,
) -> LoweredProgram {
    let high_uniform_base = 216u32.wrapping_sub(high_uniforms);
    let uses_async_copy = code.iter().any(|insn| insn.bits(0, 12) == 0xfae);
    let uses_uniform_bank = code.iter().any(instruction_uses_uniform_bank);

    let shadow_boundary = (if uses_uniform_bank {
        high_uniform_base
    } else {
        246u32
    }) & !3u32;

    let mut ctx = LowerCtx {
        code,
        high_uniform_base,
        high_uniforms,
        shadow_boundary,
        options,
        uses_async_copy,
        result: LoweredProgram::default(),
        relocations: Vec::new(),
        barrier_tokens: Vec::new(),
        call_targets: Vec::new(),
        call_links: Vec::new(),
        main_exited: false,
        computed_branch: false,
        index: 0,
        insn: Instruction::default(),
        opcode: 0,
        native_predicate: 0,
        predicate: 0,
        dest: 0,
        rewritten_uniform: false,
        guarded_uniform: false,
        guard_slot: 0,
        operand_error: None,
    };

    if code.is_empty() || code.len() > 65536 {
        return ctx.fail(0, "invalid native program size");
    }

    for index in 0..(code.len() as u32) {
        let mut insn = code[index as usize];
        ctx.result
            .source_instruction_offsets
            .push(ctx.result.instructions.len() as u32);
        let mut opcode = insn.bits(0, 12);
        if insn.bits(12, 4) == 15 {
            continue;
        }

        let mut f16_dest = 255u32;
        let mut f16_second = 255u32;
        let mut f16_components = 0u32;

        let mut active_link: Option<u32> = None;
        let mut active_entry = 0u32;
        if ctx.main_exited {
            for call in 0..ctx.call_targets.len() {
                if ctx.call_targets[call] <= index && ctx.call_targets[call] >= active_entry {
                    active_entry = ctx.call_targets[call];
                    active_link = Some(ctx.call_links[call]);
                }
            }
            let no_destination = matches!(opcode, 0x950 | 0x947 | 0x918)
                || (opcode & 0x1ff) == 0x00c
                || (opcode & 0x1ff) == 0x00b;
            let pair = matches!(opcode, 0x225 | 0x625 | 0x825);
            if let Some(link) = active_link {
                if !no_destination
                    && (insn.bits(16, 8) == link || (pair && insn.bits(16, 8) + 1 == link))
                {
                    return ctx.fail(index, "native callee overwrites link register");
                }
            }
        }

        let mut pressure = None;
        let mut requires_shadow = false;
        let mut collected_known = true;
        if !collect_register_operands(&insn, |bit, count| {
            let reg = insn.bits(bit, 8);
            if reg < 255 && reg + count > shadow_boundary {
                requires_shadow = true;
            }
        }) {
            collected_known = false;
        }
        if !collected_known {
            for bit in [16u32, 24, 32, 64] {
                if opcode == 0x81c {
                    break;
                }
                if bit == 32
                    && insn.bits(9, 3) != 1
                    && !matches!(
                        opcode,
                        0xb60
                            | 0xb66
                            | 0xb6c
                            | 0xf6c
                            | 0x986
                            | 0x987
                            | 0xf66
                            | 0x988
                            | 0x927
                            | 0x9a8
                            | 0x98e
                            | 0x59f
                    )
                {
                    continue;
                }
                let reg = insn.bits(bit, 8);
                if reg < 255 && reg + 1 > shadow_boundary {
                    requires_shadow = true;
                }
            }
        }
        if requires_shadow {
            let prepared = prepare_register_pressure(&insn, shadow_boundary);
            if !prepared.valid {
                return ctx.fail(index, "native program uses lowering scratch registers");
            }
            insn = prepared.instruction;
            ctx.result.instructions.extend_from_slice(&prepared.prefix);
            pressure = Some(prepared);
        }

        let instruction_start = ctx.result.instructions.len();
        let relocation_start = ctx.relocations.len();

        if (opcode == 0xb66 || opcode == 0xb63) && insn.bits(79, 2) == 1 {
            f16_dest = insn.bits(16, 8);
            f16_second = insn.bits(64, 8);
            f16_components = insn.bits(72, 4).count_ones();
            if f16_dest >= 246
                || (f16_second != 255 && f16_second >= 246)
                || insn.bits(61, 3) == 5
                || (f16_second == 255 && f16_components > 2)
            {
                return ctx.fail(index, "native half-precision texture result registers");
            }
            insn.words[2] &= !(1u32 << 15);
            insn.words[0] = (insn.words[0] & !0x00ff_0000u32)
                | if f16_dest == 255 {
                    0x00ff_0000u32
                } else {
                    248u32 << 16
                };
            if f16_second != 255 {
                insn.words[2] = (insn.words[2] & !0xffu32) | 246u32;
            }
        }

        let mut rewritten_uniform = false;
        let mut uniform_guard = false;
        let mut uniform_output = 7u32;
        if matches!(
            opcode,
            0x2a4
                | 0x4a4
                | 0x8a4
                | 0x891
                | 0x890
                | 0x290
                | 0x292
                | 0x892
                | 0x291
                | 0x896
                | 0x8a5
        ) {
            let guarded_lea =
                (opcode == 0x291 || opcode == 0x891) && insn.bits(80, 1) == 0 && insn.bits(81, 3) == 7;
            let guarded_add = (opcode == 0x890
                || (opcode == 0x290 && (insn.words[1] & !0xffu32) == 0))
                && insn.bits(72, 5) == 0
                && insn.bits(77, 7) == 0x7f
                && insn.bits(84, 7) == 0x7f;
            let guarded_add_no_carry =
                (opcode == 0x290 || opcode == 0x890) && insn.bits(81, 6) == 0x3f;
            if insn.bits(12, 4) != 7
                && opcode != 0x292
                && opcode != 0x892
                && !guarded_lea
                && !guarded_add
                && !guarded_add_no_carry
            {
                return ctx.fail(index, "native uniform arithmetic predication");
            }
            uniform_guard = insn.bits(12, 4) != 7;
            rewritten_uniform = true;
            let dst = ctx.uniform_register(insn.bits(16, 8));
            let a = ctx.uniform_register(insn.bits(24, 8));
            let c = ctx.uniform_register(insn.bits(64, 8));
            if dst == u32::MAX || a == u32::MAX || c == u32::MAX {
                return ctx.fail(index, "native uniform arithmetic register");
            }
            insn.words[0] = (insn.words[0] & 0xffff) | (dst << 16) | (a << 24);
            insn.words[2] = (insn.words[2] & !0x0800_00ffu32) | c;
            if matches!(opcode, 0x2a4 | 0x290 | 0x292 | 0x291) {
                let b = ctx.uniform_register(insn.bits(32, 8));
                if b == u32::MAX {
                    return ctx.fail(index, "native uniform arithmetic second register");
                }
                insn.words[1] = (insn.words[1] & !0xffu32) | b;
            }
            if (opcode == 0x292 || opcode == 0x892) && insn.bits(80, 4) != 0xe {
                if insn.bits(80, 1) != 0 {
                    return ctx.fail(index, "native uniform logic predicate output");
                }
                uniform_output = insn.bits(81, 3);
                insn.words[2] =
                    (insn.words[2] & !(7u32 << 17)) | ((6 - uniform_output) << 17);
            }
            opcode = match opcode {
                0x2a4 => 0x224,
                0x4a4 => 0x424,
                0x8a4 => 0x824,
                0x891 => 0x811,
                0x890 => 0x810,
                0x292 => 0x212,
                0x892 => 0x812,
                0x291 => 0x211,
                0x896 => 0x816,
                0x8a5 => 0x825,
                _ => 0x210,
            };
        }

        let uniform_predication = matches!(
            opcode,
            0xabb | 0xab9 | 0xc82 | 0x882 | 0x899 | 0x499
        );
        let native_predicate = insn.bits(12, 4);
        let guarded_uniform = (uniform_predication || uniform_guard) && native_predicate != 7;
        let guard_slot = 6u32.wrapping_sub(native_predicate & 7);
        let predicate = ((if guarded_uniform {
            (native_predicate & 8) | guard_slot
        } else {
            native_predicate
        }) as u64)
            << 16;
        let dest = insn.bits(16, 8) as u64;
        let guard_mask = if guard_slot < 7 { 1u32 << guard_slot } else { 0u32 };

        ctx.index = index;
        ctx.insn = insn;
        ctx.opcode = opcode;
        ctx.native_predicate = native_predicate;
        ctx.predicate = predicate;
        ctx.dest = dest;
        ctx.rewritten_uniform = rewritten_uniform;
        ctx.guarded_uniform = guarded_uniform;
        ctx.guard_slot = guard_slot;
        ctx.operand_error = None;

        if guarded_uniform {
            ctx.save_predicate(guard_mask);
            ctx.emit(
                0x38f0_0000_0007_0000u64 | (239u64 << 8) | ((guard_mask as u64) << 20),
            );
        }
        let output_mask = if uniform_output == 7 {
            0u32
        } else {
            1u32 << (6 - uniform_output)
        };
        if output_mask != 0 {
            ctx.emit(
                0x38e8_0000_0007_0000u64 | 254 | (255u64 << 8) | ((output_mask as u64) << 20),
            );
            ctx.emit(
                0x38f0_0000_0007_0000u64 | (239u64 << 8) | ((output_mask as u64) << 20),
            );
        }

        let action = fp_half::try_lower(&mut ctx)
            .or_else(|| int_logic::try_lower(&mut ctx))
            .or_else(|| memory_tex::try_lower(&mut ctx))
            .or_else(|| tensor::try_lower(&mut ctx))
            .or_else(|| control_flow::try_lower(&mut ctx));

        match action {
            Some(StepAction::Continue) => {}
            Some(StepAction::Finish(end_index)) => return ctx.finish(end_index),
            Some(StepAction::Fail(err_index, reason)) => return ctx.fail_with(err_index, reason),
            None => return ctx.fail(index, "unsupported native opcode"),
        }

        if let Some(err) = ctx.operand_error {
            return ctx.fail(index, err);
        }

        if f16_components != 0 {
            let pred = (ctx.insn.bits(12, 4) as u64) << 16;
            let pack = |ctx: &mut LowerCtx<'_>, target: u32, low: u32, high: u32| {
                ctx.emit(
                    0x5ca8_0000_0000_0000u64
                        | pred
                        | 250
                        | (1u64 << 8)
                        | (2u64 << 10)
                        | ((high as u64) << 20),
                );
                ctx.emit(
                    0x5ca8_0000_0000_0000u64
                        | pred
                        | 251
                        | (1u64 << 8)
                        | (2u64 << 10)
                        | ((low as u64) << 20),
                );
                ctx.emit(
                    0x5c18_0000_0000_0000u64
                        | pred
                        | (target as u64)
                        | (250u64 << 8)
                        | (251u64 << 20)
                        | (16u64 << 39),
                );
            };
            if f16_dest != 255 {
                pack(
                    &mut ctx,
                    f16_dest,
                    248,
                    if f16_components > 1 { 249 } else { 255 },
                );
            }
            if f16_second != 255 && f16_components > 2 {
                pack(
                    &mut ctx,
                    f16_second,
                    246,
                    if f16_components > 3 { 247 } else { 255 },
                );
            }
        }

        if output_mask != 0 {
            ctx.emit(
                0x38e8_0000_0007_0000u64 | 239 | (239u64 << 8) | ((output_mask as u64) << 20),
            );
            ctx.emit(
                0x38f0_0000_0007_0000u64 | (254u64 << 8) | ((output_mask as u64) << 20),
            );
        }

        if guarded_uniform {
            ctx.restore_predicate(guard_mask);
        }

        if let Some(prepared) = pressure {
            if !prepared.suffix.is_empty() {
                for pending in &mut ctx.relocations[relocation_start..] {
                    if !pending.call
                        && (pending.output_index as usize) >= instruction_start
                        && pending.target == (index as i64) + 1
                    {
                        pending.output_target = ctx.result.instructions.len() as u32;
                    }
                }
                ctx.result.instructions.extend_from_slice(&prepared.suffix);
            }
        }
    }

    let msg = if ctx.main_exited {
        "native program lacks captured library return"
    } else {
        "native program has no unconditional EXIT"
    };
    ctx.fail(code.len() as u32, msg)
}

pub fn lower_to_maxwell(code: &[Instruction], options: LoweringOptions) -> LoweredProgram {
    let mut high_uniforms = 0u32;
    loop {
        let lowered = lower_to_maxwell_with(code, high_uniforms, options);
        if lowered.is_ok() || lowered.high_uniform_registers <= high_uniforms {
            return lowered;
        }
        high_uniforms = lowered.high_uniform_registers;
    }
}
