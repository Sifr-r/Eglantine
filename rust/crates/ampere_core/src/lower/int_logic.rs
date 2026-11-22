// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use super::{LowerCtx, StepAction};

#[inline]
fn fail(ctx: &LowerCtx<'_>, reason: &str) -> Option<StepAction> {
    Some(StepAction::Fail(ctx.index, reason.to_owned()))
}

pub(super) fn try_lower(ctx: &mut LowerCtx<'_>) -> Option<StepAction> {
    match ctx.opcode {
        0xc24 | 0xe24 => {
            // IMAD low word with a uniform multiplier or addend.
            // 0xe24 keeps the uniform addend in bits 32-37; bit 63 negates it ("RZ, RZ, -UR4").
            let negate_c = ctx.opcode == 0xe24 && ctx.insn.bits(63, 1) != 0;
            if (ctx.insn.words[2] & !0x0800_02ffu32) != 0x078e_0000
                || (ctx.insn.words[1] & !(0xffu32 | if negate_c { 0x8000_0000u32 } else { 0u32 }))
                    != 0
            {
                return fail(ctx, "native uniform-addend multiply mode");
            }
            let a = ctx.operand(0, false);
            let b = ctx.operand(1, false);
            let mut c = ctx.operand(2, false);
            if negate_c {
                ctx.emit(
                    0x5c10_0000_0000_0000u64
                        | ctx.predicate
                        | 252
                        | (255u64 << 8)
                        | ((c as u64) << 20)
                        | (1u64 << 48),
                );
                c = 252;
            }
            ctx.multiply_low(a, b, c, false);
            Some(StepAction::Continue)
        }
        0x224 => {
            // IMAD register / zero-product move
            if ctx.insn.bits(24, 8) == 255
                && ctx.insn.words[1] == 255
                && ctx.insn.bits(75, 1) != 0
                && (ctx.insn.words[2] & !0xaffu32) == 0x078e_0000
            {
                ctx.emit(
                    ctx.base(0x5c10_0000_0000_0000u64)
                        | (255u64 << 8)
                        | ((ctx.insn.bits(64, 8) as u64) << 20)
                        | (1u64 << 48),
                );
                return Some(StepAction::Continue);
            }
            if ctx.insn.bits(24, 8) == 255
                && ctx.insn.words[1] == 255
                && (ctx.insn.words[2] & !0xffu32) == 0x078e_0000
            {
                ctx.emit(
                    ctx.base(0x5c98_0000_0000_0000u64)
                        | (15u64 << 39)
                        | ((ctx.insn.bits(64, 8) as u64) << 20),
                );
                return Some(StepAction::Continue);
            }
            if ctx.insn.bits(24, 8) == 255
                && ctx.insn.words[1] == 255
                && (ctx.insn.words[2] & !0x0780_0affu32) == 0x000e_0400
                && (!ctx.rewritten_uniform || ctx.insn.bits(87, 3) == 7)
            {
                // IMAD.X RZ, RZ, c, Pn: the high word of a 64-bit add (bit 75 inverts c).
                let mut c = ctx.insn.bits(64, 8);
                ctx.mov_imm(250, 1);
                ctx.emit(
                    0x5ca0_0000_0000_0000u64
                        | ctx.predicate
                        | 250
                        | (250u64 << 8)
                        | (255u64 << 20)
                        | ((ctx.insn.bits(87, 3) as u64) << 39)
                        | ((ctx.insn.bits(90, 1) as u64) << 42),
                );
                if ctx.insn.bits(75, 1) != 0 {
                    ctx.emit(
                        0x0400_0000_0000_0000u64
                            | ctx.predicate
                            | 251
                            | ((c as u64) << 8)
                            | (0xffff_ffffu64 << 20)
                            | (2u64 << 53),
                    );
                    c = 251;
                }
                ctx.emit(
                    ctx.base(0x5c10_0000_0000_0000u64) | ((c as u64) << 8) | (250u64 << 20),
                );
                return Some(StepAction::Continue);
            }
            if (ctx.insn.words[2] & !0x2ffu32) != 0x078e_0000 || (ctx.insn.words[1] & !0xffu32) != 0
            {
                return fail(ctx, "native integer register multiply mode");
            }
            ctx.multiply_low(
                ctx.insn.bits(24, 8),
                ctx.insn.bits(32, 8),
                ctx.insn.bits(64, 8),
                false,
            );
            Some(StepAction::Continue)
        }
        0x2a5 => {
            // UIMAD.WIDE.U32 UR(d):UR(d+1) = URa * URb + URc:UR(c+1)
            let a = ctx.uniform_register(ctx.insn.bits(24, 8));
            let b = ctx.uniform_register(ctx.insn.bits(32, 8));
            let c = ctx.insn.bits(64, 8);
            let c_low = ctx.uniform_register(c);
            let c_high = if c == 63 {
                255
            } else {
                ctx.uniform_register(c + 1)
            };
            let d_low = ctx.uniform_register(ctx.insn.bits(16, 8));
            let d_high = ctx.uniform_register(ctx.insn.bits(16, 8) + 1);
            if ctx.native_predicate != 7
                || (ctx.insn.words[1] & !0xffu32) != 0
                || (ctx.insn.words[2] & !0xffu32) != 0x0f8e_0000
                || (ctx.insn.bits(16, 8) & 1) != 0
                || (c != 63 && (c & 1) != 0)
                || a == u32::MAX
                || b == u32::MAX
                || c_low == u32::MAX
                || c_high == u32::MAX
                || d_low == u32::MAX
                || d_high == u32::MAX
                || d_low == 255
            {
                return fail(ctx, "native uniform wide multiply mode");
            }
            // High word in R253 (base-2^16 long multiplication, as IMAD.HI), low word in R250.
            ctx.xmad(250, a, b, 255, false, false, 0, false);
            ctx.xmad(251, a, b, 250, true, false, 2, false);
            ctx.xmad(250, a, b, 251, false, true, 1, false);
            ctx.xmad(252, a, b, 251, true, true, 2, false);
            ctx.emit(0x3828_0000_0007_0000u64 | 250 | (250u64 << 8) | (16u64 << 20));
            ctx.emit(0x5c10_0000_0007_0000u64 | 253 | (250u64 << 8) | (252u64 << 20));
            ctx.xmad(250, a, b, 255, false, false, 0, false);
            ctx.xmad(251, a, b, 255, true, false, 0, false);
            ctx.xmad(251, a, b, 251, false, true, 0, false);
            ctx.emit(
                0x5c18_0000_0007_0000u64 | 250 | (251u64 << 8) | (250u64 << 20) | (16u64 << 39),
            );
            ctx.emit(
                0x5c10_0000_0007_0000u64
                    | 250
                    | (250u64 << 8)
                    | ((c_low as u64) << 20)
                    | (1u64 << 47),
            );
            ctx.emit(
                0x5c10_0000_0007_0000u64
                    | (d_high as u64)
                    | (253u64 << 8)
                    | ((c_high as u64) << 20)
                    | (1u64 << 43),
            );
            ctx.emit(0x5c10_0000_0007_0000u64 | (d_low as u64) | (250u64 << 8) | (255u64 << 20));
            Some(StepAction::Continue)
        }
        0x226 => {
            // IDP.2A.LO.U16.U8: a.lo16 * b.byte0 + a.hi16 * b.byte1 + c
            if (ctx.insn.words[2] & !0xffu32) != 0x1000 || (ctx.insn.words[1] & !0xffu32) != 0 {
                return fail(ctx, "native packed dot-product mode");
            }
            let a = ctx.insn.bits(24, 8);
            let b = ctx.insn.bits(32, 8);
            let c = ctx.insn.bits(64, 8);
            for component in 0..2u32 {
                ctx.emit(
                    0x3828_0000_0000_0000u64
                        | ctx.predicate
                        | ((250 + component) as u64)
                        | ((b as u64) << 8)
                        | (((component * 8) as u64) << 20),
                );
                ctx.emit(
                    0x0400_0000_0000_0000u64
                        | ctx.predicate
                        | ((250 + component) as u64)
                        | (((250 + component) as u64) << 8)
                        | (0xffu64 << 20),
                );
            }
            ctx.xmad(252, a, 250, c, false, false, 0, false);
            ctx.xmad(ctx.dest as u32, a, 251, 252, true, false, 0, false);
            Some(StepAction::Continue)
        }
        0x225 | 0x227 | 0x625 | 0x825 | 0x827 | 0xa27 => {
            // IMAD.HI Rd / IMAD.WIDE Rd:Rd+1 = a * b + c:c+1 (.U32 or signed, bit 73), with an optional
            // carry-out (bits 81-83). b is a register, an immediate (0x8xx) or a cbuf (0xa27); 0x625 takes
            // b from bits 64-71 and adds a cbuf pair.
            let wide = matches!(ctx.opcode, 0x225 | 0x625 | 0x825);
            let immediate = matches!(ctx.opcode, 0x825 | 0x827);
            let signed_product = ctx.insn.bits(73, 1) != 0;
            let c = if ctx.opcode == 0x625 {
                255
            } else {
                ctx.insn.bits(64, 8)
            };
            let carry_output = ctx.insn.bits(81, 3);
            if (ctx.insn.words[2] & !0x000e_02ffu32) != 0x0780_0000
                || ((ctx.opcode == 0x225 || ctx.opcode == 0x227)
                    && (ctx.insn.words[1] & !0xffu32) != 0)
                || ((ctx.opcode == 0xa27 || ctx.opcode == 0x625)
                    && (!ctx.valid_cbuf() || ctx.insn.bits(62, 2) != 0))
                || (ctx.opcode == 0x625 && ctx.insn.bits(38, 16) > 0xfff8)
                || (c != 255 && (c & 1) != 0)
                || (wide && (ctx.dest == 255 || (ctx.dest & 1) != 0))
                || (signed_product && carry_output != 7)
            {
                return fail(ctx, "native high multiply mode");
            }
            let a = ctx.insn.bits(24, 8);
            let b = if immediate {
                ctx.insn.words[1]
            } else if ctx.opcode == 0xa27 {
                254
            } else {
                ctx.insn.bits(if ctx.opcode == 0x625 { 64 } else { 32 }, 8)
            };
            if ctx.opcode == 0xa27 {
                ctx.emit(
                    0x4c98_0000_0000_0000u64
                        | ctx.predicate
                        | 254
                        | (15u64 << 39)
                        | ctx.cbuf(),
                );
            }
            if signed_product {
                // The signed high word is the unsigned one minus (a < 0 ? b : 0) + (b < 0 ? a : 0),
                // which R253 holds (R252 is free until the multiplication).
                let b_register = if immediate { 254 } else { b };
                if immediate {
                    ctx.mov_imm(254, b);
                }
                ctx.emit(
                    0x3828_0000_0000_0000u64
                        | ctx.predicate
                        | 253
                        | ((a as u64) << 8)
                        | (31u64 << 20)
                        | (1u64 << 48),
                );
                ctx.emit(
                    0x5c40_0000_0000_0000u64
                        | ctx.predicate
                        | 253
                        | (253u64 << 8)
                        | ((b_register as u64) << 20)
                        | (7u64 << 48),
                );
                ctx.emit(
                    0x3828_0000_0000_0000u64
                        | ctx.predicate
                        | 252
                        | ((b_register as u64) << 8)
                        | (31u64 << 20)
                        | (1u64 << 48),
                );
                ctx.emit(
                    0x5c40_0000_0000_0000u64
                        | ctx.predicate
                        | 252
                        | (252u64 << 8)
                        | ((a as u64) << 20)
                        | (7u64 << 48),
                );
                ctx.emit(
                    0x5c10_0000_0000_0000u64 | ctx.predicate | 253 | (253u64 << 8) | (252u64 << 20),
                );
            }
            // Base-2^16 long multiplication. Each intermediate sum fits U32.
            ctx.xmad(250, a, b, 255, false, false, 0, immediate);
            ctx.xmad(251, a, b, 250, true, false, 2, immediate);
            ctx.xmad(250, a, b, 251, false, true, 1, immediate);
            ctx.xmad(252, a, b, 251, true, true, 2, immediate);
            ctx.emit(
                0x3828_0000_0000_0000u64 | ctx.predicate | 250 | (250u64 << 8) | (16u64 << 20),
            );
            if signed_product {
                ctx.emit(
                    0x5c10_0000_0000_0000u64
                        | ctx.predicate
                        | 250
                        | (250u64 << 8)
                        | (253u64 << 20)
                        | (1u64 << 48),
                );
            }
            if !wide && c == 255 && carry_output == 7 {
                ctx.emit(ctx.base(0x5c10_0000_0000_0000u64) | (250u64 << 8) | (252u64 << 20));
                return Some(StepAction::Continue);
            }
            // High word in R253 and low word in R250 (as UIMAD.WIDE.U32), then c:c+1 is added.
            ctx.emit(
                0x5c10_0000_0000_0000u64 | ctx.predicate | 253 | (250u64 << 8) | (252u64 << 20),
            );
            ctx.xmad(250, a, b, 255, false, false, 0, immediate);
            ctx.xmad(251, a, b, 255, true, false, 0, immediate);
            ctx.xmad(251, a, b, 251, false, true, 0, immediate);
            ctx.emit(
                0x5c18_0000_0000_0000u64
                    | ctx.predicate
                    | 250
                    | (251u64 << 8)
                    | (250u64 << 20)
                    | (16u64 << 39),
            );
            let mut c_low = c;
            let mut c_high = if c == 255 { 255 } else { c + 1 };
            if ctx.opcode == 0x625 {
                ctx.emit(
                    0x4c98_0000_0000_0000u64
                        | ctx.predicate
                        | 252
                        | (15u64 << 39)
                        | ctx.cbuf(),
                );
                ctx.emit(
                    0x4c98_0000_0000_0000u64
                        | ctx.predicate
                        | 254
                        | (15u64 << 39)
                        | (ctx.cbuf() + (1u64 << 20)),
                );
                c_low = 252;
                c_high = 254;
            }
            let high = if wide {
                (ctx.dest as u32) + 1
            } else {
                ctx.dest as u32
            };
            ctx.emit(
                0x5c10_0000_0000_0000u64
                    | ctx.predicate
                    | 250
                    | (250u64 << 8)
                    | ((c_low as u64) << 20)
                    | (1u64 << 47),
            );
            if carry_output == 7 {
                ctx.emit(
                    0x5c10_0000_0000_0000u64
                        | ctx.predicate
                        | (high as u64)
                        | (253u64 << 8)
                        | ((c_high as u64) << 20)
                        | (1u64 << 43),
                );
                if wide {
                    ctx.emit(ctx.base(0x5c10_0000_0000_0000u64) | (250u64 << 8) | (255u64 << 20));
                }
                return Some(StepAction::Continue);
            }
            // Maxwell IADD has no .X with .CC: R251 takes the low carry, R254 counts the high word's.
            ctx.emit(
                0x5c10_0000_0000_0000u64
                    | ctx.predicate
                    | 251
                    | (255u64 << 8)
                    | (255u64 << 20)
                    | (1u64 << 43),
            );
            ctx.emit(
                0x5c10_0000_0000_0000u64
                    | ctx.predicate
                    | 253
                    | (253u64 << 8)
                    | ((c_high as u64) << 20)
                    | (1u64 << 47),
            );
            ctx.emit(
                0x5c10_0000_0000_0000u64
                    | ctx.predicate
                    | 254
                    | (255u64 << 8)
                    | (255u64 << 20)
                    | (1u64 << 43),
            );
            ctx.emit(
                0x5c10_0000_0000_0000u64
                    | ctx.predicate
                    | 253
                    | (253u64 << 8)
                    | (251u64 << 20)
                    | (1u64 << 47),
            );
            ctx.emit(
                0x5c10_0000_0000_0000u64
                    | ctx.predicate
                    | 254
                    | (254u64 << 8)
                    | (255u64 << 20)
                    | (1u64 << 43),
            );
            ctx.emit(
                0x5c98_0000_0000_0000u64
                    | ctx.predicate
                    | (high as u64)
                    | (253u64 << 20)
                    | (15u64 << 39),
            );
            if wide {
                ctx.emit(ctx.base(0x5c98_0000_0000_0000u64) | (250u64 << 20) | (15u64 << 39));
            }
            ctx.emit(
                0x5b60_0000_0000_0000u64
                    | ctx.predicate
                    | 7
                    | ((carry_output as u64) << 3)
                    | (254u64 << 8)
                    | (255u64 << 20)
                    | (7u64 << 39)
                    | (5u64 << 49),
            );
            Some(StepAction::Continue)
        }
        0xa24 => {
            // IMAD, constant-buffer multiplier
            if (ctx.insn.words[2] & !0x2ffu32) != 0x078e_0000
                || !ctx.valid_cbuf()
                || ctx.insn.bits(62, 2) != 0
            {
                return fail(ctx, "native integer constant-buffer multiply mode");
            }
            ctx.emit(
                0x4c98_0000_0000_0000u64 | ctx.predicate | 252 | (15u64 << 39) | ctx.cbuf(),
            );
            ctx.multiply_low(ctx.insn.bits(24, 8), 252, ctx.insn.bits(64, 8), false);
            Some(StepAction::Continue)
        }
        0x424 | 0x624 => {
            // IMAD.MOV.U32: zero product plus immediate/cbuf
            if ctx.opcode == 0x624
                && ctx.insn.bits(24, 8) == 255
                && (ctx.insn.words[2] & !0x0780_0000u32) == 0x000e_06ff
            {
                let source = ctx.operand(2, false);
                ctx.mov_imm(250, 1);
                ctx.emit(
                    0x5ca0_0000_0000_0000u64
                        | ctx.predicate
                        | 250
                        | (250u64 << 8)
                        | (255u64 << 20)
                        | ((ctx.insn.bits(87, 3) as u64) << 39)
                        | ((ctx.insn.bits(90, 1) as u64) << 42),
                );
                ctx.emit(
                    ctx.base(0x5c10_0000_0000_0000u64) | ((source as u64) << 8) | (250u64 << 20),
                );
                return Some(StepAction::Continue);
            }
            if ctx.insn.bits(24, 8) != 255
                || ctx.insn.bits(64, 8) != 255
                || ctx.insn.words[2] != 0x078e_00ff
            {
                if (ctx.insn.words[2] & !0x2ffu32) != 0x078e_0000
                    || (ctx.opcode == 0x624 && ctx.insn.bits(62, 2) != 0)
                {
                    return fail(ctx, "native multiply wide addend mode");
                }
                let a = ctx.operand(0, false);
                let b = ctx.operand(1, false);
                let c = ctx.operand(2, false);
                ctx.multiply_low(a, b, c, false);
                return Some(StepAction::Continue);
            }
            if ctx.insn.bits(24, 8) != 255
                || ctx.insn.bits(64, 8) != 255
                || ctx.insn.words[2] != 0x078e_00ff
            {
                return fail(ctx, "native integer move mode");
            }
            if ctx.opcode == 0x624 {
                if !ctx.valid_cbuf() || ctx.insn.bits(62, 2) != 0 {
                    return fail(ctx, "native integer move constant buffer");
                }
                ctx.emit(ctx.base(0x4c98_0000_0000_0000u64) | (15u64 << 39) | ctx.cbuf());
            } else {
                ctx.emit(
                    ctx.base(0x0100_0000_0000_0000u64)
                        | (15u64 << 12)
                        | ((ctx.insn.words[1] as u64) << 20),
                );
            }
            Some(StepAction::Continue)
        }
        0x824 => {
            // Captured low-word IMAD forms; bit 75 negates the addend.
            let negate_c = ctx.insn.bits(75, 1) != 0;
            if ctx.insn.words[1] == 1
                && (ctx.insn.words[2] & !(0x0780_0800u32 | 0xffu32)) == 0x000e_0600
            {
                // IMAD.X Rd, Ra, 1, Rc, Pn: Ra + Rc plus the carry predicate in bits 87-90 (~Rc with bit 75).
                let mut c = ctx.insn.bits(64, 8);
                ctx.mov_imm(252, 1);
                ctx.emit(
                    0x5ca0_0000_0000_0000u64
                        | ctx.predicate
                        | 252
                        | (252u64 << 8)
                        | (255u64 << 20)
                        | ((ctx.insn.bits(87, 3) as u64) << 39)
                        | ((ctx.insn.bits(90, 1) as u64) << 42),
                );
                if negate_c {
                    ctx.emit(
                        0x0400_0000_0000_0000u64
                            | ctx.predicate
                            | 251
                            | ((c as u64) << 8)
                            | (0xffff_ffffu64 << 20)
                            | (2u64 << 53),
                    );
                    c = 251;
                }
                ctx.emit(
                    0x5c10_0000_0000_0000u64
                        | ctx.predicate
                        | 250
                        | ((ctx.insn.bits(24, 8) as u64) << 8)
                        | ((c as u64) << 20),
                );
                ctx.emit(ctx.base(0x5c10_0000_0000_0000u64) | (250u64 << 8) | (252u64 << 20));
                return Some(StepAction::Continue);
            }
            if ctx.insn.words[1] == 1 && (ctx.insn.words[2] & 0xffff_f700u32) == 0x078e_0200 {
                ctx.emit(
                    ctx.base(0x5c10_0000_0000_0000u64)
                        | ((ctx.insn.bits(24, 8) as u64) << 8)
                        | ((ctx.insn.bits(64, 8) as u64) << 20)
                        | ((negate_c as u64) << 48),
                );
                return Some(StepAction::Continue);
            }
            let multiplier = ctx.insn.words[1];
            if ctx.insn.bits(64, 8) == 255
                && !negate_c
                && multiplier > 1
                && (multiplier & (multiplier - 1)) == 0
                && (ctx.insn.words[2] & !0x2ffu32) == 0x078e_0000
            {
                // IMAD.SHL by a power of two: one shift (also the form Eden's jump-table tracking
                // expects between the clamp and the table load).
                let shift = multiplier.trailing_zeros();
                ctx.emit(
                    ctx.base(0x3848_0000_0000_0000u64)
                        | ((ctx.insn.bits(24, 8) as u64) << 8)
                        | ((shift as u64) << 20),
                );
                return Some(StepAction::Continue);
            }
            if ctx.insn.words[1] != 48 || ctx.insn.words[2] != 0x078e_02ff {
                if (ctx.insn.words[2] & !0xaffu32) != 0x078e_0000 {
                    return fail(ctx, "native integer multiply mode");
                }
                let mut c = ctx.insn.bits(64, 8);
                if negate_c && c != 255 {
                    ctx.emit(
                        0x5c10_0000_0000_0000u64
                            | ctx.predicate
                            | 252
                            | (255u64 << 8)
                            | ((c as u64) << 20)
                            | (1u64 << 48),
                    );
                    c = 252;
                }
                ctx.multiply_low(ctx.insn.bits(24, 8), ctx.insn.words[1], c, true);
                return Some(StepAction::Continue);
            }
            let source = ctx.insn.bits(24, 8) as u64;
            ctx.emit(
                0x5c18_0000_0000_0000u64
                    | ctx.predicate
                    | 250
                    | (source << 8)
                    | (source << 20)
                    | (1u64 << 39),
            );
            ctx.emit(ctx.base(0x3848_0000_0000_0000u64) | (250u64 << 8) | (4u64 << 20));
            Some(StepAction::Continue)
        }
        0x806 => {
            // VOTE: register ballot, ordinary predicate input and output.
            if ctx.insn.words[1] != 0
                || ctx.insn.bits(72, 2) > 2
                || (ctx.insn.words[2] & !0x078e_0300u32) != 0
            {
                return fail(ctx, "native vote mode");
            }
            ctx.emit(
                ctx.base(0x50d8_0000_0000_0000u64)
                    | ((ctx.insn.bits(87, 4) as u64) << 39)
                    | ((ctx.insn.bits(81, 3) as u64) << 45)
                    | ((ctx.insn.bits(72, 2) as u64) << 48),
            );
            Some(StepAction::Continue)
        }
        0x886 => {
            // VOTEU ballot and independent uniform predicate output.
            let output = ctx.insn.bits(81, 3);
            let target = ctx.insn.bits(16, 8);
            if (target >= 28 && target != 63)
                || ctx.insn.words[1] != 0
                || ctx.insn.bits(72, 2) > 2
                || ctx.native_predicate != 7
            {
                return fail(ctx, "native uniform vote mode");
            }
            let temporary = if output == 7 { 7 } else { 6 - output };
            let mask = 1u32 << temporary;
            if output != 7 {
                ctx.emit(0x38e8_0000_0007_0000u64 | 253 | (255u64 << 8) | ((mask as u64) << 20));
            }
            let uniform_target = ctx.uniform_register(target);
            ctx.emit(
                0x50d8_0000_0007_0000u64
                    | (uniform_target as u64)
                    | ((ctx.insn.bits(87, 4) as u64) << 39)
                    | ((temporary as u64) << 45)
                    | ((ctx.insn.bits(72, 2) as u64) << 48),
            );
            if output != 7 {
                ctx.emit(0x38e8_0000_0007_0000u64 | 239 | (239u64 << 8) | ((mask as u64) << 20));
                ctx.emit(0x38f0_0000_0007_0000u64 | (253u64 << 8) | ((mask as u64) << 20));
            }
            Some(StepAction::Continue)
        }
        0x3c2 => {
            // R2UR: broadcast the first lane satisfying the execution predicate.
            let target = ctx.uniform_register(ctx.dest as u32);
            if target == u32::MAX
                || target == 255
                || ctx.insn.words[1] != 0
                || ctx.insn.words[2] != 0x000e_0000
            {
                return fail(ctx, "native register-to-uniform mode");
            }
            ctx.save_predicate(1 << 6);
            ctx.emit(
                0x50d8_0000_0007_0000u64
                    | 250
                    | ((ctx.native_predicate as u64) << 39)
                    | (6u64 << 45)
                    | (1u64 << 48),
            );
            ctx.emit(
                0x5c10_0000_0007_0000u64 | 251 | (250u64 << 8) | (255u64 << 20) | (1u64 << 49),
            );
            ctx.emit(
                0x5c40_0000_0007_0000u64 | 250 | (250u64 << 8) | (251u64 << 20) | (7u64 << 48),
            );
            ctx.emit(0x5c30_0000_0007_0000u64 | 250 | (250u64 << 20));
            ctx.emit(
                0xef10_0000_0007_0000u64
                    | 251
                    | ((ctx.insn.bits(24, 8) as u64) << 8)
                    | (250u64 << 20)
                    | (1u64 << 29)
                    | (31u64 << 34)
                    | (7u64 << 48),
            );
            if ctx.native_predicate == 7 {
                // Every lane executing it is in the ballot, so no lane-less fallback can be taken.
                // Leaving the old value selectable would make it a source of the result for the
                // texture and global memory tracking (a handle traced to the uniform's last load).
                ctx.emit(
                    0x5c98_0000_0007_0000u64 | (target as u64) | (15u64 << 39) | (251u64 << 20),
                );
            } else {
                ctx.emit(
                    0x5ca0_0000_0007_0000u64
                        | (target as u64)
                        | (251u64 << 8)
                        | ((target as u64) << 20)
                        | (6u64 << 39),
                );
            }
            ctx.restore_predicate(1 << 6);
            Some(StepAction::Continue)
        }
        0x81c => {
            // PLOP3.LUT; bit 67 makes the third source a uniform predicate.
            let target = ctx.insn.bits(81, 3);
            let second_target = ctx.insn.bits(84, 3);
            let lut = ctx.insn.bits(64, 3) | (ctx.insn.bits(72, 5) << 3);
            if ctx.insn.words[1] != 0 || (target != 7 && target == second_target) {
                return fail(ctx, "native predicate LUT mode");
            }
            if target == 7 && second_target == 7 {
                return Some(StepAction::Continue);
            }
            if second_target != 7 {
                ctx.predicate_lut(
                    lut,
                    ctx.insn.bits(87, 4),
                    ctx.insn.bits(77, 4),
                    ctx.insn.bits(68, 4),
                    target,
                    ctx.insn.bits(67, 1) != 0,
                    second_target,
                    ctx.insn.bits(16, 8),
                );
                return Some(StepAction::Continue);
            }
            if ctx.insn.bits(67, 1) == 0 && (lut == 0xfe || lut == 0xa8) {
                ctx.emit(
                    0x5090_0000_0000_0000u64
                        | ctx.predicate
                        | ((target as u64) << 3)
                        | 7
                        | ((ctx.insn.bits(87, 4) as u64) << 12)
                        | (1u64 << 24)
                        | ((ctx.insn.bits(77, 4) as u64) << 29)
                        | ((ctx.insn.bits(68, 4) as u64) << 39)
                        | (((lut == 0xfe) as u64) << 45),
                );
                return Some(StepAction::Continue);
            }
            if ctx.insn.bits(67, 1) == 0
                && (lut == 0x80
                    || (lut == 8 && ctx.insn.bits(68, 4) == 7 && ctx.insn.bits(77, 4) == 7))
            {
                ctx.emit(
                    0x5090_0000_0000_0000u64
                        | ctx.predicate
                        | ((target as u64) << 3)
                        | 7
                        | (((ctx.insn.bits(87, 4) ^ if lut == 8 { 8 } else { 0 }) as u64) << 12)
                        | ((ctx.insn.bits(77, 4) as u64) << 29)
                        | ((ctx.insn.bits(68, 4) as u64) << 39),
                );
                return Some(StepAction::Continue);
            }
            ctx.predicate_lut(
                lut,
                ctx.insn.bits(87, 4),
                ctx.insn.bits(77, 4),
                ctx.insn.bits(68, 4),
                target,
                ctx.insn.bits(67, 1) != 0,
                7,
                0,
            );
            Some(StepAction::Continue)
        }
        0x89c => {
            // UPLOP3.LUT: load the uniform predicates into P0-P6, combine, merge back.
            let target = ctx.insn.bits(81, 3);
            let lut = ctx.insn.bits(64, 3) | (ctx.insn.bits(72, 5) << 3);
            if ctx.insn.bits(84, 3) != 7
                || ctx.insn.bits(67, 1) != 0
                || ctx.insn.bits(16, 8) != 0
                || ctx.insn.words[1] != 0
            {
                return fail(ctx, "native uniform predicate LUT mode");
            }
            if target == 7 {
                return Some(StepAction::Continue);
            }
            let slot = |operand: u32| -> u32 {
                if (operand & 7) == 7 {
                    operand
                } else {
                    (operand & 8) | (6 - (operand & 7))
                }
            };
            ctx.predicate = (slot(ctx.native_predicate) as u64) << 16; // a UPn guard is loaded as well
            ctx.emit(0x38e8_0000_0007_0000u64 | 254 | (255u64 << 8) | (0x7fu64 << 20));
            ctx.emit(0x38f0_0000_0007_0000u64 | (239u64 << 8) | (0x7fu64 << 20));
            ctx.predicate_lut(
                lut,
                slot(ctx.insn.bits(87, 4)),
                slot(ctx.insn.bits(77, 4)),
                slot(ctx.insn.bits(68, 4)),
                6 - target,
                false,
                7,
                0,
            );
            ctx.emit(
                0x38e8_0000_0007_0000u64 | 239 | (239u64 << 8) | ((1u64 << (6 - target)) << 20),
            );
            ctx.emit(0x38f0_0000_0007_0000u64 | (254u64 << 8) | (0x7fu64 << 20));
            Some(StepAction::Continue)
        }
        0x883 => {
            // UP2UR UR, UPR, URZ, mask: bit k of the result is UPk (R239 bit 6-k).
            let target = ctx.uniform_register(ctx.insn.bits(16, 8));
            let mask = ctx.insn.words[1];
            if target == u32::MAX
                || ctx.insn.bits(24, 8) != 63
                || ctx.native_predicate != 7
                || ctx.insn.words[2] != 0x0800_0000
                || mask > 0x7f
            {
                return fail(ctx, "native uniform predicate move mode");
            }
            if target == 255 {
                return Some(StepAction::Continue);
            }
            ctx.mov_imm(target, 0);
            for k in 0..7u32 {
                if ((mask >> k) & 1) == 0 {
                    continue;
                }
                ctx.emit(
                    0x3800_0000_0007_0000u64
                        | 250
                        | (239u64 << 8)
                        | (((6 - k) | (1u32 << 8)) as u64) << 20,
                );
                ctx.emit(
                    0x36f0_0000_0007_0000u64
                        | (target as u64)
                        | (250u64 << 8)
                        | ((k | (1u32 << 8)) as u64) << 20
                        | ((target as u64) << 39),
                );
            }
            Some(StepAction::Continue)
        }
        0x882 => {
            // UMOV immediate, into any uniform register (Hyrule Warriors: @!UP0 UMOV UR59, 0x20).
            let target = ctx.uniform_register(ctx.dest as u32);
            if target == u32::MAX || ctx.insn.words[2] != 0 {
                return fail(ctx, "native uniform immediate move mode");
            }
            if target != 255 {
                ctx.mov_imm(target, ctx.insn.words[1]);
            }
            Some(StepAction::Continue)
        }
        0xc82 => {
            // UMOV UR, UR (or URZ), including uniform execution predicates.
            // Any uniform destination (Hyrule Warriors: @UP0 UMOV UR59, UR58).
            let source = if ctx.insn.words[1] < 64 {
                ctx.uniform_register(ctx.insn.words[1])
            } else {
                u32::MAX
            };
            let target = ctx.uniform_register(ctx.dest as u32);
            if target == u32::MAX || source == u32::MAX || ctx.insn.words[2] != 0x0800_0000 {
                return fail(ctx, "native uniform move mode");
            }
            if target != 255 {
                ctx.emit(
                    0x5c98_0000_0000_0000u64
                        | ctx.predicate
                        | (target as u64)
                        | ((source as u64) << 20)
                        | (15u64 << 39),
                );
            }
            Some(StepAction::Continue)
        }
        0x899 => {
            // USHF.L.U32; accept a zero word outside the selected half.
            let dst = ctx.insn.bits(16, 8);
            let low = ctx.insn.bits(24, 8);
            let high = ctx.insn.bits(64, 8);
            let dst_reg = ctx.uniform_register(dst);
            let low_reg = ctx.uniform_register(low);
            let high_reg = ctx.uniform_register(high);
            let dst_ok = dst != 63 && dst_reg != u32::MAX;
            let low_ok = low != 63 && low_reg != u32::MAX;
            let high_ok = high != 63 && high_reg != u32::MAX;
            if dst_ok
                && low_reg != u32::MAX
                && high_reg != u32::MAX
                && ctx.insn.words[2] == (0x0800_1200u32 | high)
            {
                ctx.mov_imm(252, ctx.insn.words[1]);
                ctx.right_shift_u64_low(dst_reg, low_reg, high_reg, 252);
                return Some(StepAction::Continue);
            }
            // A UPn guard (Hyrule Warriors: @UP1 USHF.R.U32.HI) is swapped into the predicate above.
            if ctx.insn.words[2] == (0x0801_1400u32 | high)
                && low == 63
                && high_ok
                && dst_ok
                && ctx.insn.words[1] <= 32
            {
                ctx.emit(
                    0x3828_0000_0000_0000u64
                        | ctx.predicate
                        | (dst_reg as u64)
                        | ((high_reg as u64) << 8)
                        | ((ctx.insn.words[1] as u64) << 20)
                        | (1u64 << 48),
                );
                return Some(StepAction::Continue);
            }
            // Logical right shifts of one register (bit 76 right, bit 80 high word, bits 73-74
            // U32): USHF.R.U32 URd, URa, n, URZ and USHF.R.U32.HI URd, URZ, n, URb.
            let right_low = ctx.insn.words[2] == 0x0800_163f && low_ok;
            let right_high =
                ctx.insn.words[2] == (0x0801_1600u32 | high) && low == 63 && high_ok;
            if (right_low || right_high) && dst_ok && ctx.insn.words[1] <= 32 {
                ctx.emit(
                    0x3828_0000_0000_0000u64
                        | ctx.predicate
                        | (dst_reg as u64)
                        | (((if right_high { high_reg } else { low_reg }) as u64) << 8)
                        | ((ctx.insn.words[1] as u64) << 20),
                );
                return Some(StepAction::Continue);
            }
            let high_result =
                ctx.insn.words[2] == (0x0801_0600u32 | high) && low == 63 && high_ok;
            let low_result = ctx.insn.words[2] == 0x0800_063f && low_ok;
            if !dst_ok || ctx.insn.words[1] > 32 || (!high_result && !low_result) {
                return fail(ctx, "native uniform shift mode");
            }
            ctx.emit(
                0x3848_0000_0000_0000u64
                    | ctx.predicate
                    | (dst_reg as u64)
                    | (((if high_result { high_reg } else { low_reg }) as u64) << 8)
                    | ((ctx.insn.words[1] as u64) << 20),
            );
            Some(StepAction::Continue)
        }
        0xf89 => {
            // SHFL with immediate lane and segmentation/clamp mask, any mode.
            let lane = ctx.insn.bits(53, 5);
            let mask = ctx.insn.bits(40, 13);
            let mode = ctx.insn.bits(58, 2);
            if ctx.insn.words[2] != 0x000e_0000
                || ctx.insn.bits(32, 8) != 0
                || ctx.insn.bits(60, 4) != 0
            {
                return fail(ctx, "native immediate shuffle mode");
            }
            ctx.emit(
                ctx.base(0xef10_0000_0000_0000u64)
                    | ((ctx.insn.bits(24, 8) as u64) << 8)
                    | ((lane as u64) << 20)
                    | (1u64 << 28)
                    | (1u64 << 29)
                    | ((mode as u64) << 30)
                    | ((mask as u64) << 34)
                    | (7u64 << 48),
            );
            Some(StepAction::Continue)
        }
        0x989 => {
            // SHFL, immediate lane and register clamp/segmentation mask, any mode.
            if (ctx.insn.words[1] & 0xf01f_ffffu32) != 0
                || (ctx.insn.words[2] & !0x000e_00ffu32) != 0
            {
                return fail(ctx, "native register-mask shuffle mode");
            }
            ctx.emit(
                ctx.base(0xef10_0000_0000_0000u64)
                    | ((ctx.insn.bits(24, 8) as u64) << 8)
                    | ((ctx.insn.bits(53, 5) as u64) << 20)
                    | (1u64 << 28)
                    | ((ctx.insn.bits(58, 2) as u64) << 30)
                    | ((ctx.insn.bits(64, 8) as u64) << 39)
                    | ((ctx.insn.bits(81, 3) as u64) << 48),
            );
            Some(StepAction::Continue)
        }
        0x589 => {
            // SHFL, register lane and immediate segmentation mask, any mode (58-59).
            if (ctx.insn.words[2] & !0x000e_0000u32) != 0
                || ctx.insn.bits(53, 5) != 0
                || ctx.insn.bits(60, 4) != 0
            {
                return fail(ctx, "native shuffle mode");
            }
            ctx.emit(
                0xef10_0000_0000_0000u64
                    | ctx.predicate
                    | ctx.dest
                    | ((ctx.insn.bits(24, 8) as u64) << 8)
                    | ((ctx.insn.bits(32, 8) as u64) << 20)
                    | (1u64 << 29)
                    | ((ctx.insn.bits(58, 2) as u64) << 30)
                    | ((ctx.insn.bits(40, 13) as u64) << 34)
                    | ((ctx.insn.bits(81, 3) as u64) << 48),
            );
            Some(StepAction::Continue)
        }
        0x2bd => {
            // UFLO.U32, the highest set-bit index (.SH, bit 74: its shift amount).
            let target = ctx.uniform_register(ctx.dest as u32);
            let source = ctx.uniform_register(ctx.insn.bits(32, 8));
            if ctx.dest == 63
                || target == u32::MAX
                || source == u32::MAX
                || (ctx.insn.words[1] & !0xffu32) != 0
                || (ctx.insn.words[2] & !0x400u32) != 0x080e_0000
                || ctx.native_predicate != 7
            {
                return fail(ctx, "native uniform leading bit mode");
            }
            ctx.emit(
                0x5c30_0000_0000_0000u64
                    | ctx.predicate
                    | (target as u64)
                    | ((source as u64) << 20)
                    | ((ctx.insn.bits(74, 1) as u64) << 41),
            );
            Some(StepAction::Continue)
        }
        0x2be => {
            // UBREV, as a bit-reversing zero shift like BREV.
            let target = ctx.uniform_register(ctx.dest as u32);
            let source = ctx.uniform_register(ctx.insn.bits(32, 8));
            if ctx.dest == 63
                || target == u32::MAX
                || source == u32::MAX
                || (ctx.insn.words[1] & !0xffu32) != 0
                || ctx.insn.words[2] != 0x0800_0000
                || ctx.native_predicate != 7
            {
                return fail(ctx, "native uniform bit reverse mode");
            }
            ctx.emit(
                0x3828_0000_0000_0000u64
                    | ctx.predicate
                    | (target as u64)
                    | ((source as u64) << 8)
                    | (1u64 << 39)
                    | (1u64 << 40),
            );
            Some(StepAction::Continue)
        }
        0x2bf => {
            // UPOPC, as POPC of the uniform's register (bit 63 inverts it).
            let target = ctx.uniform_register(ctx.dest as u32);
            let source = ctx.uniform_register(ctx.insn.bits(32, 8));
            if ctx.dest == 63
                || target == u32::MAX
                || source == u32::MAX
                || (ctx.insn.words[1] & !0x8000_00ffu32) != 0
                || ctx.insn.words[2] != 0x0800_0000
                || ctx.native_predicate != 7
            {
                return fail(ctx, "native uniform population count mode");
            }
            ctx.emit(
                0x5c08_0000_0000_0000u64
                    | ctx.predicate
                    | (target as u64)
                    | ((source as u64) << 20)
                    | ((ctx.insn.bits(63, 1) as u64) << 40),
            );
            Some(StepAction::Continue)
        }
        0x3a1 => {
            // MATCH.ALL (.U32, or .U64 on bit 73) with only the all-equal predicate retained.
            let source = ctx.insn.bits(24, 8);
            let target = ctx.insn.bits(81, 3);
            let words = if ctx.insn.bits(73, 1) != 0 { 2 } else { 1 };
            let scratch = if target == 6 { 5 } else { 6 }; // the comparison, saved around the match
            if ctx.dest != 255
                || source >= 254
                || (words == 2 && (source & 1) != 0)
                || target == 7
                || ctx.native_predicate != 7
                || ctx.insn.words[1] != 0
                || (ctx.insn.words[2] & !0xe_0200u32) != 0
            {
                return fail(ctx, "native warp match mode");
            }
            ctx.save_predicate(1u32 << scratch);
            // Compare every lane with the lowest active one, then require all to agree.
            ctx.emit(0x50d8_0000_0007_0000u64 | 250 | (7u64 << 39) | (7u64 << 45) | (1u64 << 48));
            ctx.emit(
                0x5c10_0000_0007_0000u64 | 251 | (250u64 << 8) | (255u64 << 20) | (1u64 << 49),
            );
            ctx.emit(
                0x5c40_0000_0007_0000u64 | 250 | (250u64 << 8) | (251u64 << 20) | (7u64 << 48),
            );
            ctx.emit(0x5c30_0000_0007_0000u64 | 250 | (250u64 << 20));
            for word in 0..words {
                ctx.emit(
                    0xef10_0000_0007_0000u64
                        | ((251 + word) as u64)
                        | (((source + word) as u64) << 8)
                        | (250u64 << 20)
                        | (1u64 << 29)
                        | (31u64 << 34)
                        | (7u64 << 48),
                );
            }
            ctx.emit(
                0x5b60_0000_0007_0000u64
                    | 7
                    | ((scratch as u64) << 3)
                    | ((source as u64) << 8)
                    | (251u64 << 20)
                    | (7u64 << 39)
                    | (2u64 << 49),
            );
            if words == 2 {
                ctx.emit(
                    0x5b60_0000_0007_0000u64
                        | 7
                        | ((scratch as u64) << 3)
                        | (((source + 1) as u64) << 8)
                        | (252u64 << 20)
                        | ((scratch as u64) << 39)
                        | (2u64 << 49),
                );
            }
            ctx.emit(
                0x50d8_0000_0007_0000u64 | 255 | ((scratch as u64) << 39) | ((target as u64) << 45),
            );
            ctx.restore_predicate(1u32 << scratch);
            Some(StepAction::Continue)
        }
        0x80d => {
            // CSMTEST: a test of shading-mode state Eden does not model, taken as false.
            // Kirby Air Riders' vertex-side shaders run "CSMTEST.A.FCSM_TR P0" then "@P0 EXIT":
            // never leave early. Outputs of PT discard the result.
            let first = ctx.insn.bits(81, 3);
            let second = ctx.insn.bits(84, 3);
            if ctx.insn.bits(87, 4) != 7 || ctx.insn.bits(16, 16) != 0 {
                return fail(ctx, "native CSM test mode");
            }
            for target in [first, second] {
                if target != 7 {
                    ctx.emit(
                        0x5090_0000_0000_0000u64
                            | ctx.predicate
                            | ((target as u64) << 3)
                            | 7
                            | (7u64 << 12)
                            | (1u64 << 15)
                            | (7u64 << 29)
                            | (7u64 << 39),
                    );
                }
            }
            Some(StepAction::Continue)
        }
        0x3c4 => {
            // REDUX.SUM (modulo 2^32), .MIN/.MAX (unsigned), .OR or .XOR over the actual active lanes.
            let source = ctx.insn.bits(24, 8);
            let target = ctx.uniform_register(ctx.dest as u32);
            let mode = ctx.insn.words[2] & !0x200u32;
            let maximum = mode == 0x1_4000;
            let minimum = mode == 0x1_0000;
            let signed_order = ctx.insn.bits(73, 1) != 0;
            // OR and XOR: inactive lanes contribute zero, as for the sum. Maxwell LOP: 1 = OR, 2 = XOR.
            let logical = if ctx.insn.words[2] == 0x4000 {
                1u32
            } else if ctx.insn.words[2] == 0x8000 {
                2u32
            } else {
                0u32
            };
            if target == u32::MAX
                || target == 255
                || ctx.insn.words[1] != 0
                || (ctx.insn.words[2] != 0xc000 && !maximum && !minimum && logical == 0)
                || (signed_order && !maximum && !minimum)
                || ctx.native_predicate != 7
            {
                return fail(ctx, "native warp sum mode");
            }
            ctx.emit(0x50d8_0000_0007_0000u64 | 250 | (7u64 << 39) | (7u64 << 45) | (1u64 << 48));
            ctx.mov_imm(target, if minimum { u32::MAX } else { 0 });
            for lane in 0..32u32 {
                ctx.emit(
                    0xef10_0000_0007_0000u64
                        | 251
                        | ((source as u64) << 8)
                        | ((lane as u64) << 20)
                        | (1u64 << 28)
                        | (1u64 << 29)
                        | (31u64 << 34)
                        | (7u64 << 48),
                );
                if signed_order {
                    ctx.emit(
                        0x0400_0000_0007_0000u64
                            | 251
                            | (251u64 << 8)
                            | (0x8000_0000u64 << 20)
                            | (2u64 << 53),
                    );
                }
                ctx.emit(0x3828_0000_0007_0000u64 | 252 | (250u64 << 8) | ((lane as u64) << 20));
                ctx.emit(0x0400_0000_0007_0000u64 | 252 | (252u64 << 8) | (1u64 << 20));
                ctx.emit(
                    0x5c10_0000_0007_0000u64 | 252 | (255u64 << 8) | (252u64 << 20) | (1u64 << 48),
                );
                // Inactive lanes: 0, or ~0 for the minimum (OR with the inverted lane mask).
                ctx.emit(
                    0x5c40_0000_0007_0000u64
                        | 251
                        | (251u64 << 8)
                        | (252u64 << 20)
                        | (7u64 << 48)
                        | ((minimum as u64) << 40)
                        | ((minimum as u64) << 41),
                );
                if minimum {
                    ctx.emit(
                        0x5c20_0000_0007_0000u64
                            | (target as u64)
                            | ((target as u64) << 8)
                            | (251u64 << 20)
                            | (7u64 << 39),
                    );
                } else if maximum {
                    ctx.emit(
                        0x5c20_0000_0007_0000u64
                            | (target as u64)
                            | ((target as u64) << 8)
                            | (251u64 << 20)
                            | (15u64 << 39),
                    );
                } else if logical != 0 {
                    ctx.emit(
                        0x5c40_0000_0007_0000u64
                            | (target as u64)
                            | ((target as u64) << 8)
                            | (251u64 << 20)
                            | ((logical as u64) << 41)
                            | (7u64 << 48),
                    );
                } else {
                    ctx.emit(
                        0x5c10_0000_0007_0000u64
                            | (target as u64)
                            | ((target as u64) << 8)
                            | (251u64 << 20),
                    );
                }
            }
            if signed_order {
                ctx.emit(
                    0x0400_0000_0007_0000u64
                        | (target as u64)
                        | ((target as u64) << 8)
                        | (0x8000_0000u64 << 20)
                        | (2u64 << 53),
                );
            }
            Some(StepAction::Continue)
        }
        0x301 => {
            // BREV register.
            if ctx.insn.words[2] != 0 || ctx.insn.words[1] > 255 {
                return fail(ctx, "native bit reverse mode");
            }
            ctx.emit(
                ctx.base(0x3828_0000_0000_0000u64)
                    | ((ctx.insn.words[1] as u64) << 8)
                    | (1u64 << 39)
                    | (1u64 << 40),
            );
            Some(StepAction::Continue)
        }
        0x219 | 0xa19 | 0xc19 => {
            // U32 register (or cbuf, 0xa19, or uniform, 0xc19) shifts with a zero word outside the selected result.
            let low = ctx.insn.bits(24, 8);
            let high = ctx.insn.bits(64, 8);
            let constant = ctx.opcode == 0xa19;
            // A constant shift amount is loaded into R251 once the form is known.
            let shift = if ctx.opcode == 0xc19 {
                ctx.uniform_register(ctx.insn.bits(32, 8))
            } else if constant {
                251
            } else {
                ctx.insn.bits(32, 8)
            };
            let shift_operand = if constant {
                ctx.valid_cbuf() && ctx.insn.bits(62, 2) == 0
            } else {
                ctx.insn.words[1] <= 255
            };
            let load_shift = |ctx: &mut LowerCtx<'_>| {
                if constant {
                    ctx.materialize(2, 0, 251, false, false);
                }
            };
            let right = ctx.insn.bits(76, 1) != 0;
            let high_result = ctx.insn.bits(80, 1) != 0;
            if ctx.opcode == 0x219
                && shift_operand
                && (ctx.insn.words[2] & !0xffu32) == 0x0000_1200
            {
                ctx.right_shift_u64_low(ctx.dest as u32, low, high, shift);
                return Some(StepAction::Continue);
            }
            if right
                && high_result
                && low == 255
                && ctx.insn.bits(73, 2) == 2
                && shift_operand
                && shift != u32::MAX
                && (ctx.insn.words[2]
                    & !(0x0001_18ffu32
                        | if ctx.opcode == 0xc19 {
                            0x0800_0000u32
                        } else {
                            0u32
                        }))
                    == 0x0000_0400
            {
                load_shift(ctx);
                ctx.emit(
                    ctx.base(0x5c28_0000_0000_0000u64)
                        | ((high as u64) << 8)
                        | ((shift as u64) << 20)
                        | (1u64 << 48)
                        | ((ctx.insn.bits(75, 1) as u64) << 39),
                );
                return Some(StepAction::Continue);
            }
            // SHF.R.U32 of the low word with a zero high word: a plain logical right shift.
            if right
                && !high_result
                && high == 255
                && ctx.insn.bits(73, 2) == 3
                && shift != u32::MAX
                && shift_operand
                && (ctx.insn.words[2]
                    & !(0x0001_18ffu32
                        | if ctx.opcode == 0xc19 {
                            0x0800_0000u32
                        } else {
                            0u32
                        }))
                    == 0x0000_0600
            {
                load_shift(ctx);
                ctx.emit(
                    ctx.base(0x5c28_0000_0000_0000u64)
                        | ((low as u64) << 8)
                        | ((shift as u64) << 20)
                        | ((ctx.insn.bits(75, 1) as u64) << 39),
                );
                return Some(StepAction::Continue);
            }
            if !shift_operand
                || ctx.insn.bits(73, 2) != 3
                || shift == u32::MAX
                || (ctx.insn.words[2]
                    & !(0x0001_18ffu32
                        | if ctx.opcode == 0xc19 {
                            0x0800_0000u32
                        } else {
                            0u32
                        }))
                    != 0x0000_0600
                || (if right {
                    !high_result || low != 255
                } else {
                    high_result || high != 255
                })
            {
                return fail(ctx, "native register shift mode");
            }
            load_shift(ctx);
            ctx.emit(
                ctx.base(if right {
                    0x5cf8_0000_0000_0000u64
                } else {
                    0x5bf8_0000_0000_0000u64
                }) | (((if right { high } else { 255 }) as u64) << 8)
                    | ((shift as u64) << 20)
                    | (((if right { 255 } else { low }) as u64) << 39)
                    | ((ctx.insn.bits(75, 1) as u64) << 50),
            );
            Some(StepAction::Continue)
        }
        0x300 | 0xd00 => {
            // FLO, including a uniform source.
            if (ctx.insn.words[2]
                & !(0x600u32
                    | if ctx.opcode == 0xd00 {
                        0x0800_0000u32
                    } else {
                        0u32
                    }))
                != 0x000e_0000
            {
                return fail(ctx, "native leading bit mode");
            }
            let source = ctx.operand(1, false);
            ctx.emit(
                ctx.base(0x5c30_0000_0000_0000u64)
                    | ((source as u64) << 20)
                    | ((ctx.insn.bits(63, 1) as u64) << 40)
                    | ((ctx.insn.bits(74, 1) as u64) << 41)
                    | ((ctx.insn.bits(73, 1) as u64) << 48),
            );
            Some(StepAction::Continue)
        }
        0x309 | 0xd09 => {
            // POPC, including a uniform source.
            if ctx.insn.words[2] != (if ctx.opcode == 0xd09 { 0x0800_0000 } else { 0 }) {
                return fail(ctx, "native population count mode");
            }
            let source = ctx.operand(1, false);
            ctx.emit(
                ctx.base(0x5c08_0000_0000_0000u64)
                    | ((source as u64) << 20)
                    | ((ctx.insn.bits(63, 1) as u64) << 40),
            );
            Some(StepAction::Continue)
        }
        0x918 => {
            if ctx.insn.words[1] != 0 || ctx.insn.words[2] != 0 {
                return fail(ctx, "native NOP mode");
            }
            ctx.emit(0x50b0_0000_0000_0f00u64 | ctx.predicate);
            Some(StepAction::Continue)
        }
        0x95c => {
            // TRAP: a parameter guard that stops the console GPU; end the thread instead.
            if (ctx.insn.words[0] & !0xf000u32) != 0x095c
                || ctx.insn.words[1] != 4
                || ctx.insn.words[2] != 0x0030_0000
            {
                return fail(ctx, "native trap mode");
            }
            ctx.emit(0xe300_0000_0000_000fu64 | ctx.predicate);
            Some(StepAction::Continue)
        }
        0x992 => {
            // MEMBAR.SC: preserve workgroup/device visibility.
            let scope = ctx.insn.bits(76, 3);
            if ctx.insn.words[1] != 0
                || ctx.insn.words[2] != (scope << 12)
                || !matches!(scope, 0 | 2 | 3 | 5)
            {
                return fail(ctx, "native memory barrier scope");
            }
            let legacy_scope = match scope {
                0 => 0u64,
                2 => 1u64,
                3 => 2u64,
                _ => 3u64,
            };
            ctx.emit(0xef98_0000_0000_0000u64 | ctx.predicate | (legacy_scope << 8));
            Some(StepAction::Continue)
        }
        0x9ab => {
            // ERRBAR: wait for prior memory operations on the host backend.
            if ctx.insn.words[1] != 0 || ctx.insn.words[2] != 0 {
                return fail(ctx, "native error barrier mode");
            }
            ctx.emit(0xef98_0000_0000_0000u64 | ctx.predicate | (1u64 << 8));
            Some(StepAction::Continue)
        }
        0xb1d => {
            // BAR.SYNC 0 (bit 80: .DEFER_BLOCKING), full workgroup.
            if ctx.insn.words[1] != 0
                || (ctx.insn.words[2] & !0x1_0000u32) != 0
                || ctx.native_predicate != 7
            {
                return fail(ctx, "native workgroup barrier mode");
            }
            ctx.emit(
                0xf0a8_0000_0000_0000u64
                    | ctx.predicate
                    | (1u64 << 43)
                    | (1u64 << 44)
                    | (1u64 << 39)
                    | (7u64 << 39),
            );
            Some(StepAction::Continue)
        }
        0x919 | 0x9c3 => {
            // S2R / S2UR: special registers the Maxwell S2R handles.
            // Same numbering as Maxwell: lane id (0), invocation id/info, Y direction, thread kill
            // (17-19, 29), W scale factors (30-31, Kirby Air Riders), thread and workgroup ids,
            // lane masks (56-60).
            let special = ctx.insn.bits(72, 8);
            if special == 132 && ctx.insn.words[1] == 0 && ctx.insn.words[2] == (special << 8) {
                // SR_VARIABLE_RATE: without variable-rate shading every fragment is 1x1, its width
                // and height in bytes 1 and 2 (Nintendo Switch Sports Resort multiplies them).
                let target = if ctx.opcode == 0x9c3 {
                    ctx.uniform_register(ctx.dest as u32)
                } else {
                    ctx.dest as u32
                };
                if target == u32::MAX {
                    return fail(ctx, "native uniform special destination");
                }
                ctx.mov_imm(target, 0x0001_0100);
                return Some(StepAction::Continue);
            }
            if ctx.insn.words[1] != 0
                || ctx.insn.words[2] != (special << 8)
                || (special != 0
                    && !(17..=19).contains(&special)
                    && !(29..=35).contains(&special)
                    && special != 37
                    && special != 38
                    && special != 39
                    && special != 56
                    && special != 57
                    && special != 58
                    && special != 59
                    && special != 60)
            {
                return fail(ctx, "native special register");
            }
            let target = if ctx.opcode == 0x9c3 {
                ctx.uniform_register(ctx.dest as u32)
            } else {
                ctx.dest as u32
            };
            if target == u32::MAX {
                return fail(ctx, "native uniform special destination");
            }
            ctx.emit(
                0xf0c8_0000_0000_0000u64
                    | ctx.predicate
                    | (target as u64)
                    | ((special as u64) << 20),
            );
            Some(StepAction::Continue)
        }
        0x28c | 0x88c => {
            // UISETP, signed (bit 73) or unsigned, with predicate accumulation.
            let output = ctx.insn.bits(81, 3);
            let accum = ctx.insn.bits(87, 3);
            let accum_neg = ctx.insn.bits(90, 1);
            let set_op = ctx.insn.bits(74, 2);
            let a = ctx.uniform_register(ctx.insn.bits(24, 8));
            let b = if ctx.opcode == 0x88c {
                252
            } else {
                ctx.uniform_register(ctx.insn.bits(32, 8))
            };
            if output > 6
                || ctx.insn.bits(84, 3) != 7
                || ctx.insn.bits(72, 1) != 0
                || set_op == 3
                || a == u32::MAX
                || b == u32::MAX
            {
                return fail(ctx, "native uniform comparison mode");
            }
            let temporary = 6 - output;
            let accum_pred = if accum == 7 { 7 } else { 6 - accum };
            // @UPn: the guard and the old output load too, so a false guard merges it back unchanged.
            let guarded = ctx.native_predicate != 7;
            let load_mask = (if accum == 7 { 0 } else { 1u32 << accum_pred })
                | if guarded {
                    (1u32 << temporary)
                        | if ctx.guard_slot < 7 {
                            1u32 << ctx.guard_slot
                        } else {
                            0
                        }
                } else {
                    0
                };
            let mask = (1u32 << temporary) | load_mask;
            if ctx.opcode == 0x88c {
                ctx.emit(
                    0x0100_0000_0000_0000u64
                        | (7u64 << 16)
                        | 252
                        | (15u64 << 12)
                        | ((ctx.insn.words[1] as u64) << 20),
                );
            }
            ctx.emit(0x38e8_0000_0007_0000u64 | 253 | (255u64 << 8) | ((mask as u64) << 20));
            if load_mask != 0 {
                ctx.emit(0x38f0_0000_0007_0000u64 | (239u64 << 8) | ((load_mask as u64) << 20));
            }
            let guard_pred = if guarded {
                (ctx.native_predicate & 8) | ctx.guard_slot
            } else {
                7
            };
            ctx.emit(
                0x5b60_0000_0000_0000u64
                    | ((guard_pred as u64) << 16)
                    | 7
                    | ((temporary as u64) << 3)
                    | ((a as u64) << 8)
                    | ((b as u64) << 20)
                    | ((accum_pred as u64) << 39)
                    | ((accum_neg as u64) << 42)
                    | ((set_op as u64) << 45)
                    | ((ctx.insn.bits(73, 1) as u64) << 48)
                    | ((ctx.insn.bits(76, 3) as u64) << 49),
            );
            ctx.emit(
                0x38e8_0000_0007_0000u64 | 239 | (239u64 << 8) | ((1u64 << temporary) << 20),
            );
            ctx.emit(0x38f0_0000_0007_0000u64 | (253u64 << 8) | ((mask as u64) << 20));
            Some(StepAction::Continue)
        }
        0x287 | 0x887 => {
            // USEL, UP0/UP1; preserve ordinary predicates.
            let condition = ctx.insn.bits(87, 3);
            let target = ctx.uniform_register(ctx.dest as u32);
            let a = ctx.uniform_register(ctx.insn.bits(24, 8));
            let b = if ctx.opcode == 0x887 {
                252
            } else {
                ctx.uniform_register(ctx.insn.bits(32, 8))
            };
            if condition > 6
                || target == u32::MAX
                || a == u32::MAX
                || b == u32::MAX
                || (ctx.insn.words[2] & !0x0780_0000u32) != 0x0800_0000
            {
                return fail(ctx, "native uniform select mode");
            }
            // The clamp's UISETP may sit a few instructions back, with only instructions between
            // that write no uniform state and do not branch (Nintendo Switch Sports' jump tables),
            // and no branch seen so far landing between them.
            let mut compare = ctx.index;
            let max_back = 8u32.min(ctx.index);
            for back in 1..=max_back {
                let earlier = ctx.code[(ctx.index - back) as usize].bits(0, 12);
                let datapath = earlier & 0x1ff;
                if earlier == 0x88c {
                    compare = ctx.index - back;
                    break;
                }
                if (0x080..=0x0bf).contains(&datapath)
                    || (0x1c2..=0x1c4).contains(&datapath)
                    || (0x940..=0x95f).contains(&earlier)
                {
                    break;
                }
            }
            let target_between = ctx
                .relocations
                .iter()
                .any(|r| r.target > (compare as i64) && r.target <= (ctx.index as i64));
            if ctx.opcode == 0x887
                && ctx.native_predicate == 7
                && compare < ctx.index
                && !target_between
                && ctx.insn.words[1] <= 0x7_ffff
                && ctx.code[compare as usize].bits(12, 4) == 7
                && ctx.code[compare as usize].bits(24, 8) == ctx.insn.bits(24, 8)
                && ctx.code[compare as usize].words[1] == ctx.insn.words[1]
                && ctx.code[compare as usize].words[2] == (0x0bf0_1070u32 | (condition << 17))
                && ctx.insn.words[2] == (0x0800_0000u32 | (condition << 23))
            {
                // Preserve the unsigned clamp idiom for indirect-table analysis.
                ctx.emit(
                    0x3820_0000_0007_0000u64
                        | (target as u64)
                        | ((a as u64) << 8)
                        | ((ctx.insn.words[1] as u64) << 20)
                        | (7u64 << 39),
                );
                return Some(StepAction::Continue);
            }
            if ctx.opcode == 0x887 {
                ctx.emit(
                    0x0100_0000_0000_0000u64
                        | (7u64 << 16)
                        | 252
                        | (15u64 << 12)
                        | ((ctx.insn.words[1] as u64) << 20),
                );
            }
            let temporary = 6 - condition;
            let mask = (1u32 << temporary)
                | if ctx.native_predicate != 7 && ctx.guard_slot < 7 {
                    1u32 << ctx.guard_slot
                } else {
                    0
                };
            ctx.emit(0x38e8_0000_0007_0000u64 | 253 | (255u64 << 8) | ((mask as u64) << 20));
            ctx.emit(0x38f0_0000_0007_0000u64 | (239u64 << 8) | ((mask as u64) << 20));
            let pred_bits = (ctx.native_predicate & 8)
                | if ctx.native_predicate != 7 {
                    ctx.guard_slot
                } else {
                    7
                };
            ctx.emit(
                0x5ca0_0000_0000_0000u64
                    | ((pred_bits as u64) << 16)
                    | (target as u64)
                    | ((a as u64) << 8)
                    | ((b as u64) << 20)
                    | ((temporary as u64) << 39)
                    | ((ctx.insn.bits(90, 1) as u64) << 42),
            );
            ctx.emit(0x38f0_0000_0007_0000u64 | (253u64 << 8) | ((mask as u64) << 20));
            Some(StepAction::Continue)
        }
        0x91a => {
            // DEPBAR.LE SBn, count, {list}: scoreboard wait, represented by IR data dependencies.
            let scoreboard = ctx.insn.words[1] & !0xfffu32;
            if ctx.insn.words[0] != 0x791a
                || (ctx.insn.words[1] & !0x7fffu32) != 0x8000
                || ctx.insn.words[2] != 0
            {
                return fail(ctx, "native dependency barrier mode");
            }
            if ctx.uses_async_copy && scoreboard == 0x8000 {
                ctx.emit(0xef98_0000_0000_0000u64 | ctx.predicate); // MEMBAR.CTA
            }
            Some(StepAction::Continue)
        }
        0x816 => {
            // Captured PRMT half-word concatenation
            if ctx.insn.words[1] > 0xffff || (ctx.insn.words[2] & !0xffu32) != 0 {
                return fail(ctx, "native permutation mode");
            }
            if !matches!(ctx.insn.words[1], 0x5410 | 0x5432 | 0x7610 | 0x7632) {
                // Byte i of the result is byte (s & 7) of c:a, or its sign bit repeated when s & 8.
                let a = ctx.insn.bits(24, 8);
                let c = ctx.insn.bits(64, 8);
                if a >= 250 && a != 255 {
                    return fail(ctx, "native permutation source");
                }
                ctx.mov_imm(250, 0);
                for i in 0..4u32 {
                    let select = (ctx.insn.words[1] >> (i * 4)) & 0xf;
                    let source = if (select & 4) != 0 { c } else { a };
                    let position = (select & 3) * 8;
                    let field = if (select & 8) != 0 {
                        (position + 7) | (1u32 << 8)
                    } else {
                        position | (8u32 << 8)
                    };
                    ctx.emit(
                        0x3800_0000_0000_0000u64
                            | ctx.predicate
                            | 251
                            | ((source as u64) << 8)
                            | ((field as u64) << 20)
                            | ((((select >> 3) & 1) as u64) << 48),
                    );
                    ctx.emit(
                        0x36f0_0000_0000_0000u64
                            | ctx.predicate
                            | 250
                            | (251u64 << 8)
                            | (((i * 8) | (8u32 << 8)) as u64) << 20
                            | (250u64 << 39),
                    );
                }
                ctx.emit(ctx.base(0x5c10_0000_0000_0000u64) | (250u64 << 8) | (255u64 << 20));
                return Some(StepAction::Continue);
            }
            let a = ctx.insn.bits(24, 8);
            let b = ctx.insn.bits(64, 8);
            if ctx.insn.words[1] != 0x5432 && ctx.insn.words[1] != 0x7632 {
                ctx.emit(
                    0x0400_0000_0000_0000u64
                        | ctx.predicate
                        | 250
                        | ((a as u64) << 8)
                        | (0xffffu64 << 20),
                );
            } else {
                ctx.emit(
                    0x3828_0000_0000_0000u64
                        | ctx.predicate
                        | 250
                        | ((a as u64) << 8)
                        | (16u64 << 20),
                );
            }
            if ctx.insn.words[1] == 0x7610 || ctx.insn.words[1] == 0x7632 {
                ctx.emit(
                    0x0400_0000_0000_0000u64
                        | ctx.predicate
                        | 251
                        | ((b as u64) << 8)
                        | (0xffff_0000u64 << 20),
                );
                ctx.emit(ctx.base(0x5c10_0000_0000_0000u64) | (250u64 << 8) | (251u64 << 20));
            } else {
                ctx.emit(
                    ctx.base(0x5c18_0000_0000_0000u64)
                        | ((b as u64) << 8)
                        | (250u64 << 20)
                        | (16u64 << 39),
                );
            }
            Some(StepAction::Continue)
        }
        0x803 => {
            // P2R PR, register merge source, immediate mask, byte select (76-77)
            if ctx.insn.words[1] > 0x7f || (ctx.insn.words[2] & !0x3000u32) != 0 {
                return fail(ctx, "native predicate-to-register mode");
            }
            ctx.emit(
                ctx.base(0x38e8_0000_0000_0000u64)
                    | ((ctx.insn.bits(24, 8) as u64) << 8)
                    | ((ctx.insn.words[1] as u64) << 20)
                    | ((ctx.insn.bits(76, 2) as u64) << 41),
            );
            Some(StepAction::Continue)
        }
        0x804 => {
            // R2P PR, register, immediate mask, byte select (76-77)
            if ctx.insn.bits(16, 8) != 0
                || ctx.insn.words[1] > 0x7f
                || (ctx.insn.words[2] & !0x3000u32) != 0
            {
                return fail(ctx, "native register-to-predicate mode");
            }
            ctx.emit(
                0x38f0_0000_0000_0000u64
                    | ctx.predicate
                    | ((ctx.insn.bits(24, 8) as u64) << 8)
                    | ((ctx.insn.words[1] as u64) << 20)
                    | ((ctx.insn.bits(76, 2) as u64) << 41),
            );
            Some(StepAction::Continue)
        }
        0xc02 | 0x202 | 0x802 | 0xa02 => {
            // MOV uniform register / register / immediate / constant buffer
            if ctx.insn.bits(72, 4) != 15
                || (ctx.insn.bits(76, 16)
                    & !(if ctx.opcode == 0xc02 {
                        0x8000u32
                    } else {
                        0u32
                    }))
                    != 0
            {
                return fail(ctx, "native MOV modifiers");
            }
            if ctx.opcode == 0x802 {
                ctx.emit(
                    ctx.base(0x0100_0000_0000_0000u64)
                        | (15u64 << 12)
                        | ((ctx.insn.words[1] as u64) << 20),
                );
            } else if ctx.opcode == 0xc02 {
                // Any uniform register (Hyrule Warriors: MOV R18, UR17; MOV R105, UR42).
                let source = if ctx.insn.words[1] < 64 {
                    ctx.uniform_register(ctx.insn.words[1])
                } else {
                    u32::MAX
                };
                if source == u32::MAX || ctx.insn.words[2] != 0x0800_0f00 {
                    return fail(ctx, "native uniform move source");
                }
                ctx.emit(
                    ctx.base(0x5c98_0000_0000_0000u64) | (15u64 << 39) | ((source as u64) << 20),
                );
            } else if ctx.opcode == 0x202 {
                if ctx.insn.bits(40, 24) != 0 {
                    return fail(ctx, "native MOV register modifiers");
                }
                ctx.emit(
                    ctx.base(0x5c98_0000_0000_0000u64)
                        | (15u64 << 39)
                        | ((ctx.insn.bits(32, 8) as u64) << 20),
                );
            } else {
                if !ctx.valid_cbuf() || ctx.insn.bits(62, 2) != 0 {
                    return fail(ctx, "native MOV constant buffer");
                }
                ctx.emit(ctx.base(0x4c98_0000_0000_0000u64) | (15u64 << 39) | ctx.cbuf());
            }
            Some(StepAction::Continue)
        }
        0x81a => {
            // SGXT[.U32] Rd, Ra, n: the low n bits, sign-extended unless .U32 (BFE at position 0)
            if (ctx.insn.words[2] & !0x200u32) != 0 || ctx.insn.words[1] > 32 || ctx.dest >= 255 {
                return fail(ctx, "native sign extend mode");
            }
            ctx.emit(
                ctx.base(0x3800_0000_0000_0000u64)
                    | ((ctx.insn.bits(24, 8) as u64) << 8)
                    | (((ctx.insn.words[1] << 8) as u64) << 20)
                    | ((ctx.insn.bits(73, 1) as u64) << 48),
            );
            Some(StepAction::Continue)
        }
        0x299 => {
            // USHF by a uniform amount, one register shifted (SHL/SHR, bit 75 wraps).
            let dst = ctx.uniform_register(ctx.insn.bits(16, 8));
            let low = ctx.uniform_register(ctx.insn.bits(24, 8));
            let shift = ctx.uniform_register(ctx.insn.bits(32, 8));
            let high = ctx.uniform_register(ctx.insn.bits(64, 8));
            let right = ctx.insn.bits(76, 1) != 0;
            let high_result = ctx.insn.bits(80, 1) != 0;
            if dst == u32::MAX
                || dst == 255
                || low == u32::MAX
                || shift == u32::MAX
                || high == u32::MAX
                || ctx.native_predicate != 7
                || ctx.insn.bits(73, 2) != 3
                || (ctx.insn.words[2] & !0x0001_18ffu32) != 0x0800_0600
            {
                return fail(ctx, "native uniform register shift mode");
            }
            let source = if high_result && low == 255 {
                high // (high:0) >> or << s, high word
            } else if !high_result && high == 255 {
                low // (0:low) >> or << s, low word
            } else {
                return fail(ctx, "native uniform funnel shift");
            };
            ctx.emit(
                (if right {
                    0x5c28_0000_0000_0000u64
                } else {
                    0x5c48_0000_0000_0000u64
                }) | ctx.predicate
                    | (dst as u64)
                    | ((source as u64) << 8)
                    | ((shift as u64) << 20)
                    | ((ctx.insn.bits(75, 1) as u64) << 39),
            );
            Some(StepAction::Continue)
        }
        0x499 => {
            // USHF.L[.W].U32.HI URd, URZ, URb, imm: imm << URb, as 0x419 below
            let target = ctx.uniform_register(ctx.dest as u32);
            let shift = ctx.uniform_register(ctx.insn.bits(64, 8));
            if ctx.insn.bits(24, 8) != 63
                || (ctx.insn.words[2] & !0x8ffu32) != 0x0801_0600
                || (ctx.insn.words[3] & 0xc000_01ffu32) != 0
                || target == u32::MAX
                || target == 255
                || shift == u32::MAX
            {
                return fail(ctx, "native uniform immediate high-word shift mode");
            }
            ctx.mov_imm(250, ctx.insn.words[1]);
            ctx.emit(
                0x5c48_0000_0000_0000u64
                    | ctx.predicate
                    | (target as u64)
                    | (250u64 << 8)
                    | ((shift as u64) << 20)
                    | ((ctx.insn.bits(75, 1) as u64) << 39),
            );
            Some(StepAction::Continue)
        }
        0x419 => {
            // SHF.L[.W].U32.HI Rd, RZ, Rb, imm (high word of imm:0 << Rb) = imm << Rb
            // .W (bit 75) wraps the shift to Rb & 31; without it a shift of 32 or more gives zero,
            // like Maxwell SHL without .W (bit 39).
            if ctx.insn.bits(24, 8) != 255 || (ctx.insn.words[2] & !0x8ffu32) != 0x0001_0600 {
                return fail(ctx, "native funnel shift immediate mode");
            }
            ctx.mov_imm(250, ctx.insn.words[1]);
            ctx.emit(
                ctx.base(0x5c48_0000_0000_0000u64)
                    | (250u64 << 8)
                    | ((ctx.insn.bits(64, 8) as u64) << 20)
                    | ((ctx.insn.bits(75, 1) as u64) << 39),
            );
            Some(StepAction::Continue)
        }
        0x805 => {
            // CS2R SRZ
            if ctx.insn.bits(72, 8) != 255 || ctx.insn.bits(80, 1) == 0 || ctx.dest >= 249 {
                return fail(ctx, "native special-register read");
            }
            ctx.mov_imm(ctx.dest as u32, 0);
            ctx.mov_imm((ctx.dest as u32) + 1, 0);
            Some(StepAction::Continue)
        }
        0x207 | 0x807 | 0xa07 | 0xc07 | 0x208 | 0x808 | 0xa08 => {
            // Bitwise SEL / FSEL
            // FSEL applies the floating operand modifiers (negate/absolute) before selecting.
            let floating = (ctx.opcode & 0x1ff) == 0x008;
            let a = ctx.operand(0, floating);
            let b = ctx.operand(1, floating);
            ctx.emit(
                ctx.base(0x5ca0_0000_0000_0000u64)
                    | ((a as u64) << 8)
                    | ((b as u64) << 20)
                    | ((ctx.insn.bits(87, 3) as u64) << 39)
                    | ((ctx.insn.bits(90, 1) as u64) << 42),
            );
            Some(StepAction::Continue)
        }
        0x212 | 0x812 | 0xa12 | 0xc12 => {
            // LOP3.LUT, any table, optional P = (result != 0)
            let lut = ctx.insn.bits(72, 8);
            let pred_target = ctx.insn.bits(81, 3);
            if ctx.insn.bits(80, 1) != 0
                || ctx.insn.bits(87, 4) != 15
                || (ctx.opcode == 0x212 && ctx.insn.bits(40, 24) != 0)
                || (ctx.opcode == 0xa12 && ctx.insn.bits(62, 2) != 0)
            {
                return fail(ctx, "native LUT/predicate mode");
            }
            let b = if ctx.opcode == 0x212 {
                ctx.insn.bits(32, 8)
            } else if ctx.opcode == 0x812 {
                ctx.mov_imm(251, ctx.insn.words[1]);
                251
            } else if ctx.opcode == 0xc12 {
                let reg = ctx.uniform_register(ctx.insn.bits(32, 8));
                if reg == u32::MAX {
                    return fail(ctx, "native uniform logic source");
                }
                reg
            } else {
                ctx.operand(1, false)
            };
            let a = ctx.insn.bits(24, 8);
            let c = ctx.insn.bits(64, 8);
            ctx.emit(
                0x5be0_0000_0000_0000u64
                    | ctx.predicate
                    | ctx.dest
                    | ((a as u64) << 8)
                    | ((b as u64) << 20)
                    | ((lut as u64) << 28)
                    | ((if pred_target == 7 { 0u64 } else { 3u64 }) << 36)
                    | ((c as u64) << 39)
                    | ((pred_target as u64) << 48),
            );
            Some(StepAction::Continue)
        }
        0x213 | 0xa13 | 0xc13 => {
            // IABS: max(x, 0 - x), signed.
            if (ctx.insn.words[2]
                & !(if ctx.opcode == 0xc13 {
                    0x0800_0000u32
                } else {
                    0u32
                }))
                != 0
            {
                return fail(ctx, "native integer absolute mode");
            }
            let source = ctx.operand(1, false);
            ctx.emit(
                0x5c10_0000_0000_0000u64
                    | ctx.predicate
                    | 250
                    | (255u64 << 8)
                    | ((source as u64) << 20)
                    | (1u64 << 48),
            );
            ctx.emit(
                ctx.base(0x5c20_0000_0000_0000u64)
                    | ((source as u64) << 8)
                    | (250u64 << 20)
                    | (15u64 << 39)
                    | (1u64 << 48),
            );
            Some(StepAction::Continue)
        }
        0x217 | 0x817 | 0xa17 | 0xc17 => {
            // IMNMX register, immediate, cbuf or uniform operand.
            if (ctx.insn.words[2]
                & !(0x0400_0200u32
                    | if ctx.opcode == 0xc17 {
                        0x0800_0000u32
                    } else {
                        0u32
                    }))
                != 0x0380_0000
                || (ctx.opcode == 0x817
                    && ctx.insn.words[1] > 0x7_ffff
                    && ctx.insn.words[1] < 0xfff8_0000)
                || (ctx.opcode != 0xa17 && ctx.opcode != 0x817 && ctx.insn.words[1] > 255)
                || (ctx.opcode == 0xa17 && (!ctx.valid_cbuf() || ctx.insn.bits(62, 2) != 0))
            {
                return fail(ctx, "native integer min/max mode");
            }
            // An immediate is a signed 20-bit value: 19 bits at 20, the sign in bit 56.
            let source = if ctx.opcode == 0xc17 {
                ctx.uniform_register(ctx.insn.words[1])
            } else if ctx.opcode == 0xa17 {
                ctx.operand(1, false)
            } else if ctx.opcode == 0x817 {
                ctx.insn.words[1] & 0x7_ffff
            } else {
                ctx.insn.words[1]
            };
            if source == u32::MAX {
                return fail(ctx, "native uniform min/max operand");
            }
            let sign_bit = if ctx.opcode == 0x817 {
                ctx.insn.words[1] >> 31
            } else {
                0
            };
            ctx.emit(
                ctx.base(if ctx.opcode == 0x817 {
                    0x3820_0000_0000_0000u64
                } else {
                    0x5c20_0000_0000_0000u64
                }) | ((ctx.insn.bits(24, 8) as u64) << 8)
                    | ((source as u64) << 20)
                    | ((sign_bit as u64) << 56)
                    | ((ctx.insn.bits(87, 4) as u64) << 39)
                    | ((ctx.insn.bits(73, 1) as u64) << 48),
            );
            Some(StepAction::Continue)
        }
        0x819 => {
            // SHF.HI, immediate shift
            let is_signed = ctx.insn.bits(73, 2) == 2;
            if ctx.insn.bits(80, 1) == 0
                && ctx.insn.bits(76, 1) != 0
                && ctx.insn.bits(73, 2) == 1
                && ctx.insn.bits(75, 1) == 0
                && ctx.insn.words[1] <= 32
            {
                // SHF.R.U64 funnel: the translator reads bits 20-38 as the immediate, so the U64
                // clamp (bit 38) would turn every shift into 63; shifts up to 32 need no clamp.
                ctx.emit(
                    ctx.base(0x38f8_0000_0000_0000u64)
                        | ((ctx.insn.bits(24, 8) as u64) << 8)
                        | ((ctx.insn.words[1] as u64) << 20)
                        | ((ctx.insn.bits(64, 8) as u64) << 39),
                );
                return Some(StepAction::Continue);
            }
            if ctx.insn.bits(80, 1) == 0
                && ctx.insn.bits(76, 1) == 0
                && ctx.insn.bits(64, 8) == 255
                && ctx.insn.bits(73, 2) == 3
                && ctx.insn.bits(75, 1) == 0
                && ctx.insn.words[1] <= 32
            {
                ctx.emit(
                    ctx.base(0x3848_0000_0000_0000u64)
                        | ((ctx.insn.bits(24, 8) as u64) << 8)
                        | ((ctx.insn.words[1] as u64) << 20),
                );
                return Some(StepAction::Continue);
            }
            if ctx.insn.bits(80, 1) == 0
                && ctx.insn.bits(76, 1) != 0
                && ctx.insn.bits(64, 8) == 255
                && (ctx.insn.bits(73, 2) == 3 || is_signed)
                && ctx.insn.bits(75, 1) == 0
                && ctx.insn.words[1] <= 32
            {
                // SHF.R low word with a zero high word: a logical right shift.
                ctx.emit(
                    ctx.base(0x3828_0000_0000_0000u64)
                        | ((ctx.insn.bits(24, 8) as u64) << 8)
                        | ((ctx.insn.words[1] as u64) << 20),
                );
                return Some(StepAction::Continue);
            }
            if (ctx.insn.bits(73, 2) != 3 && !is_signed)
                || ctx.insn.bits(75, 1) != 0
                || ctx.insn.bits(80, 1) == 0
                || ctx.insn.words[1] > 32
            {
                return fail(ctx, "native funnel-shift mode");
            }
            let low = ctx.insn.bits(24, 8);
            let high = ctx.insn.bits(64, 8);
            let shift = ctx.insn.words[1];
            if ctx.insn.bits(76, 1) != 0 {
                if low != 255 {
                    return fail(ctx, "native nonzero low right-shift word");
                }
                ctx.emit(
                    ctx.base(0x3828_0000_0000_0000u64)
                        | ((high as u64) << 8)
                        | ((shift as u64) << 20)
                        | ((is_signed as u64) << 48),
                );
            } else {
                if is_signed {
                    return fail(ctx, "native signed left funnel shift");
                }
                ctx.emit(
                    ctx.base(0x36f8_0000_0000_0000u64)
                        | ((low as u64) << 8)
                        | ((high as u64) << 39)
                        | ((shift as u64) << 20),
                );
            }
            Some(StepAction::Continue)
        }
        0xa11 => {
            // LEA with constant base and carry predicate.
            if (ctx.insn.words[2] & !(0x0780_f800u32 | 0xffu32)) == 0x000f_0400
                && ctx.insn.bits(62, 2) == 0
            {
                // LEA.HI.X with a high source: high word of (Rc:Ra << shift), plus the constant
                // high word and the carry predicate (bits 87-90).
                let b = ctx.operand(1, false);
                ctx.emit(
                    0x36f8_0000_0000_0000u64
                        | ctx.predicate
                        | 250
                        | ((ctx.insn.bits(24, 8) as u64) << 8)
                        | ((ctx.insn.bits(64, 8) as u64) << 39)
                        | ((ctx.insn.bits(75, 5) as u64) << 20),
                );
                ctx.mov_imm(252, 1);
                ctx.emit(
                    0x5ca0_0000_0000_0000u64
                        | ctx.predicate
                        | 252
                        | (252u64 << 8)
                        | (255u64 << 20)
                        | ((ctx.insn.bits(87, 4) as u64) << 39),
                );
                ctx.emit(
                    0x5c10_0000_0000_0000u64 | ctx.predicate | 250 | (250u64 << 8) | ((b as u64) << 20),
                );
                ctx.emit(ctx.base(0x5c10_0000_0000_0000u64) | (250u64 << 8) | (252u64 << 20));
                return Some(StepAction::Continue);
            }
            if (ctx.insn.words[2] & !(0x0780_0000u32 | 0x0000_f800u32)) == 0x000f_06ff
                && ctx.insn.bits(62, 2) == 0
                && ctx.insn.bits(75, 5) != 0
            {
                // LEA.HI.X.SX32, no high source: (sign-extended Ra << shift) >> 32, plus the
                // constant high word and the carry predicate (bits 87-90).
                let b = ctx.operand(1, false);
                ctx.emit(
                    0x3828_0000_0000_0000u64
                        | ctx.predicate
                        | 250
                        | ((ctx.insn.bits(24, 8) as u64) << 8)
                        | (((32 - ctx.insn.bits(75, 5)) as u64) << 20)
                        | (1u64 << 48),
                );
                ctx.mov_imm(252, 1);
                ctx.emit(
                    0x5ca0_0000_0000_0000u64
                        | ctx.predicate
                        | 252
                        | (252u64 << 8)
                        | (255u64 << 20)
                        | ((ctx.insn.bits(87, 3) as u64) << 39)
                        | ((ctx.insn.bits(90, 1) as u64) << 42),
                );
                ctx.emit(
                    0x5c10_0000_0000_0000u64 | ctx.predicate | 250 | (250u64 << 8) | ((b as u64) << 20),
                );
                ctx.emit(ctx.base(0x5c10_0000_0000_0000u64) | (250u64 << 8) | (252u64 << 20));
                return Some(StepAction::Continue);
            }
            if ctx.insn.bits(80, 1) != 0
                && ctx.insn.bits(72, 1) == 0
                && ctx.insn.bits(74, 1) == 0
                && ctx.insn.bits(81, 3) == 7
                && ctx.insn.bits(64, 8) == 255
                && ctx.insn.bits(62, 2) == 0
                && ctx.insn.bits(75, 5) != 0
            {
                // LEA.HI Rd, Ra, c[][], RZ, s without .X: the constant plus the high word of
                // Ra << s, a right shift by 32 - s that sign-extends with .SX32 (bit 73).
                let b = ctx.operand(1, false);
                ctx.emit(
                    0x3828_0000_0000_0000u64
                        | ctx.predicate
                        | 250
                        | ((ctx.insn.bits(24, 8) as u64) << 8)
                        | (((32 - ctx.insn.bits(75, 5)) as u64) << 20)
                        | ((ctx.insn.bits(73, 1) as u64) << 48),
                );
                ctx.emit(ctx.base(0x5c10_0000_0000_0000u64) | (250u64 << 8) | ((b as u64) << 20));
                return Some(StepAction::Continue);
            }
            if ctx.insn.bits(72, 1) != 0
                || ctx.insn.bits(74, 1) != 0
                || ctx.insn.bits(80, 1) != 0
                || ctx.insn.bits(62, 2) != 0
            {
                return fail(ctx, "native constant LEA carry mode");
            }
            // LEA Rd, Ra, c[][], s is ISCADD; a carry out (bits 81-83) also compares the sum
            // against the constant, as the low half of a 64-bit address.
            let b = ctx.operand(1, false);
            ctx.emit(
                ctx.base(0x5c18_0000_0000_0000u64)
                    | ((ctx.insn.bits(24, 8) as u64) << 8)
                    | ((b as u64) << 20)
                    | ((ctx.insn.bits(75, 5) as u64) << 39),
            );
            if ctx.insn.bits(81, 3) != 7 {
                ctx.emit(
                    0x5b60_0000_0000_0000u64
                        | ctx.predicate
                        | 7
                        | ((ctx.insn.bits(81, 3) as u64) << 3)
                        | (ctx.dest << 8)
                        | ((b as u64) << 20)
                        | (7u64 << 39)
                        | (1u64 << 49),
                );
            }
            Some(StepAction::Continue)
        }
        0x211 | 0x811 | 0xc11 => {
            // LEA low word (0xc11: uniform Rb)
            if ctx.insn.bits(80, 1) != 0 {
                // LEA.HI Rd, Ra, Rb (or an immediate, 0x811), Rc, s: Rb + high word of (Rc:Ra << s);
                // bit 73 sign-extends Ra (Rc is RZ) and .X (bit 74) adds the carry predicate in bits 87-90.
                let extended = ctx.insn.bits(74, 1) != 0;
                let high = ctx.insn.bits(64, 8);
                if ctx.insn.bits(72, 1) != 0
                    || ctx.insn.bits(81, 3) != 7
                    || (high != 255 && ctx.insn.bits(73, 1) != 0)
                    || (ctx.opcode == 0x211
                        && (ctx.insn.bits(62, 1) != 0
                            || (ctx.insn.bits(63, 1) != 0 && extended)))
                    || (ctx.opcode == 0xc11 && (ctx.insn.words[1] & !0x3fu32) != 0)
                {
                    return fail(ctx, "native high effective address mode");
                }
                let mut b = if ctx.opcode == 0xc11 {
                    ctx.uniform_register(ctx.insn.bits(32, 8))
                } else {
                    ctx.insn.bits(32, 8)
                };
                if b == u32::MAX {
                    return fail(ctx, "native uniform LEA register");
                }
                if ctx.opcode == 0x811 {
                    b = if ctx.insn.words[1] != 0 { 251 } else { 255 };
                    if b == 251 {
                        ctx.mov_imm(251, ctx.insn.words[1]);
                    }
                }
                let shift = (((32 - ctx.insn.bits(75, 5)) as u64) << 20)
                    | ((ctx.insn.bits(73, 1) as u64) << 48);
                // The high word is a right shift of Ra, or with Rc a left funnel shift (SHF.L).
                let high_word = |ctx: &mut LowerCtx<'_>, target: u64| {
                    if high == 255 {
                        ctx.emit(
                            target
                                | 0x3828_0000_0000_0000u64
                                | ((ctx.insn.bits(24, 8) as u64) << 8)
                                | shift,
                        );
                    } else {
                        ctx.emit(
                            target
                                | 0x36f8_0000_0000_0000u64
                                | ((ctx.insn.bits(24, 8) as u64) << 8)
                                | ((high as u64) << 39)
                                | ((ctx.insn.bits(75, 5) as u64) << 20),
                        );
                    }
                };
                if b == 255 && !extended {
                    let base = ctx.base(0);
                    high_word(ctx, base);
                } else {
                    let target = ctx.predicate | 250;
                    high_word(ctx, target);
                    if extended {
                        ctx.mov_imm(252, 1);
                        ctx.emit(
                            0x5ca0_0000_0000_0000u64
                                | ctx.predicate
                                | 252
                                | (252u64 << 8)
                                | (255u64 << 20)
                                | ((ctx.insn.bits(87, 3) as u64) << 39)
                                | ((ctx.insn.bits(90, 1) as u64) << 42),
                        );
                        ctx.emit(
                            0x5c10_0000_0000_0000u64
                                | ctx.predicate
                                | 250
                                | (250u64 << 8)
                                | (252u64 << 20),
                        );
                    }
                    // 0x211 bit 63: -Rb, Maxwell IADD's negated second source (bit 48).
                    ctx.emit(
                        ctx.base(0x5c10_0000_0000_0000u64)
                            | (250u64 << 8)
                            | ((b as u64) << 20)
                            | (((if ctx.opcode == 0x211 {
                                ctx.insn.bits(63, 1)
                            } else {
                                0
                            }) as u64)
                                << 48),
                    );
                }
                return Some(StepAction::Continue);
            }
            // A carry out (bits 81-83) is the low half of a 64-bit address; IADD3.X adds it next.
            // ULEA's carry can go to any UPn (Ocarina of Time: ULEA UR10, UP3, UR7, UR10, 0x2):
            // it is computed in P(6-n) and merged into R239 like UISETP.
            let carry_out = ctx.insn.bits(81, 3);
            if ctx.insn.bits(72, 1) != 0
                || ctx.insn.bits(74, 1) != 0
                || ctx.insn.bits(80, 1) != 0
                || (carry_out != 7
                    && ((ctx.opcode != 0x811 && ctx.insn.bits(63, 1) != 0)
                        || (ctx.rewritten_uniform
                            && (ctx.opcode != 0x211 || (!ctx.lower_extended() && carry_out > 1)))))
                || (ctx.opcode == 0x211 && ctx.insn.bits(62, 1) != 0)
                || (ctx.opcode == 0xc11 && (ctx.insn.words[1] & !0x8000_003fu32) != 0)
            {
                return fail(ctx, "native LEA carry/high mode");
            }
            // 0x211 bit 63 negates Rb (LEA Rd, Ra, -Rb, s), as Maxwell ISCADD's bit 48.
            let mut b = if ctx.opcode == 0xc11 {
                ctx.uniform_register(ctx.insn.bits(32, 8))
            } else {
                ctx.insn.bits(32, 8)
            };
            if b == u32::MAX {
                return fail(ctx, "native uniform LEA register");
            }
            if ctx.opcode == 0x811 {
                ctx.mov_imm(251, ctx.insn.words[1]);
                b = 251;
            }
            if carry_out != 7 {
                // The sum in R250, carry = sum < Rb (unsigned), as IADD3's carry out; a uniform
                // predicate goes through P(6-n) into R239.
                let target = if ctx.rewritten_uniform {
                    6 - carry_out
                } else {
                    carry_out
                };
                let mask = 1u32 << target;
                ctx.emit(
                    0x5c18_0000_0000_0000u64
                        | ctx.predicate
                        | 250
                        | ((ctx.insn.bits(24, 8) as u64) << 8)
                        | ((b as u64) << 20)
                        | ((ctx.insn.bits(75, 5) as u64) << 39),
                );
                if ctx.rewritten_uniform {
                    ctx.emit(
                        0x38e8_0000_0007_0000u64 | 253 | (255u64 << 8) | ((mask as u64) << 20),
                    );
                }
                ctx.emit(
                    0x5b60_0000_0000_0000u64
                        | ctx.predicate
                        | 7
                        | ((target as u64) << 3)
                        | (250u64 << 8)
                        | ((b as u64) << 20)
                        | (7u64 << 39)
                        | (1u64 << 49),
                );
                if ctx.rewritten_uniform {
                    ctx.emit(
                        0x38e8_0000_0007_0000u64 | 239 | (239u64 << 8) | ((mask as u64) << 20),
                    );
                    ctx.emit(0x38f0_0000_0007_0000u64 | (253u64 << 8) | ((mask as u64) << 20));
                }
                ctx.emit(ctx.base(0x5c98_0000_0000_0000u64) | (15u64 << 39) | (250u64 << 20));
                return Some(StepAction::Continue);
            }
            ctx.emit(
                ctx.base(0x5c18_0000_0000_0000u64)
                    | ((ctx.insn.bits(24, 8) as u64) << 8)
                    | ((b as u64) << 20)
                    | ((ctx.insn.bits(75, 5) as u64) << 39)
                    | (((if ctx.opcode != 0x811 {
                        ctx.insn.bits(63, 1)
                    } else {
                        0
                    }) as u64)
                        << 48),
            );
            Some(StepAction::Continue)
        }
        0x210 | 0x810 | 0xa10 | 0xc10 => {
            // IADD3 with zero third source and no carry outputs
            // 64-bit adds: IADD3 with a carry-out predicate (bits 81-83), then IADD3.X (bit 74) with
            // a carry-in predicate (bits 87-89, negate 90). Uniform forms keep UPn in R239 bit 6-n.
            if (ctx.opcode == 0x210 || ctx.opcode == 0x810)
                && ctx.insn.bits(64, 8) == 255
                && ctx.insn.bits(73, 1) == 0
                && ctx.insn.bits(75, 2) == 0
                && ctx.insn.bits(84, 3) == 7
                && ctx.insn.bits(77, 4) == 15
                && ctx.insn.bits(72, 1) == 0
                && (ctx.insn.bits(81, 3) != 7 || ctx.insn.bits(74, 1) != 0)
                && (ctx.opcode == 0x810 || ctx.insn.bits(63, 1) == 0)
            {
                let carry_in = ctx.insn.bits(74, 1) != 0;
                let carry_out = ctx.insn.bits(81, 3);
                let a = ctx.insn.bits(24, 8);
                // A uniform carry goes through P(6-n) and R239 bit 6-n, so any UPn works.
                if (carry_in && carry_out != 7)
                    || (ctx.opcode == 0x210 && (ctx.insn.words[1] & !0xffu32) != 0)
                {
                    return fail(ctx, "native integer carry mode");
                }
                if ctx.opcode == 0x810 {
                    ctx.emit(
                        0x1c00_0000_0000_0000u64
                            | ctx.predicate
                            | 250
                            | ((a as u64) << 8)
                            | ((ctx.insn.words[1] as u64) << 20),
                    );
                } else {
                    ctx.emit(
                        0x5c10_0000_0000_0000u64
                            | ctx.predicate
                            | 250
                            | ((a as u64) << 8)
                            | ((ctx.insn.bits(32, 8) as u64) << 20),
                    );
                }
                if !carry_in {
                    // carry = (a + b) < a, unsigned
                    let target = if ctx.rewritten_uniform {
                        6 - carry_out
                    } else {
                        carry_out
                    };
                    let mask = 1u32 << target;
                    if ctx.rewritten_uniform {
                        ctx.save_predicate(mask);
                    }
                    ctx.emit(
                        0x5b60_0000_0000_0000u64
                            | ctx.predicate
                            | 7
                            | ((target as u64) << 3)
                            | (250u64 << 8)
                            | ((a as u64) << 20)
                            | (7u64 << 39)
                            | (1u64 << 49),
                    );
                    if ctx.rewritten_uniform {
                        ctx.emit(
                            0x38e8_0000_0007_0000u64 | 239 | (239u64 << 8) | ((mask as u64) << 20),
                        );
                        ctx.restore_predicate(mask);
                    }
                    ctx.emit(
                        ctx.base(0x5c98_0000_0000_0000u64) | (15u64 << 39) | (250u64 << 20),
                    );
                    return Some(StepAction::Continue);
                }
                let condition = ctx.insn.bits(87, 3);
                let source = if ctx.rewritten_uniform && condition != 7 {
                    6 - condition
                } else {
                    condition
                };
                let mask = 1u32 << source;
                ctx.mov_imm(252, 1);
                if ctx.rewritten_uniform && condition != 7 {
                    ctx.save_predicate(mask);
                    ctx.emit(0x38f0_0000_0007_0000u64 | (239u64 << 8) | ((mask as u64) << 20));
                }
                ctx.emit(
                    0x5ca0_0000_0000_0000u64
                        | ctx.predicate
                        | 251
                        | (252u64 << 8)
                        | (255u64 << 20)
                        | ((source as u64) << 39)
                        | ((ctx.insn.bits(90, 1) as u64) << 42),
                );
                if ctx.rewritten_uniform && condition != 7 {
                    ctx.restore_predicate(mask);
                }
                ctx.emit(ctx.base(0x5c10_0000_0000_0000u64) | (250u64 << 8) | (251u64 << 20));
                return Some(StepAction::Continue);
            }
            // Three sources, negations or two carries: the sum builds up in R253 (.CC) with the carry
            // count in R254 (.X). Pu/Pv (bits 81/84) are set when the count reaches 1/2, and IADD3.X
            // adds both carry-ins (bits 87, 77). A negated source is ~x, plus 1 unless extended.
            let extended = ctx.insn.bits(74, 1) != 0;
            let negate_a = ctx.insn.bits(72, 1) != 0;
            let negate_b = ctx.opcode != 0x810 && ctx.insn.bits(63, 1) != 0;
            let negate_c = ctx.insn.bits(75, 1) != 0;
            if (extended || ctx.insn.bits(81, 6) != 0x3f)
                && (ctx.insn.bits(64, 8) != 255
                    || ctx.insn.bits(84, 3) != 7
                    || negate_a
                    || negate_b
                    || negate_c
                    || (extended && ctx.insn.bits(77, 4) != 15))
            {
                let first_output = ctx.insn.bits(81, 3);
                let second_output = ctx.insn.bits(84, 3);
                if ctx.rewritten_uniform
                    || ctx.guarded_uniform
                    || ctx.insn.bits(73, 1) != 0
                    || ctx.insn.bits(76, 1) != 0
                    || (first_output != 7 && first_output == second_output)
                    || (!extended
                        && (ctx.insn.bits(77, 4) != 15 || ctx.insn.bits(87, 4) != 15))
                    || (ctx.opcode == 0x210 && (ctx.insn.words[1] & !0x8000_00ffu32) != 0)
                    || (ctx.opcode == 0xa10 && ctx.insn.bits(62, 1) != 0)
                    || (ctx.opcode == 0xc10 && (ctx.insn.words[1] & !0x8000_003fu32) != 0)
                {
                    return fail(ctx, "native three-input carry mode");
                }
                let mut a = ctx.insn.bits(24, 8);
                let mut b = ctx.operand(1, false);
                let mut c = ctx.insn.bits(64, 8);
                let invert = |ctx: &mut LowerCtx<'_>, source: &mut u32, target: u32| {
                    ctx.emit(
                        0x0400_0000_0000_0000u64
                            | ctx.predicate
                            | (target as u64)
                            | ((*source as u64) << 8)
                            | (0xffff_ffffu64 << 20)
                            | (2u64 << 53),
                    );
                    *source = target;
                };
                if negate_a {
                    invert(ctx, &mut a, 250);
                }
                if negate_b {
                    invert(ctx, &mut b, 251);
                }
                if negate_c {
                    invert(ctx, &mut c, 252);
                }
                let accumulate = |ctx: &mut LowerCtx<'_>, source: u32| {
                    ctx.emit(
                        0x5c10_0000_0000_0000u64
                            | ctx.predicate
                            | 253
                            | (253u64 << 8)
                            | ((source as u64) << 20)
                            | (1u64 << 47),
                    );
                    ctx.emit(
                        0x5c10_0000_0000_0000u64
                            | ctx.predicate
                            | 254
                            | (254u64 << 8)
                            | (255u64 << 20)
                            | (1u64 << 43),
                    );
                };
                ctx.emit(
                    0x5c10_0000_0000_0000u64
                        | ctx.predicate
                        | 253
                        | ((a as u64) << 8)
                        | ((b as u64) << 20)
                        | (1u64 << 47),
                );
                ctx.emit(
                    0x5c10_0000_0000_0000u64
                        | ctx.predicate
                        | 254
                        | (255u64 << 8)
                        | (255u64 << 20)
                        | (1u64 << 43),
                );
                accumulate(ctx, c);
                if extended {
                    for carry in [ctx.insn.bits(87, 4), ctx.insn.bits(77, 4)] {
                        if carry == 15 {
                            continue;
                        }
                        ctx.mov_imm(250, 1);
                        if carry != 7 {
                            ctx.emit(
                                0x5ca0_0000_0000_0000u64
                                    | ctx.predicate
                                    | 250
                                    | (250u64 << 8)
                                    | (255u64 << 20)
                                    | (((carry & 7) as u64) << 39)
                                    | (((carry >> 3) as u64) << 42),
                            );
                        }
                        accumulate(ctx, 250);
                    }
                } else {
                    let ones = (negate_a as u32) + (negate_b as u32) + (negate_c as u32);
                    if ones != 0 {
                        ctx.mov_imm(250, ones);
                        accumulate(ctx, 250);
                    }
                }
                if ctx.dest != 255 {
                    ctx.emit(
                        ctx.base(0x5c98_0000_0000_0000u64) | (253u64 << 20) | (15u64 << 39),
                    );
                }
                // An output that is also the guard predicate is written last.
                let guard_first =
                    (ctx.native_predicate & 7) != 7 && first_output == (ctx.native_predicate & 7);
                for output in [
                    if guard_first { 1u32 } else { 0u32 },
                    if guard_first { 0u32 } else { 1u32 },
                ] {
                    let target = if output != 0 {
                        second_output
                    } else {
                        first_output
                    };
                    if target == 7 {
                        continue;
                    }
                    ctx.mov_imm(250, output + 1);
                    ctx.emit(
                        0x5b60_0000_0000_0000u64
                            | ctx.predicate
                            | 7
                            | ((target as u64) << 3)
                            | (254u64 << 8)
                            | (250u64 << 20)
                            | (7u64 << 39)
                            | (6u64 << 49),
                    );
                }
                return Some(StepAction::Continue);
            }
            if (ctx.opcode == 0xa10 || ctx.opcode == 0xc10) && ctx.insn.bits(81, 3) != 7 {
                // cbuf or uniform b
                if ctx.insn.bits(64, 8) != 255
                    || ctx.insn.bits(73, 4) != 0
                    || ctx.insn.bits(84, 7) != 0x7f
                    || ctx.insn.bits(77, 4) != 15
                    || ctx.insn.bits(63, 1) != 0
                    || ctx.insn.bits(72, 1) != 0
                    || (ctx.opcode == 0xc10 && (ctx.insn.words[1] & !0x3fu32) != 0)
                {
                    return fail(ctx, "native constant add carry mode");
                }
                let b = ctx.operand(1, false);
                ctx.emit(
                    ctx.base(0x5c10_0000_0000_0000u64)
                        | ((ctx.insn.bits(24, 8) as u64) << 8)
                        | ((b as u64) << 20),
                );
                ctx.emit(
                    0x5b60_0000_0000_0000u64
                        | ctx.predicate
                        | 7
                        | ((ctx.insn.bits(81, 3) as u64) << 3)
                        | (ctx.dest << 8)
                        | ((b as u64) << 20)
                        | (7u64 << 39)
                        | (1u64 << 49),
                );
                return Some(StepAction::Continue);
            }
            if (ctx.opcode == 0xa10 || ctx.opcode == 0xc10) && ctx.insn.bits(74, 1) != 0 {
                // IADD3.X Ra + cbuf or UR + carry predicate
                if (ctx.insn.words[2]
                    & !(0x0780_0000u32
                        | if ctx.opcode == 0xc10 {
                            0x0800_0000u32
                        } else {
                            0u32
                        }))
                    != 0x007f_e4ff
                    || (ctx.opcode == 0xc10 && (ctx.insn.words[1] & !0x3fu32) != 0)
                {
                    return fail(ctx, "native extended constant add mode");
                }
                let a = ctx.insn.bits(24, 8);
                let b = ctx.operand(1, false);
                ctx.mov_imm(250, 1);
                ctx.emit(
                    0x5ca0_0000_0000_0000u64
                        | ctx.predicate
                        | 250
                        | (250u64 << 8)
                        | (255u64 << 20)
                        | ((ctx.insn.bits(87, 3) as u64) << 39)
                        | ((ctx.insn.bits(90, 1) as u64) << 42),
                );
                if a != 255 {
                    ctx.emit(
                        0x5c10_0000_0000_0000u64
                            | ctx.predicate
                            | 250
                            | ((a as u64) << 8)
                            | (250u64 << 20),
                    );
                }
                ctx.emit(ctx.base(0x5c10_0000_0000_0000u64) | ((b as u64) << 8) | (250u64 << 20));
                return Some(StepAction::Continue);
            }
            // Third source, optionally negated (bit 75; 63 and 72 negate b and a).
            if ctx.insn.bits(64, 8) != 255
                && (ctx.insn.bits(73, 4) & !4u32) == 0
                && ctx.insn.bits(77, 7) == 0x7f
                && ctx.insn.bits(84, 7) == 0x7f
            {
                let b = ctx.operand(1, false);
                // Immediate bit 63 belongs to the literal, not the register-source negation.
                ctx.emit(
                    0x5c10_0000_0000_0000u64
                        | ctx.predicate
                        | 250
                        | ((ctx.insn.bits(24, 8) as u64) << 8)
                        | ((b as u64) << 20)
                        | ((negate_b as u64) << 48)
                        | ((negate_a as u64) << 49),
                );
                ctx.emit(
                    ctx.base(0x5c10_0000_0000_0000u64)
                        | (250u64 << 8)
                        | ((ctx.insn.bits(64, 8) as u64) << 20)
                        | ((ctx.insn.bits(75, 1) as u64) << 48),
                );
                return Some(StepAction::Continue);
            }
            if ctx.insn.bits(64, 8) != 255
                || ctx.insn.bits(73, 4) != 0
                || ctx.insn.bits(77, 7) != 0x7f
                || ctx.insn.bits(84, 7) != 0x7f
            {
                return fail(ctx, "native integer carry/third source");
            }
            if ctx.opcode == 0xa10 || ctx.opcode == 0xc10 {
                if ctx.opcode == 0xc10 && (ctx.insn.words[1] & !0x8000_003fu32) != 0 {
                    return fail(ctx, "native uniform add operand");
                }
                let b = ctx.operand(1, false);
                ctx.emit(
                    ctx.base(0x5c10_0000_0000_0000u64)
                        | ((ctx.insn.bits(24, 8) as u64) << 8)
                        | ((b as u64) << 20)
                        | ((ctx.insn.bits(63, 1) as u64) << 48)
                        | ((ctx.insn.bits(72, 1) as u64) << 49),
                );
            } else if ctx.opcode == 0x210 {
                if (ctx.insn.words[1] & !0x8000_00ffu32) != 0 {
                    return fail(ctx, "native integer add register modifiers");
                }
                ctx.emit(
                    ctx.base(0x5c10_0000_0000_0000u64)
                        | ((ctx.insn.bits(24, 8) as u64) << 8)
                        | ((ctx.insn.bits(32, 8) as u64) << 20)
                        | ((ctx.insn.bits(63, 1) as u64) << 48)
                        | ((ctx.insn.bits(72, 1) as u64) << 49),
                );
            } else {
                ctx.emit(
                    ctx.base(0x1c00_0000_0000_0000u64)
                        | ((ctx.insn.bits(24, 8) as u64) << 8)
                        | ((ctx.insn.words[1] as u64) << 20)
                        | ((ctx.insn.bits(72, 1) as u64) << 56),
                );
            }
            Some(StepAction::Continue)
        }
        _ => None,
    }
}
