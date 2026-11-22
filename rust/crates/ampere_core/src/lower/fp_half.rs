// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use super::{HalfSource, LowerCtx, Relocation, StepAction};

const FMUL_SCALE_MAP: [u64; 8] = [0, 3, 2, 1, 0, 6, 5, 4];

fn clear_nan_to_r250(ctx: &mut LowerCtx<'_>, source: u32) {
    ctx.emit(
        0x5800_0000_0000_0000u64
            | ctx.predicate
            | 250
            | ((source as u64) << 8)
            | ((source as u64) << 20)
            | (7u64 << 39)
            | (7u64 << 48),
    );
    ctx.emit(
        0x5c40_0000_0000_0000u64
            | ctx.predicate
            | 250
            | ((source as u64) << 8)
            | (250u64 << 20)
            | (7u64 << 48),
    );
}

fn clamp_float_u8_to_r250(ctx: &mut LowerCtx<'_>, source: u32) {
    clear_nan_to_r250(ctx, source);
    ctx.mov_imm(252, 0);
    ctx.emit(
        0x5c60_0000_0000_0000u64
            | ctx.predicate
            | 250
            | (250u64 << 8)
            | (252u64 << 20)
            | (15u64 << 39),
    );
    ctx.mov_imm(252, 0x437f_0000u32);
    ctx.emit(
        0x5c60_0000_0000_0000u64
            | ctx.predicate
            | 250
            | (250u64 << 8)
            | (252u64 << 20)
            | (7u64 << 39),
    );
}

fn lower_hfma2(ctx: &mut LowerCtx<'_>) -> StepAction {
    let relu = ctx.insn.bits(79, 1) != 0;
    if ctx.insn.bits(78, 1) != 0
        || ctx.insn.bits(85, 1) != 0
        || ctx.insn.bits(91, 1) != 0
        || if relu {
            ctx.insn.bits(77, 1) != 0
        } else {
            ctx.insn.bits(87, 4) != 0
        }
    {
        return StepAction::Fail(ctx.index, "native half fused multiply-add mode".to_owned());
    }
    if relu && ctx.native_predicate != 7 {
        ctx.relocations.push(Relocation::new(
            ctx.result.instructions.len() as u32,
            (ctx.index + 1) as i64,
        ));
        ctx.emit(0xe240_0000_0000_000fu64 | (((ctx.native_predicate ^ 8u32) as u64) << 16));
        ctx.predicate = 7u64 << 16;
    }
    let mut a = ctx.half_source(0);
    let mut b = ctx.half_source(1);
    let c = ctx.half_source(2);
    if ctx.insn.bits(86, 1) != 0 {
        if b.swizzle != 0 || b.absolute || b.reg >= 250 {
            return StepAction::Fail(ctx.index, "native half mixed-sign source mode".to_owned());
        }
        ctx.emit(
            0x36f8_0000_0000_0000u64
                | ctx.predicate
                | 251
                | ((b.reg as u64) << 8)
                | (16u64 << 20)
                | ((b.reg as u64) << 39),
        );
        ctx.emit(
            0x0400_0000_0000_0000u64
                | ctx.predicate
                | 251
                | (251u64 << 8)
                | (0x8000u64 << 20)
                | (2u64 << 53),
        );
        b.reg = 251;
    }
    if b.absolute || c.absolute || (a.absolute && a.reg >= 248) {
        return StepAction::Fail(
            ctx.index,
            "native half fused multiply-add absolute source".to_owned(),
        );
    }
    if a.absolute {
        let mask = if a.swizzle == 1 {
            0x7fff_ffffu64
        } else {
            0x7fff_7fffu64
        };
        ctx.emit(
            0x0400_0000_0000_0000u64 | ctx.predicate | 250 | ((a.reg as u64) << 8) | (mask << 20),
        );
        a.reg = 250;
        a.absolute = false;
    }
    let precision = if ctx.insn.bits(76, 1) != 0 {
        2
    } else {
        ctx.insn.bits(80, 1)
    };
    ctx.emit(
        0x5d00_0000_0000_0000u64
            | ctx.predicate
            | if relu { 254u64 } else { ctx.dest }
            | ((a.reg as u64) << 8)
            | ((b.reg as u64) << 20)
            | ((b.swizzle as u64) << 28)
            | ((c.negate as u64) << 30)
            | (((a.negate != b.negate) as u64) << 31)
            | ((ctx.insn.bits(77, 1) as u64) << 32)
            | ((c.swizzle as u64) << 35)
            | ((precision as u64) << 37)
            | ((c.reg as u64) << 39)
            | ((a.swizzle as u64) << 47),
    );
    if relu {
        ctx.save_predicate(1u32 << 6);
        for lane in 0..2u32 {
            ctx.emit(
                0x3828_0000_0007_0000u64 | 251 | (254u64 << 8) | (((lane * 16) as u64) << 20),
            );
            ctx.emit(0x0400_0000_0007_0000u64 | 251 | (251u64 << 8) | (0xffffu64 << 20));
            ctx.emit(0x0400_0000_0007_0000u64 | 252 | (251u64 << 8) | (0x7fffu64 << 20));
            ctx.mov_imm(250, 0x7c00);
            ctx.emit(
                0x5b60_0000_0007_0000u64
                    | 7
                    | (6u64 << 3)
                    | (250u64 << 8)
                    | (252u64 << 20)
                    | (7u64 << 39)
                    | (1u64 << 49),
            );
            ctx.emit(0x3828_0000_0007_0000u64 | 252 | (251u64 << 8) | (15u64 << 20));
            ctx.mov_imm(250, 1);
            ctx.emit(
                0x5c10_0000_0007_0000u64 | 252 | (252u64 << 8) | (250u64 << 20) | (1u64 << 48),
            );
            ctx.emit(
                0x5c40_0000_0007_0000u64 | 251 | (251u64 << 8) | (252u64 << 20) | (7u64 << 48),
            );
            ctx.mov_imm(250, 0x7fff);
            ctx.emit(
                0x5ca0_0000_0007_0000u64 | 251 | (250u64 << 8) | (251u64 << 20) | (6u64 << 39),
            );
            if lane == 0 {
                ctx.emit(ctx.base(0x5c98_0000_0000_0000u64) | (251u64 << 20) | (15u64 << 39));
            } else {
                ctx.emit(0x3848_0000_0007_0000u64 | 251 | (251u64 << 8) | (16u64 << 20));
                ctx.emit(
                    ctx.base(0x5c40_0000_0000_0000u64)
                        | (ctx.dest << 8)
                        | (251u64 << 20)
                        | (1u64 << 41)
                        | (7u64 << 48),
                );
            }
        }
        ctx.restore_predicate(1u32 << 6);
        if ctx.insn.bits(87, 4) != 7 {
            ctx.emit(
                ctx.base(0x5ca0_0000_0000_0000u64)
                    | (ctx.dest << 8)
                    | (254u64 << 20)
                    | ((ctx.insn.bits(87, 4) as u64) << 39),
            );
        }
    }
    StepAction::Continue
}

