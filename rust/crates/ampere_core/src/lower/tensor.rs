// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use crate::types::native_register_copy;

use super::{HalfSource, LowerCtx, StepAction};

#[inline]
fn emit_add(ctx: &mut LowerCtx<'_>, target: u32, lhs: u32, rhs: u32) {
    ctx.emit(
        0x5c10_0000_0000_0000u64
            | ctx.predicate
            | (target as u64)
            | ((lhs as u64) << 8)
            | ((rhs as u64) << 20),
    );
}

#[inline]
fn emit_shuffle(ctx: &mut LowerCtx<'_>, target: u32, source: u32, lane: u32) {
    ctx.emit(
        0xef10_0000_0000_0000u64
            | ctx.predicate
            | (target as u64)
            | ((source as u64) << 8)
            | ((lane as u64) << 20)
            | (1u64 << 29)
            | (0x1fu64 << 34)
            | (7u64 << 48),
    );
}

#[inline]
fn emit_round_half(ctx: &mut LowerCtx<'_>, reg: u32) {
    ctx.emit(
        0x5ca8_0000_0000_0000u64
            | ctx.predicate
            | (reg as u64)
            | (1u64 << 8)
            | (2u64 << 10)
            | ((reg as u64) << 20),
    );
}

#[inline]
fn emit_fma(ctx: &mut LowerCtx<'_>, target: u32, lhs: u32, rhs: u32) {
    ctx.emit(
        0x5980_0000_0000_0000u64
            | ctx.predicate
            | (target as u64)
            | ((lhs as u64) << 8)
            | ((rhs as u64) << 20)
            | ((target as u64) << 39),
    );
}