fn lower_f2i(ctx: &mut LowerCtx<'_>) -> StepAction {
    let uniform_mask = if ctx.opcode == 0xd05 {
        0x0800_0000u32
    } else {
        0u32
    };
    let mode = ctx.insn.words[2] & !uniform_mask;
    let half = ctx.opcode == 0x305 && (mode & !0x100u32) == 0x0010_f000u32;
    let small = mode == 0x0021_2800u32;
    let floor = (mode & !0x1_0100u32) == 0x0020_7000u32;
    let nearest = (mode & !0x1_0100u32) == 0x0020_3000u32;
    let round_up = (mode & !0x1_0100u32) == 0x0020_b000u32;
    let signed_byte = ctx.opcode == 0x305 && mode == 0x0021_e100u32;
    let short_trunc = ctx.opcode == 0x305 && (mode & !0x100u32) == 0x0021_e800u32;
    let absolute = ctx.opcode != 0xd05 && !half && ctx.insn.bits(62, 1) != 0;
    let negate = ctx.opcode != 0xd05 && !half && ctx.insn.bits(63, 1) != 0;
    let unsigned_byte = ctx.opcode == 0x305 && (mode & !0x1_0000u32) == 0x0020_e000u32;
    let valid_words1_mask = if half {
        0x1000_00ffu32
    } else {
        0xffu32
            | if absolute { 0x4000_0000u32 } else { 0u32 }
            | if negate { 0x8000_0000u32 } else { 0u32 }
    };
    if (!half
        && !small
        && !floor
        && !nearest
        && !round_up
        && !signed_byte
        && !short_trunc
        && !unsigned_byte
        && (mode & !0x1_0100u32) != 0x0020_f000u32)
        || ctx.insn.bits(24, 8) != 0
        || (ctx.insn.words[3] & 0xc000_01ffu32) != 0
        || (ctx.opcode == 0xb05 && (ctx.insn.words[1] & 0x3800_003fu32) != 0)
        || (ctx.opcode != 0xb05 && (ctx.insn.words[1] & !valid_words1_mask) != 0)
        || (ctx.opcode == 0xd05 && ctx.uniform_register(ctx.insn.bits(32, 8)) == u32::MAX)
    {
        return StepAction::Fail(
            ctx.index,
            "native float-to-integer conversion mode".to_owned(),
        );
    }
    let is_signed = ctx.insn.bits(72, 1) != 0;
    let mut source = if ctx.opcode == 0xb05 {
        ctx.materialize(2, 0, 252, absolute, negate)
    } else if ctx.opcode == 0xd05 {
        ctx.uniform_register(ctx.insn.bits(32, 8))
    } else {
        ctx.insn.bits(32, 8)
    };
    if half {
        ctx.extract_half(
            HalfSource {
                reg: source,
                swizzle: if ctx.insn.bits(60, 1) != 0 { 3 } else { 2 },
                absolute: false,
                negate: false,
            },
            0,
            252,
        );
        source = 252;
    }
    if ctx.opcode == 0x305 && (absolute || negate) {
        source = ctx.materialize(0, source, 249, absolute, negate);
    }
    if unsigned_byte {
        clamp_float_u8_to_r250(ctx, source);
        source = 250;
    } else if !is_signed {
        clear_nan_to_r250(ctx, source);
        if !small && !short_trunc {
            ctx.mov_imm(252, 0x4f80_0000);
            ctx.emit(
                0x5800_0000_0000_0000u64
                    | ctx.predicate
                    | 251
                    | (250u64 << 8)
                    | (252u64 << 20)
                    | (7u64 << 39)
                    | (6u64 << 48),
            );
            ctx.mov_imm(252, 0x4f7f_ffff);
            ctx.emit(
                0x5c60_0000_0000_0000u64
                    | ctx.predicate
                    | 250
                    | (250u64 << 8)
                    | (252u64 << 20)
                    | (7u64 << 39),
            );
        }
        source = 250;
    }
    let dst_size = if small || signed_byte || short_trunc {
        1u64
    } else {
        2u64
    };
    let rounding = if small || nearest {
        0u64
    } else if floor {
        1u64
    } else if round_up {
        2u64
    } else {
        3u64
    };
    ctx.emit(
        ctx.base(0x5cb0_0000_0000_0000u64)
            | (dst_size << 8)
            | (2u64 << 10)
            | ((is_signed as u64) << 12)
            | ((source as u64) << 20)
            | (rounding << 39)
            | ((ctx.insn.bits(80, 1) as u64) << 44),
    );
    if signed_byte {
        ctx.emit(
            ctx.base(0x5ce0_0000_0000_0000u64)
                | (1u64 << 10)
                | (1u64 << 12)
                | (1u64 << 13)
                | (ctx.dest << 20)
                | (1u64 << 50),
        );
    }
    if !is_signed && !small && !short_trunc && !unsigned_byte {
        ctx.emit(
            ctx.base(0x5c40_0000_0000_0000u64)
                | (ctx.dest << 8)
                | (251u64 << 20)
                | (1u64 << 41)
                | (7u64 << 48),
        );
    }
    StepAction::Continue
}

fn lower_setp(ctx: &mut LowerCtx<'_>) -> StepAction {
    let floating = (ctx.opcode & 0x1ff) == 0xb;
    if !floating && ctx.insn.bits(72, 1) != 0 {
        let compare = ctx.insn.bits(76, 3);
        let destination = ctx.insn.bits(81, 3);
        let destination_b = ctx.insn.bits(84, 3);
        let low = ctx.insn.bits(68, 4);
        let accumulate = ctx.insn.bits(87, 4);
        let combine = ctx.insn.bits(74, 2);
        if compare == 0 || compare == 7 || combine == 3 {
            return StepAction::Fail(
                ctx.index,
                "native extended integer comparison mode".to_owned(),
            );
        }
        let a = ctx.operand(0, false);
        let b = ctx.operand(1, false);
        let mut temporary = 0u32;
        while temporary == destination
            || temporary == destination_b
            || temporary == (low & 7)
            || temporary == (accumulate & 7)
            || temporary == ctx.insn.bits(12, 3)
        {
            temporary += 1;
        }
        let mask = 1u32 << temporary;
        ctx.emit(0x38e8_0000_0007_0000u64 | 253 | (255u64 << 8) | ((mask as u64) << 20));
        let signed_bit = ctx.insn.bits(73, 1);
        let predicate = ctx.predicate;
        let isetp = |op: u32, input: u32, comb: u32| -> u64 {
            0x5b60_0000_0000_0000u64
                | predicate
                | 7
                | ((temporary as u64) << 3)
                | ((a as u64) << 8)
                | ((b as u64) << 20)
                | (((input & 7) as u64) << 39)
                | (((input >> 3) as u64) << 42)
                | ((comb as u64) << 45)
                | ((signed_bit as u64) << 48)
                | ((op as u64) << 49)
        };
        const AND: u32 = 0;
        const OR: u32 = 1;
        const LT: u32 = 1;
        const EQ: u32 = 2;
        const GT: u32 = 4;
        const NE: u32 = 5;
        if compare == EQ {
            ctx.emit(isetp(EQ, low, AND));
        } else if compare == NE {
            ctx.emit(isetp(NE, low, OR));
        } else {
            ctx.emit(isetp(EQ, low, AND));
            ctx.emit(isetp(if compare <= 3 { LT } else { GT }, temporary, OR));
        }
        ctx.emit(
            0x5090_0000_0000_0000u64
                | ctx.predicate
                | (destination_b as u64)
                | ((destination as u64) << 3)
                | ((temporary as u64) << 12)
                | (7u64 << 29)
                | (((accumulate & 7) as u64) << 39)
                | (((accumulate >> 3) as u64) << 42)
                | ((combine as u64) << 45),
        );
        ctx.emit(0x38f0_0000_0007_0000u64 | (253u64 << 8) | ((mask as u64) << 20));
        return StepAction::Continue;
    }
    let a = ctx.operand(0, floating);
    let b = ctx.operand(1, floating);
    let combine = ctx.insn.bits(74, 2);
    if combine > 2 {
        return StepAction::Fail(ctx.index, "native predicate combination".to_owned());
    }
    let mut out = if floating {
        0x5bb0_0000_0000_0000u64
    } else {
        0x5b60_0000_0000_0000u64
    } | ctx.predicate
        | (ctx.insn.bits(84, 3) as u64)
        | ((ctx.insn.bits(81, 3) as u64) << 3)
        | ((a as u64) << 8)
        | ((b as u64) << 20)
        | ((ctx.insn.bits(87, 3) as u64) << 39)
        | ((ctx.insn.bits(90, 1) as u64) << 42)
        | ((combine as u64) << 45);
    if floating {
        out |= ((ctx.insn.bits(80, 1) as u64) << 47) | ((ctx.insn.bits(76, 4) as u64) << 48);
    } else {
        out |= ((ctx.insn.bits(73, 1) as u64) << 48) | ((ctx.insn.bits(76, 3) as u64) << 49);
    }
    ctx.emit(out);
    StepAction::Continue
}