pub(super) fn try_lower(ctx: &mut LowerCtx<'_>) -> Option<StepAction> {
    match ctx.opcode {
        0x23a => {
            // MOVM.16.MT88: transpose an 8x8 16-bit matrix held one row pair per lane.
            let source = ctx.insn.bits(24, 8);
            if ctx.insn.words[1] != 0
                || ctx.insn.words[2] != 0
                || source >= 246
                || ctx.dest >= 246
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native matrix move mode".to_owned(),
                ));
            }
            ctx.emit(0xf0c8_0000_0000_0000u64 | ctx.predicate | 246);
            ctx.emit(
                0x0400_0000_0000_0000u64 | ctx.predicate | 249 | (246u64 << 8) | (3u64 << 20),
            );
            ctx.emit(
                0x3848_0000_0000_0000u64 | ctx.predicate | 249 | (249u64 << 8) | (3u64 << 20),
            );
            ctx.emit(
                0x3828_0000_0000_0000u64 | ctx.predicate | 247 | (246u64 << 8) | (3u64 << 20),
            );
            ctx.emit(
                0x5c10_0000_0000_0000u64 | ctx.predicate | 249 | (249u64 << 8) | (247u64 << 20),
            );
            ctx.emit(
                0xef10_0000_0000_0000u64
                    | ctx.predicate
                    | 247
                    | ((source as u64) << 8)
                    | (249u64 << 20)
                    | (1u64 << 29)
                    | (31u64 << 34)
                    | (7u64 << 48),
            );
            ctx.emit(
                0x1c00_0000_0000_0000u64 | ctx.predicate | 249 | (249u64 << 8) | (4u64 << 20),
            );
            ctx.emit(
                0xef10_0000_0000_0000u64
                    | ctx.predicate
                    | 248
                    | ((source as u64) << 8)
                    | (249u64 << 20)
                    | (1u64 << 29)
                    | (31u64 << 34)
                    | (7u64 << 48),
            );
            ctx.emit(
                0x3828_0000_0000_0000u64 | ctx.predicate | 249 | (246u64 << 8) | (2u64 << 20),
            );
            ctx.emit(
                0x0400_0000_0000_0000u64 | ctx.predicate | 249 | (249u64 << 8) | (1u64 << 20),
            );
            ctx.emit(
                0x3848_0000_0000_0000u64 | ctx.predicate | 249 | (249u64 << 8) | (4u64 << 20),
            );
            ctx.emit(
                0x5c28_0000_0000_0000u64 | ctx.predicate | 247 | (247u64 << 8) | (249u64 << 20),
            );
            ctx.emit(
                0x0400_0000_0000_0000u64
                    | ctx.predicate
                    | 247
                    | (247u64 << 8)
                    | (0xffffu64 << 20),
            );
            ctx.emit(
                0x5c28_0000_0000_0000u64 | ctx.predicate | 248 | (248u64 << 8) | (249u64 << 20),
            );
            ctx.emit(
                0x5c18_0000_0000_0000u64
                    | ctx.predicate
                    | ctx.dest
                    | (248u64 << 8)
                    | (247u64 << 20)
                    | (16u64 << 39),
            );
            Some(StepAction::Continue)
        }
        0x23c => {
            // HMMA.1688/.16816.F16: D(16x8) = A(16xK) * B(Kx8) + C, f16 fragments.
            let Some(mode) = ctx.tensor_mode() else {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native matrix multiply (set EDEN_NX2_TENSOR to emulate)".to_owned(),
                ));
            };
            let a = ctx.insn.bits(24, 8);
            let b = ctx.insn.bits(32, 8);
            let c = ctx.insn.bits(64, 8);
            let shape = ctx.insn.words[2] & !0xffu32;
            let depth = if shape == 0 { 8u32 } else { 16u32 };
            let a_count = depth / 4;
            let b_count = depth / 8;
            if (shape != 0 && shape != 0x800)
                || (ctx.insn.words[1] & !0xffu32) != 0
                || (ctx.dest & 1) != 0
                || ctx.dest + 2 > 246
                || (a & (a_count - 1)) != 0
                || a + a_count > 246
                || (b & (b_count - 1)) != 0
                || b + b_count > 246
                || (c != 255 && ((c & 1) != 0 || c + 2 > 246))
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native matrix multiply mode".to_owned(),
                ));
            }
            if mode == "compact" && ctx.native_predicate == 7 {
                ctx.emit(
                    0x5d30_0000_0000_0000u64
                        | ctx.predicate
                        | ctx.dest
                        | ((a as u64) << 8)
                        | ((b as u64) << 20)
                        | ((c as u64) << 39)
                        | (1u64 << 48)
                        | (u64::from(depth == 8) << 49),
                );
                return Some(StepAction::Continue);
            }
            if mode == "stub" {
                let dest_u32 = ctx.dest as u32;
                for row in 0..2u32 {
                    if c == 255 {
                        ctx.mov_imm(dest_u32 + row, 0);
                    } else if c != dest_u32 {
                        ctx.emit(
                            0x5c98_0000_0000_0000u64
                                | ctx.predicate
                                | ((dest_u32 + row) as u64)
                                | (15u64 << 39)
                                | (((c + row) as u64) << 20),
                        );
                    }
                }
                return Some(StepAction::Continue);
            }
            let mut used = [false; 256];
            let mut mark = |reg: u32, count: u32| {
                if reg != 255 {
                    for k in 0..count {
                        used[(reg + k) as usize] = true;
                    }
                }
            };
            mark(ctx.dest as u32, 2);
            mark(a, a_count);
            mark(b, b_count);
            mark(c, 2);
            let mut sums = ctx.shadow_boundary;
            while sums >= 4
                && (used[(sums - 4) as usize]
                    || used[(sums - 3) as usize]
                    || used[(sums - 2) as usize]
                    || used[(sums - 1) as usize])
            {
                sums -= 4;
            }
            if sums < 4 {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native matrix multiply registers".to_owned(),
                ));
            }
            sums -= 4;
            for k in 0..4u32 {
                ctx.emit(native_register_copy(2, sums + k, 0, sums + k));
            }
            for row in 0..2u32 {
                for col in 0..2u32 {
                    ctx.extract_half(
                        HalfSource {
                            reg: if c == 255 { 255 } else { c + row },
                            swizzle: 0,
                            absolute: false,
                            negate: false,
                        },
                        col,
                        sums + row * 2 + col,
                    );
                }
            }
            ctx.emit(0xf0c8_0000_0000_0000u64 | ctx.predicate | 250);
            ctx.emit(
                0x0400_0000_0000_0000u64 | ctx.predicate | 251 | (250u64 << 8) | (0x1cu64 << 20),
            );
            ctx.emit(
                0x0400_0000_0000_0000u64 | ctx.predicate | 252 | (250u64 << 8) | (3u64 << 20),
            );
            ctx.emit(
                0x3848_0000_0000_0000u64 | ctx.predicate | 252 | (252u64 << 8) | (3u64 << 20),
            );
            for pair in 0..(depth / 2) {
                ctx.mov_imm(253, pair & 3);
                emit_add(ctx, 254, 252, 253);
                emit_add(ctx, 253, 251, 253);
                emit_shuffle(ctx, 246, a + if pair >= 4 { 2 } else { 0 }, 253);
                emit_shuffle(ctx, 247, a + if pair >= 4 { 3 } else { 1 }, 253);
                emit_shuffle(ctx, 248, b + pair / 4, 254);
                ctx.mov_imm(253, 4);
                emit_add(ctx, 254, 254, 253);
                emit_shuffle(ctx, 249, b + pair / 4, 254);
                for half in 0..2u32 {
                    ctx.extract_half(
                        HalfSource {
                            reg: 248,
                            swizzle: 0,
                            absolute: false,
                            negate: false,
                        },
                        half,
                        253,
                    );
                    ctx.extract_half(
                        HalfSource {
                            reg: 249,
                            swizzle: 0,
                            absolute: false,
                            negate: false },
                        half,
                        254,
                    );
                    for row in 0..2u32 {
                        ctx.extract_half(
                            HalfSource {
                                reg: 246 + row,
                                swizzle: 0,
                                absolute: false,
                                negate: false,
                            },
                            half,
                            250,
                        );
                        emit_fma(ctx, sums + row * 2, 250, 253);
                        emit_fma(ctx, sums + row * 2 + 1, 250, 254);
                    }
                }
                if (pair & 3) == 3 {
                    for reg in sums..(sums + 4) {
                        emit_round_half(ctx, reg);
                        if pair + 1 < depth / 2 {
                            ctx.extract_half(
                                HalfSource {
                                    reg,
                                    swizzle: 2,
                                    absolute: false,
                                    negate: false,
                                },
                                0,
                                reg,
                            );
                        }
                    }
                }
            }
            for row in 0..2u32 {
                ctx.emit(
                    0x5c18_0000_0000_0000u64
                        | ctx.predicate
                        | (ctx.dest + (row as u64))
                        | (((sums + row * 2 + 1) as u64) << 8)
                        | (((sums + row * 2) as u64) << 20)
                        | (16u64 << 39),
                );
            }
            for k in 0..4u32 {
                ctx.emit(native_register_copy(0, sums + k, 2, sums + k));
            }
            Some(StepAction::Continue)
        }
        0x83b => {
            // LDSM.16.M88.{1,2,4} (ldmatrix): 8x8 16-bit matrices from shared memory.
            let count_bits = ctx.insn.bits(72, 2);
            let count = 1u32 << count_bits;
            let transpose = ctx.insn.bits(78, 1) != 0;
            let uniform_offset = ctx.insn.bits(91, 1) != 0;
            if count_bits == 3
                || (ctx.insn.words[2] & !0x0800_4300u32) != 0
                || (!uniform_offset && ctx.insn.bits(32, 8) != 0)
                || ctx.dest + (count as u64) > 246
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native matrix load mode".to_owned(),
                ));
            }
            let mut address = ctx.insn.bits(24, 8);
            let offset = ctx.insn.bits(40, 24) as u64;
            if uniform_offset {
                let uniform_offset_reg = ctx.uniform_register(ctx.insn.bits(32, 8));
                if uniform_offset_reg == u32::MAX {
                    return Some(StepAction::Fail(
                        ctx.index,
                        "native matrix load uniform offset".to_owned(),
                    ));
                }
                ctx.emit(
                    0x5c10_0000_0000_0000u64
                        | ctx.predicate
                        | 254
                        | ((address as u64) << 8)
                        | ((uniform_offset_reg as u64) << 20),
                );
                address = 254;
            } else if address != 255
                && (address as u64) >= ctx.dest
                && (address as u64) < ctx.dest + (count as u64)
            {
                ctx.emit(
                    0x5c10_0000_0000_0000u64
                        | ctx.predicate
                        | 254
                        | ((address as u64) << 8)
                        | (255u64 << 20),
                );
                address = 254;
            }
            if transpose {
                ctx.emit(0xf0c8_0000_0000_0000u64 | ctx.predicate | 246);
                ctx.emit(
                    0x0400_0000_0000_0000u64 | ctx.predicate | 247 | (246u64 << 8) | (3u64 << 20),
                );
                ctx.emit(
                    0x3848_0000_0000_0000u64 | ctx.predicate | 247 | (247u64 << 8) | (1u64 << 20),
                );
                ctx.emit(
                    0x3828_0000_0000_0000u64 | ctx.predicate | 248 | (246u64 << 8) | (2u64 << 20),
                );
                ctx.emit(
                    0x3848_0000_0000_0000u64 | ctx.predicate | 248 | (248u64 << 8) | (1u64 << 20),
                );
                for matrix in 0..count {
                    for half in 0..2u32 {
                        ctx.emit(
                            0x1c00_0000_0000_0000u64
                                | ctx.predicate
                                | 249
                                | (247u64 << 8)
                                | (((matrix * 8 + half) as u64) << 20),
                        );
                        ctx.emit(
                            0xef10_0000_0000_0000u64
                                | ctx.predicate
                                | 250
                                | ((address as u64) << 8)
                                | (249u64 << 20)
                                | (1u64 << 29)
                                | (31u64 << 34)
                                | (7u64 << 48),
                        );
                        ctx.emit(
                            0x5c10_0000_0000_0000u64
                                | ctx.predicate
                                | 250
                                | (250u64 << 8)
                                | (248u64 << 20),
                        );
                        ctx.emit(
                            0xef48_0000_0000_0000u64
                                | ctx.predicate
                                | ((251 + half) as u64)
                                | (250u64 << 8)
                                | (offset << 20)
                                | (2u64 << 48),
                        );
                    }
                    ctx.emit(
                        0x5c18_0000_0000_0000u64
                            | ctx.predicate
                            | (ctx.dest + (matrix as u64))
                            | (252u64 << 8)
                            | (251u64 << 20)
                            | (16u64 << 39),
                    );
                }
                return Some(StepAction::Continue);
            }
            ctx.emit(0xf0c8_0000_0000_0000u64 | ctx.predicate | 250);
            ctx.emit(
                0x3828_0000_0000_0000u64 | ctx.predicate | 251 | (250u64 << 8) | (2u64 << 20),
            );
            ctx.emit(
                0x0400_0000_0000_0000u64 | ctx.predicate | 250 | (250u64 << 8) | (3u64 << 20),
            );
            ctx.emit(
                0x3848_0000_0000_0000u64 | ctx.predicate | 250 | (250u64 << 8) | (2u64 << 20),
            );
            for matrix in 0..count {
                let mut lane = 251u32;
                if matrix != 0 {
                    ctx.emit(
                        0x1c00_0000_0000_0000u64
                            | ctx.predicate
                            | 252
                            | (251u64 << 8)
                            | (((matrix * 8) as u64) << 20),
                    );
                    lane = 252;
                }
                ctx.emit(
                    0xef10_0000_0000_0000u64
                        | ctx.predicate
                        | 253
                        | ((address as u64) << 8)
                        | ((lane as u64) << 20)
                        | (1u64 << 29)
                        | (31u64 << 34)
                        | (7u64 << 48),
                );
                ctx.emit(
                    0x5c10_0000_0000_0000u64
                        | ctx.predicate
                        | 253
                        | (253u64 << 8)
                        | (250u64 << 20),
                );
                ctx.emit(
                    0xef48_0000_0000_0000u64
                        | ctx.predicate
                        | (ctx.dest + (matrix as u64))
                        | (253u64 << 8)
                        | (offset << 20)
                        | (4u64 << 48),
                );
            }
            Some(StepAction::Continue)
        }
        _ => None,
    }
}