pub(super) fn try_lower(ctx: &mut LowerCtx<'_>) -> Option<StepAction> {
    match ctx.opcode {
        0x304 => {
            if ctx.insn.words[2] != 0x0020_0800
                || ctx.insn.bits(24, 8) != 0
                || (ctx.insn.words[1] & !0xc000_00ffu32) != 0
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native scalar float conversion mode".to_owned(),
                ));
            }
            ctx.emit(
                ctx.base(0x5ca8_0000_0000_0000u64)
                    | (1u64 << 8)
                    | (2u64 << 10)
                    | ((ctx.insn.bits(32, 8) as u64) << 20)
                    | ((ctx.insn.bits(62, 1) as u64) << 49)
                    | ((ctx.insn.bits(63, 1) as u64) << 45),
            );
            Some(StepAction::Continue)
        }
        0x23e | 0xa3e => {
            if ctx.insn.bits(62, 2) != 0
                || ctx.insn.bits(73, 5) != 0
                || ctx.insn.bits(79, 2) != 0
                || ctx.insn.bits(81, 10) != 0
                || ctx.insn.bits(91, 1) != 0
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native float packing modifiers".to_owned(),
                ));
            }
            let merge = ctx.insn.bits(78, 1) != 0;
            if ctx.insn.bits(72, 1) != 0 {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native float packing selector".to_owned(),
                ));
            }
            let a = if merge {
                ctx.operand(2, false)
            } else {
                ctx.operand(0, false)
            };
            let b = ctx.operand(1, false);
            if merge {
                ctx.emit(
                    0x5c98_0000_0000_0000u64
                        | ctx.predicate
                        | 250
                        | ((a as u64) << 20)
                        | (15u64 << 39),
                );
            } else {
                ctx.emit(
                    0x5ca8_0000_0000_0000u64
                        | ctx.predicate
                        | 250
                        | (1u64 << 8)
                        | (2u64 << 10)
                        | ((a as u64) << 20),
                );
            }
            ctx.emit(
                0x5ca8_0000_0000_0000u64
                    | ctx.predicate
                    | 251
                    | (1u64 << 8)
                    | (2u64 << 10)
                    | ((b as u64) << 20),
            );
            ctx.emit(
                ctx.base(0x5c18_0000_0000_0000u64)
                    | (250u64 << 8)
                    | (251u64 << 20)
                    | (16u64 << 39),
            );
            Some(StepAction::Continue)
        }
        0x232 | 0x832 | 0xa32 => {
            if ctx.insn.bits(78, 2) != 0
                || ctx.insn.bits(81, 10) != 0
                || ctx.insn.bits(91, 1) != 0
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native half multiply mode".to_owned(),
                ));
            }
            let a = ctx.half_source(0);
            let b = ctx.half_source(1);
            let precision = if ctx.insn.bits(76, 1) != 0 {
                2
            } else {
                ctx.insn.bits(80, 1)
            };
            ctx.emit(
                ctx.base(0x5d08_0000_0000_0000u64)
                    | ((a.reg as u64) << 8)
                    | ((b.reg as u64) << 20)
                    | ((b.swizzle as u64) << 28)
                    | ((b.absolute as u64) << 30)
                    | (((a.negate != b.negate) as u64) << 31)
                    | ((ctx.insn.bits(77, 1) as u64) << 32)
                    | ((precision as u64) << 39)
                    | ((a.absolute as u64) << 44)
                    | ((a.swizzle as u64) << 47),
            );
            Some(StepAction::Continue)
        }
        0x230 | 0x430 | 0x630 => {
            if ctx.insn.bits(76, 1) != 0
                || ctx.insn.bits(79, 1) != 0
                || ctx.insn.bits(81, 10) != 0
                || ctx.insn.bits(91, 1) != 0
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native half add mode".to_owned(),
                ));
            }
            let a = ctx.half_source(0);
            let b = ctx.half_source(if ctx.opcode == 0x230 { 1 } else { 2 });
            if ctx.insn.bits(78, 1) != 0 {
                if a.reg != 255
                    || !a.negate
                    || a.absolute
                    || a.swizzle != 0
                    || b.swizzle < 2
                    || ctx.insn.bits(77, 1) != 0
                    || ctx.insn.bits(80, 1) != 0
                {
                    return Some(StepAction::Fail(
                        ctx.index,
                        "native float-output half add mode".to_owned(),
                    ));
                }
                ctx.extract_half(b, 0, ctx.dest as u32);
            } else {
                ctx.emit(
                    ctx.base(0x5d10_0000_0000_0000u64)
                        | ((a.reg as u64) << 8)
                        | ((b.reg as u64) << 20)
                        | ((b.swizzle as u64) << 28)
                        | ((b.absolute as u64) << 30)
                        | ((b.negate as u64) << 31)
                        | ((ctx.insn.bits(77, 1) as u64) << 32)
                        | ((ctx.insn.bits(80, 1) as u64) << 39)
                        | ((a.negate as u64) << 43)
                        | ((a.absolute as u64) << 44)
                        | ((a.swizzle as u64) << 47),
                );
            }
            Some(StepAction::Continue)
        }
        0x231 | 0x431 | 0x631 | 0x831 | 0xa31 => Some(lower_hfma2(ctx)),
        0x233 | 0x433 | 0x633 => {
            let combine = ctx.insn.bits(69, 2);
            if ctx.insn.bits(64, 5) != 0
                || combine > 2
                || ctx.insn.bits(81, 6) != 0
                || ctx.insn.bits(91, 1) != 0
                || (ctx.opcode == 0x233 && (ctx.insn.words[1] & 0x0fff_ff00u32) != 0)
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native packed half comparison mode".to_owned(),
                ));
            }
            let a = ctx.half_source(0);
            let b = ctx.half_source(if ctx.opcode == 0x233 { 1 } else { 2 });
            ctx.emit(
                ctx.base(0x5d18_0000_0000_0000u64)
                    | ((a.reg as u64) << 8)
                    | ((b.reg as u64) << 20)
                    | ((b.swizzle as u64) << 28)
                    | ((b.absolute as u64) << 30)
                    | ((b.negate as u64) << 31)
                    | ((ctx.insn.bits(76, 4) as u64) << 35)
                    | ((ctx.insn.bits(87, 4) as u64) << 39)
                    | ((a.negate as u64) << 43)
                    | ((a.absolute as u64) << 44)
                    | ((combine as u64) << 45)
                    | ((a.swizzle as u64) << 47)
                    | ((ctx.insn.bits(71, 1) as u64) << 49)
                    | ((ctx.insn.bits(80, 1) as u64) << 50),
            );
            Some(StepAction::Continue)
        }
        0x240 | 0x840 => {
            if ctx.insn.bits(76, 11) != 0 || ctx.insn.bits(91, 1) != 0 {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native half min/max mode".to_owned(),
                ));
            }
            let a = ctx.half_source(0);
            let b = if ctx.opcode == 0x840 {
                HalfSource {
                    reg: 255,
                    swizzle: 0,
                    absolute: false,
                    negate: false,
                }
            } else {
                ctx.half_source(1)
            };
            if a.swizzle == 1 || b.swizzle == 1 || (b.reg >= 250 && b.reg != 255) {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native half min/max source mode".to_owned(),
                ));
            }
            for lane in 0..2u32 {
                ctx.extract_half(a, lane, 250);
                if ctx.opcode == 0x840 {
                    let half = (ctx.insn.words[1] >> (lane * 16)) & 0xffff;
                    ctx.mov_imm(251, half);
                    ctx.extract_half(
                        HalfSource {
                            reg: 251,
                            swizzle: 2,
                            absolute: false,
                            negate: false,
                        },
                        0,
                        251,
                    );
                } else {
                    ctx.extract_half(b, lane, 251);
                }
                let target = if lane == 0 { 252u32 } else { 250u32 };
                ctx.emit(
                    0x5c60_0000_0000_0000u64
                        | ctx.predicate
                        | (target as u64)
                        | (250u64 << 8)
                        | (251u64 << 20)
                        | ((ctx.insn.bits(87, 3) as u64) << 39)
                        | ((ctx.insn.bits(90, 1) as u64) << 42),
                );
                ctx.emit(
                    0x5ca8_0000_0000_0000u64
                        | ctx.predicate
                        | (target as u64)
                        | (1u64 << 8)
                        | (2u64 << 10)
                        | ((target as u64) << 20),
                );
            }
            ctx.emit(
                ctx.base(0x5c18_0000_0000_0000u64)
                    | (250u64 << 8)
                    | (252u64 << 20)
                    | (16u64 << 39),
            );
            Some(StepAction::Continue)
        }
        0x822 => {
            let swizzle = ctx.insn.words[1];
            if swizzle > 0xff || ctx.insn.words[2] > 255 {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native swizzled add mode".to_owned(),
                ));
            }
            let maxwell = ((swizzle & 0x55) << 1) | ((swizzle & 0xaa) >> 1);
            ctx.emit(
                ctx.base(0x50f8_0000_0000_0000u64)
                    | ((ctx.insn.bits(24, 8) as u64) << 8)
                    | ((ctx.insn.bits(64, 8) as u64) << 20)
                    | ((maxwell as u64) << 28),
            );
            Some(StepAction::Continue)
        }
        0x311 => {
            if ctx.insn.words[2] != 0x0021_d800
                || (ctx.insn.words[1] & !0xffu32) != 0
                || ctx.insn.bits(24, 8) != 0
                || (ctx.dest & 1) != 0
                || ctx.dest >= 246
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native unsigned 64-bit float conversion mode".to_owned(),
                ));
            }
            ctx.emit(
                ctx.base(0x5cb0_0000_0000_0000u64)
                    | (3u64 << 8)
                    | (2u64 << 10)
                    | ((ctx.insn.bits(32, 8) as u64) << 20)
                    | (3u64 << 39)
                    | (1u64 << 44),
            );
            Some(StepAction::Continue)
        }
        0x312 => {
            if (ctx.insn.words[2] & !0xc400u32) != 0x0030_1000
                || (ctx.insn.words[1] & !0xffu32) != 0
                || ctx.insn.bits(24, 8) != 0
                || (ctx.insn.bits(32, 8) & 1) != 0
                || ctx.insn.bits(32, 8) >= 254
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native 64-bit integer-to-float mode".to_owned(),
                ));
            }
            ctx.emit(
                ctx.base(0x5cb8_0000_0000_0000u64)
                    | (2u64 << 8)
                    | (3u64 << 10)
                    | ((ctx.insn.bits(74, 1) as u64) << 13)
                    | ((ctx.insn.bits(32, 8) as u64) << 20)
                    | ((ctx.insn.bits(78, 2) as u64) << 39),
            );
            Some(StepAction::Continue)
        }
        0x243 => {
            if (ctx.insn.words[1] & !0xffu32) != 0
                || (ctx.insn.words[2] & !0xc0ffu32) != 0x400u32
                || (!ctx.lower_extended() && ctx.insn.bits(24, 8) != 255)
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native packed float-to-byte mode".to_owned(),
                ));
            }
            if ctx.insn.bits(24, 8) != 255
                || ctx.insn.bits(64, 8) != 255
                || ctx.insn.bits(78, 2) != 3
            {
                let rounding = ctx.insn.bits(78, 2);
                let convert_byte = |ctx: &mut LowerCtx<'_>, source: u32| {
                    clamp_float_u8_to_r250(ctx, source);
                    ctx.emit(
                        0x5cb0_0000_0000_0000u64
                            | ctx.predicate
                            | 250
                            | (2u64 << 8)
                            | (2u64 << 10)
                            | (250u64 << 20)
                            | ((rounding as u64) << 39),
                    );
                };
                convert_byte(ctx, ctx.insn.bits(32, 8));
                ctx.emit(
                    0x5c98_0000_0000_0000u64
                        | ctx.predicate
                        | 251
                        | (15u64 << 39)
                        | (250u64 << 20),
                );
                if ctx.insn.bits(24, 8) != 255 {
                    convert_byte(ctx, ctx.insn.bits(24, 8));
                    ctx.emit(
                        0x3848_0000_0000_0000u64 | ctx.predicate | 250 | (250u64 << 8) | (8u64 << 20),
                    );
                    ctx.emit(
                        0x5c40_0000_0000_0000u64
                            | ctx.predicate
                            | 251
                            | (251u64 << 8)
                            | (250u64 << 20)
                            | (1u64 << 41)
                            | (7u64 << 48),
                    );
                }
                if ctx.insn.bits(64, 8) != 255 {
                    ctx.mov_imm(252, 0xffff_0000u32);
                    ctx.emit(
                        0x5c40_0000_0000_0000u64
                            | ctx.predicate
                            | 250
                            | ((ctx.insn.bits(64, 8) as u64) << 8)
                            | (252u64 << 20)
                            | (7u64 << 48),
                    );
                    ctx.emit(
                        0x5c40_0000_0000_0000u64
                            | ctx.predicate
                            | 251
                            | (251u64 << 8)
                            | (250u64 << 20)
                            | (1u64 << 41)
                            | (7u64 << 48),
                    );
                }
                ctx.emit(
                    ctx.base(0x5c98_0000_0000_0000u64) | (15u64 << 39) | (251u64 << 20),
                );
                return Some(StepAction::Continue);
            }
            ctx.insn.words[0] &= 0x00ff_ffffu32;
            ctx.insn.words[2] = 0x0021_e000u32;
            ctx.opcode = 0x305;
            Some(lower_f2i(ctx))
        }
        0x305 | 0xb05 | 0xd05 => Some(lower_f2i(ctx)),
        0x307 | 0xb07 => {
            if ctx.insn.bits(75, 2) != 2
                || ctx.insn.bits(84, 2) != 2
                || ctx.insn.bits(81, 3) != 0
                || ctx.insn.bits(86, 6) != 0
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native floating round format".to_owned(),
                ));
            }
            if ctx.opcode == 0xb07 && !ctx.valid_cbuf() {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native constant buffer address".to_owned(),
                ));
            }
            let op = if ctx.opcode == 0xb07 {
                0x4ca8_0000_0000_0000u64
            } else {
                0x5ca8_0000_0000_0000u64
            };
            let src_bits = if ctx.opcode == 0xb07 {
                ctx.cbuf()
            } else {
                (ctx.insn.bits(32, 8) as u64) << 20
            };
            ctx.emit(
                ctx.base(op)
                    | (2u64 << 8)
                    | (2u64 << 10)
                    | src_bits
                    | (((8 + ctx.insn.bits(78, 2)) as u64) << 39)
                    | ((ctx.insn.bits(80, 1) as u64) << 44)
                    | ((ctx.insn.bits(63, 1) as u64) << 45)
                    | ((ctx.insn.bits(62, 1) as u64) << 49),
            );
            Some(StepAction::Continue)
        }
        0x223 | 0x423 | 0x623 | 0x823 | 0xa23 | 0xc23 | 0xe23 => {
            let allowed_hi = if ctx.opcode == 0xe23 || ctx.opcode == 0xc23 {
                0x400u32
            } else {
                0u32
            };
            if ctx.insn.bits(76, 1) != 0 || (ctx.insn.bits(81, 11) & !allowed_hi) != 0 {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native FFMA denormal/source mode".to_owned(),
                ));
            }
            let a = ctx.operand(0, true);
            let b = ctx.operand(1, true);
            let c = ctx.operand(2, true);
            ctx.emit(
                ctx.base(0x5980_0000_0000_0000u64)
                    | ((a as u64) << 8)
                    | ((b as u64) << 20)
                    | ((c as u64) << 39)
                    | ((ctx.insn.bits(77, 1) as u64) << 50)
                    | ((ctx.insn.bits(78, 2) as u64) << 51)
                    | ((ctx.insn.bits(80, 1) as u64) << 53),
            );
            Some(StepAction::Continue)
        }
        0x220 | 0x820 | 0xa20 | 0xc20 => {
            let scale = ctx.insn.bits(84, 3);
            if ctx.insn.bits(76, 1) != 0
                || scale == 0
                || ctx.insn.bits(81, 3) != 0
                || ctx.insn.bits(87, 4) != 0
                || ctx.insn.bits(91, 1) != (ctx.opcode == 0xc20) as u32
                || (scale != 4 && (ctx.insn.bits(80, 1) == 0 || ctx.insn.bits(78, 2) != 0))
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native FMUL scale/mode".to_owned(),
                ));
            }
            let a = ctx.operand(0, true);
            let b = ctx.operand(1, true);
            ctx.emit(
                ctx.base(0x5c68_0000_0000_0000u64)
                    | ((a as u64) << 8)
                    | ((b as u64) << 20)
                    | ((ctx.insn.bits(78, 2) as u64) << 39)
                    | (FMUL_SCALE_MAP[scale as usize] << 41)
                    | ((ctx.insn.bits(80, 1) as u64) << 44)
                    | ((ctx.insn.bits(77, 1) as u64) << 50),
            );
            Some(StepAction::Continue)
        }
        0x221 | 0x421 | 0x621 | 0x821 | 0xa21 | 0xe21 => {
            if ctx.insn.bits(81, 10) != 0
                || ctx.insn.bits(91, 1) != (ctx.opcode == 0xe21) as u32
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native FADD mode".to_owned(),
                ));
            }
            let form = ctx.insn.bits(9, 3);
            let a = ctx.operand(0, true);
            let b = ctx.operand(if matches!(form, 2 | 3 | 7) { 2 } else { 1 }, true);
            ctx.emit(
                ctx.base(0x5c58_0000_0000_0000u64)
                    | ((a as u64) << 8)
                    | ((b as u64) << 20)
                    | ((ctx.insn.bits(78, 2) as u64) << 39)
                    | ((ctx.insn.bits(80, 1) as u64) << 44)
                    | ((ctx.insn.bits(77, 1) as u64) << 50),
            );
            Some(StepAction::Continue)
        }
        0x209 | 0x809 | 0xa09 | 0xc09 => {
            if ctx.insn.bits(74, 6) != 0
                || ctx.insn.bits(82, 5) != 0
                || (ctx.opcode != 0xc09 && ctx.insn.bits(91, 1) != 0)
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native floating min/max mode".to_owned(),
                ));
            }
            let a = ctx.operand(0, true);
            let b = ctx.operand(1, true);
            let propagate_nan = ctx.insn.bits(81, 1) != 0;
            if propagate_nan {
                ctx.emit(
                    0x5800_0000_0000_0000u64
                        | ctx.predicate
                        | 252
                        | ((a as u64) << 8)
                        | ((b as u64) << 20)
                        | (7u64 << 39)
                        | (7u64 << 48),
                );
            }
            ctx.emit(
                ctx.base(0x5c60_0000_0000_0000u64)
                    | ((a as u64) << 8)
                    | ((b as u64) << 20)
                    | ((ctx.insn.bits(87, 3) as u64) << 39)
                    | ((ctx.insn.bits(90, 1) as u64) << 42)
                    | ((ctx.insn.bits(80, 1) as u64) << 44),
            );
            if propagate_nan {
                ctx.emit(
                    ctx.base(0x5c40_0000_0000_0000u64)
                        | (ctx.dest << 8)
                        | (252u64 << 20)
                        | (7u64 << 48),
                );
                ctx.emit(
                    0x0400_0000_0000_0000u64
                        | ctx.predicate
                        | 252
                        | (252u64 << 8)
                        | (0xffff_ffffu64 << 20)
                        | (2u64 << 53),
                );
                ctx.emit(
                    0x0400_0000_0000_0000u64
                        | ctx.predicate
                        | 252
                        | (252u64 << 8)
                        | (0x7fff_ffffu64 << 20),
                );
                ctx.emit(
                    ctx.base(0x5c40_0000_0000_0000u64)
                        | (ctx.dest << 8)
                        | (252u64 << 20)
                        | (1u64 << 41)
                        | (7u64 << 48),
                );
            }
            Some(StepAction::Continue)
        }
        0x308 | 0xb08 | 0xd08 => {
            let operation = ctx.insn.bits(74, 6);
            let expected_hi = if ctx.opcode == 0xd08 { 0x800u32 } else { 0u32 };
            if operation > 9
                || operation == 6
                || operation == 7
                || (operation == 9 && ctx.insn.bits(73, 1) != 0)
                || ctx.insn.bits(80, 12) != expected_hi
            {
                return Some(StepAction::Fail(ctx.index, "native MUFU mode".to_owned()));
            }
            let turns = operation == 0 || operation == 1;
            let to_radians = |ctx: &mut LowerCtx<'_>, source: u32| -> u32 {
                ctx.emit(
                    0x1e00_0000_0000_0000u64
                        | ctx.predicate
                        | 250
                        | ((source as u64) << 8)
                        | (0x40c9_0fdbu64 << 20),
                );
                250u32
            };
            if ctx.insn.bits(73, 1) != 0 {
                if ctx.opcode != 0x308 || ctx.insn.bits(60, 2) > 1 {
                    return Some(StepAction::Fail(
                        ctx.index,
                        "native half special-function source".to_owned(),
                    ));
                }
                ctx.emit(
                    0x5ca8_0000_0000_0000u64
                        | ctx.predicate
                        | 250
                        | (2u64 << 8)
                        | (1u64 << 10)
                        | ((ctx.insn.bits(32, 8) as u64) << 20)
                        | ((ctx.insn.bits(60, 1) as u64) << 41)
                        | ((ctx.insn.bits(63, 1) as u64) << 45)
                        | ((ctx.insn.bits(62, 1) as u64) << 49),
                );
                if turns {
                    to_radians(ctx, 250);
                }
                ctx.emit(
                    0x5080_0000_0000_0000u64
                        | ctx.predicate
                        | 250
                        | (250u64 << 8)
                        | ((operation as u64) << 20),
                );
                ctx.emit(
                    ctx.base(0x5ca8_0000_0000_0000u64)
                        | (1u64 << 8)
                        | (2u64 << 10)
                        | (250u64 << 20),
                );
            } else {
                let mut src = if ctx.opcode == 0xd08 {
                    ctx.uniform_register(ctx.insn.bits(32, 8))
                } else {
                    ctx.operand(1, true)
                };
                if src == u32::MAX {
                    return Some(StepAction::Fail(
                        ctx.index,
                        "native MUFU uniform source".to_owned(),
                    ));
                }
                if turns {
                    src = to_radians(ctx, src);
                }
                ctx.emit(
                    ctx.base(0x5080_0000_0000_0000u64)
                        | ((src as u64) << 8)
                        | ((operation as u64) << 20),
                );
            }
            Some(StepAction::Continue)
        }
        0x306 | 0xd06 => {
            let source = if ctx.opcode == 0xd06 {
                ctx.uniform_register(ctx.insn.bits(32, 8))
            } else {
                ctx.insn.bits(32, 8)
            };
            let is_signed = ctx.insn.bits(74, 1) as u64;
            let uniform = ctx.opcode == 0xd06;
            let expected_half_mode = if uniform {
                0x0810_1000u32
            } else {
                0x0010_1000u32
            };
            if (ctx.insn.words[2] & !0x400u32) == expected_half_mode
                && (ctx.insn.words[1] & 0xefff_ff00u32) == 0
                && source != u32::MAX
            {
                ctx.emit(
                    ctx.base(0x5cb8_0000_0000_0000u64)
                        | (2u64 << 8)
                        | (1u64 << 10)
                        | (is_signed << 13)
                        | ((source as u64) << 20)
                        | (((ctx.insn.bits(60, 2) * 2) as u64) << 41),
                );
                return Some(StepAction::Continue);
            }
            let invalid_words1_mask = if !uniform || ctx.lower_extended() {
                0xcfff_ff00u32
            } else {
                0xffff_ff00u32
            };
            let expected_byte_mode = if uniform { 0x0800_1000u32 } else { 0x1000u32 };
            if (ctx.insn.words[2] & !0x400u32) != expected_byte_mode
                || (ctx.insn.words[1] & invalid_words1_mask) != 0
                || source == u32::MAX
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native byte-to-float conversion mode".to_owned(),
                ));
            }
            ctx.emit(
                ctx.base(0x5cb8_0000_0000_0000u64)
                    | (2u64 << 8)
                    | (is_signed << 13)
                    | ((source as u64) << 20)
                    | ((ctx.insn.bits(60, 2) as u64) << 41),
            );
            Some(StepAction::Continue)
        }
        0x906 => {
            if (ctx.insn.words[2] & !0xc400u32) != 0x0020_1000u32 {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native immediate integer-to-float mode".to_owned(),
                ));
            }
            ctx.mov_imm(250, ctx.insn.words[1]);
            ctx.emit(
                ctx.base(0x5cb8_0000_0000_0000u64)
                    | (2u64 << 8)
                    | (2u64 << 10)
                    | ((ctx.insn.bits(74, 1) as u64) << 13)
                    | (250u64 << 20)
                    | ((ctx.insn.bits(78, 2) as u64) << 39),
            );
            Some(StepAction::Continue)
        }
        0x245 | 0x845 | 0xa45 | 0xc45 => {
            let allowed = 0x400u32
                | 0xc000u32
                | if ctx.opcode == 0xc45 {
                    0x0800_0000u32
                } else {
                    0u32
                };
            if (ctx.insn.words[2] & !allowed) != 0x0020_1000u32 {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native packed integer-to-float format".to_owned(),
                ));
            }
            let src = ctx.operand(1, false);
            ctx.emit(
                ctx.base(0x5cb8_0000_0000_0000u64)
                    | (2u64 << 8)
                    | (2u64 << 10)
                    | ((ctx.insn.bits(74, 1) as u64) << 13)
                    | ((src as u64) << 20)
                    | ((ctx.insn.bits(78, 2) as u64) << 39),
            );
            Some(StepAction::Continue)
        }
        0x20a | 0x80a | 0xa0a | 0xc0a => {
            if ctx.insn.bits(64, 8) != 0
                || ctx.insn.bits(74, 2) == 3
                || ctx.insn.bits(81, 6) != 0
                || (ctx.opcode != 0xc0a && ctx.insn.bits(91, 1) != 0)
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native floating comparison output mode".to_owned(),
                ));
            }
            let a = ctx.operand(0, true);
            let b = ctx.operand(1, true);
            ctx.emit(
                ctx.base(0x5800_0000_0000_0000u64)
                    | ((a as u64) << 8)
                    | ((b as u64) << 20)
                    | ((ctx.insn.bits(87, 4) as u64) << 39)
                    | ((ctx.insn.bits(74, 2) as u64) << 45)
                    | ((ctx.insn.bits(76, 4) as u64) << 48)
                    | (1u64 << 52)
                    | ((ctx.insn.bits(80, 1) as u64) << 55),
            );
            Some(StepAction::Continue)
        }
        0x234 | 0x434 | 0x634 => {
            let mut source = 250u64 << 20;
            if ctx.opcode == 0x234 {
                if (ctx.insn.words[1] & 0x0fff_ff00u32) != 0 || ctx.insn.bits(64, 5) != 0 {
                    return Some(StepAction::Fail(
                        ctx.index,
                        "native half comparison mode".to_owned(),
                    ));
                }
                source = ((ctx.insn.bits(32, 8) as u64) << 20)
                    | ((ctx.insn.bits(60, 2) as u64) << 28)
                    | ((ctx.insn.bits(62, 1) as u64) << 30)
                    | ((ctx.insn.bits(63, 1) as u64) << 31);
            } else if ctx.opcode == 0x634 {
                if ctx.insn.bits(64, 5) != 0 {
                    return Some(StepAction::Fail(
                        ctx.index,
                        "native half comparison mode".to_owned(),
                    ));
                }
                if ctx.materialize(2, 255, 250, false, false) == 255 {
                    return Some(StepAction::Continue);
                }
                source |= ((ctx.insn.bits(60, 2) as u64) << 28)
                    | ((ctx.insn.bits(62, 1) as u64) << 30)
                    | ((ctx.insn.bits(63, 1) as u64) << 31);
            } else {
                ctx.mov_imm(250, ctx.insn.words[1]);
            }
            ctx.emit(
                0x5d20_0000_0000_0000u64
                    | ctx.predicate
                    | (ctx.insn.bits(84, 3) as u64)
                    | ((ctx.insn.bits(81, 3) as u64) << 3)
                    | ((ctx.insn.bits(80, 1) as u64) << 6)
                    | ((ctx.insn.bits(24, 8) as u64) << 8)
                    | source
                    | ((ctx.insn.bits(76, 4) as u64) << 35)
                    | ((ctx.insn.bits(87, 3) as u64) << 39)
                    | ((ctx.insn.bits(90, 1) as u64) << 42)
                    | ((ctx.insn.bits(72, 1) as u64) << 43)
                    | ((ctx.insn.bits(73, 1) as u64) << 44)
                    | ((ctx.insn.bits(69, 2) as u64) << 45)
                    | ((ctx.insn.bits(74, 2) as u64) << 47)
                    | ((ctx.insn.bits(71, 1) as u64) << 49),
            );
            Some(StepAction::Continue)
        }
        0x20b | 0x80b | 0xa0b | 0xc0b | 0x20c | 0x80c | 0xa0c | 0xc0c => Some(lower_setp(ctx)),
        _ => None,
    }
}
