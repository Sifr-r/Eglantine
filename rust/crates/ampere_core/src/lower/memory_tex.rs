// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use super::{LowerCtx, StepAction};

const RG_MASKS: [u32; 8] = [1, 2, 4, 8, 3, 9, 10, 12];
const RGBA_MASKS: [u32; 5] = [7, 11, 13, 14, 15];

#[inline]
fn fail(ctx: &LowerCtx<'_>, msg: impl Into<String>) -> StepAction {
    StepAction::Fail(ctx.index, msg.into())
}

#[inline]
fn emit_mov(ctx: &mut LowerCtx<'_>, target: u32, source: u32) {
    ctx.emit(
        0x5c98_0000_0000_0000u64
            | ctx.predicate
            | (target as u64)
            | (15u64 << 39)
            | ((source as u64) << 20),
    );
}

#[inline]
fn find_swizzle(mask: u32, split: bool) -> Option<u32> {
    let table: &[u32] = if split { &RGBA_MASKS } else { &RG_MASKS };
    table
        .iter()
        .position(|&entry| entry == mask)
        .map(|idx| idx as u32)
}

#[inline]
fn scatter_250(ctx: &mut LowerCtx<'_>, components: u32, second_dest: u32) {
    let split = second_dest != 255;
    for i in 0..components {
        let target = if !split || i < 2 {
            if ctx.dest == 255 {
                255
            } else {
                (ctx.dest as u32) + i
            }
        } else {
            second_dest + (i - 2)
        };
        if target != 255 {
            emit_mov(ctx, target, 250 + i);
        }
    }
}

fn lower_uldc(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let opcode = ctx.opcode;
    let uniform_dest = insn.bits(16, 8);
    let index_reg = insn.bits(24, 8);
    let width = insn.bits(73, 3);
    let target = ctx.uniform_register(uniform_dest);
    let index_target = if opcode == 0xabb {
        ctx.uniform_register(index_reg)
    } else {
        255
    };
    let lower_extended = ctx.lower_extended();
    let alignment = if !lower_extended {
        4
    } else if opcode == 0xabb {
        1
    } else if width >= 4 {
        4
    } else if width >= 2 {
        2
    } else {
        1
    };
    if !lower_extended && width < 4 {
        return fail(ctx, "native uniform constant load mode");
    }
    if target == u32::MAX
        || target == 255
        || width > 5
        || (width == 5
            && ((uniform_dest & 1) != 0
                || ctx.uniform_register(uniform_dest + 1) != target + 1))
        || ctx.bank() >= 18
        || !insn.bits(38, 16).is_multiple_of(alignment)
        || insn.bits(62, 2) != 0
        || (insn.words[2] & !0xe00u32)
            != if opcode == 0xab9 {
                0u32
            } else {
                0x0800_0000u32
            }
        || index_target == u32::MAX
    {
        return fail(ctx, "native uniform constant load mode");
    }
    if opcode == 0xab9 && width == 4 {
        ctx.emit(
            0x4c98_0000_0000_0000u64
                | ctx.predicate
                | (target as u64)
                | (15u64 << 39)
                | ctx.cbuf(),
        );
    } else {
        ctx.emit(
            0xef90_0000_0000_0000u64
                | ctx.predicate
                | (target as u64)
                | ((index_target as u64) << 8)
                | ((insn.bits(38, 16) as u64) << 20)
                | ((ctx.bank() as u64) << 36)
                | ((width as u64) << 48),
        );
    }
    StepAction::Continue
}

fn lower_ldc(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let width = insn.bits(73, 3);
    let mode = insn.bits(78, 2);
    if width > 5
        || (mode != 0 && mode != 2)
        || insn.bits(91, 1) != 0
        || ctx.bank() >= 18
        || (width == 5 && ((ctx.dest & 1) != 0 || ctx.dest >= 254))
    {
        return fail(
            ctx,
            format!(
                "native indexed constant load mode: width={} mode={} bit91={} bank={}",
                width,
                mode,
                insn.bits(91, 1),
                ctx.bank()
            ),
        );
    }
    ctx.emit(
        ctx.base(0xef90_0000_0000_0000u64)
            | ((insn.bits(24, 8) as u64) << 8)
            | ((insn.bits(38, 16) as u64) << 20)
            | ((ctx.bank() as u64) << 36)
            | ((mode as u64) << 44)
            | ((width as u64) << 48),
    );
    StepAction::Continue
}

fn lower_txd(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let bindless = ctx.opcode == 0xf6c;
    let handle = if bindless {
        ctx.uniform_register(insn.bits(40, 6))
    } else {
        255
    };
    let binding = insn.bits(54, 5);
    let offset = insn.bits(40, 14);
    let coordinates = insn.bits(24, 8);
    let derivatives = insn.bits(32, 8);
    let dimension = insn.bits(61, 3);
    let high = insn.bits(64, 8);
    let mask = insn.bits(72, 4);
    let channels = mask.count_ones();
    if (insn.words[2] & !(0xfffu32 | (1u32 << 26)))
        != if bindless {
            0x081e_0000u32
        } else {
            0x001e_0000u32
        }
        || mask == 0
        || insn.bits(59, 2) != 0
        || (dimension != 1 && dimension != 5)
        || (if bindless {
            handle >= 248 || insn.bits(46, 13) != 0 || derivatives > 244 || coordinates > 245
        } else {
            binding >= 18 || offset > 0x1fff
        })
        || (channels > 1 && (ctx.dest & 1) != 0)
        || ctx.dest >= 248
        || (channels <= 2 && high != 255)
        || (high != 255 && ((channels > 3 && (high & 1) != 0) || high >= 248))
        || coordinates >= 248
        || derivatives >= 247
    {
        return fail(ctx, "native gradient texture mode");
    }
    if !bindless && !ctx.bind_texture_buffer(binding) {
        return fail(ctx, "native texture descriptor buffer conflict");
    }
    let consecutive = !bindless && (channels <= 2 || high as u64 == ctx.dest + 2);
    let target = if consecutive { ctx.dest } else { 248 };
    if bindless {
        emit_mov(ctx, 248, handle);
        let coord_count = if dimension == 5 { 3 } else { 2 };
        for i in 0..coord_count {
            emit_mov(ctx, 249 + i, coordinates + i);
        }
    }
    ctx.emit(
        (if bindless {
            0xde40_0000_0000_0000u64
        } else {
            0xde00_0000_0000_0000u64
        }) | ctx.predicate
            | target
            | ((if bindless { 248u64 } else { coordinates as u64 }) << 8)
            | ((derivatives as u64) << 20)
            | ((if dimension == 5 { 3u64 } else { 2u64 }) << 28)
            | ((mask as u64) << 31)
            | (if bindless {
                0u64
            } else {
                (offset as u64) << 36
            })
            | ((insn.bits(90, 1) as u64) << 49)
            | (7u64 << 51),
    );
    if !consecutive {
        for i in 0..channels {
            if i >= 2 && high == 255 {
                break;
            }
            let to = if i < 2 {
                (ctx.dest as u32) + i
            } else {
                high + (i - 2)
            };
            emit_mov(ctx, to, 248 + i);
        }
    }
    StepAction::Continue
}

fn lower_red(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let type_field = insn.bits(73, 3);
    let operation = insn.bits(87, 3);
    let uniform_address = insn.bits(24, 8) == 255;
    let variable = 0x0380_0e00u32 | if uniform_address { 0x3fu32 } else { 0u32 };
    let mode = if uniform_address {
        0x0c10_e100u32
    } else {
        0x0010_e100u32
    };
    let offset = ((insn.bits(40, 24) as i32) << 8) >> 8;
    if (insn.words[2] & !variable) != mode
        || (type_field != 0 && type_field != 1 && type_field != 3)
        || (type_field == 3 && operation != 0)
    {
        return fail(ctx, "native global reduction mode");
    }
    let address = if uniform_address {
        ctx.uniform_register(insn.bits(64, 6))
    } else {
        insn.bits(24, 8)
    };
    if address == u32::MAX || (address & 1) != 0 || !(-0x8_0000..=0x7_ffff).contains(&offset) {
        return fail(ctx, "native global reduction address");
    }
    ctx.emit(
        0xebf8_0000_0000_0000u64
            | ctx.predicate
            | (insn.bits(32, 8) as u64)
            | ((address as u64) << 8)
            | ((type_field as u64) << 20)
            | ((operation as u64) << 23)
            | (((offset as u32 & 0xf_ffff) as u64) << 28)
            | (1u64 << 48),
    );
    StepAction::Continue
}

fn lower_atoms_popc(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let stride = insn.bits(78, 2);
    let offset = insn.bits(40, 24);
    if ctx.dest != 255
        || (insn.words[1] & 0xff) != 0
        || (offset & 3) != 0
        || (insn.words[2] & !0xc000u32) != 0x0d00_003fu32
    {
        return fail(ctx, "native shared increment mode");
    }
    let mut address = insn.bits(24, 8);
    if stride != 0 && address != 255 {
        ctx.emit(
            0x3848_0000_0000_0000u64
                | ctx.predicate
                | 251
                | ((address as u64) << 8)
                | (((stride + 1) as u64) << 20),
        );
        address = 251;
    }
    ctx.mov_imm(250, 1);
    ctx.emit(
        0xec00_0000_0000_0000u64
            | ctx.predicate
            | 255
            | ((address as u64) << 8)
            | (250u64 << 20)
            | (((offset >> 2) as u64) << 30),
    );
    StepAction::Continue
}

fn lower_atoms(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let offset = insn.bits(40, 24);
    let operation = insn.bits(87, 4);
    if (insn.words[2] & !0x0780_0200u32) != 0 || operation > 8 || (offset & 3) != 0 {
        return fail(ctx, "native shared atomic mode");
    }
    ctx.emit(
        ctx.base(0xec00_0000_0000_0000u64)
            | ((insn.bits(24, 8) as u64) << 8)
            | ((insn.bits(32, 8) as u64) << 20)
            | ((insn.bits(73, 1) as u64) << 28)
            | (((offset >> 2) as u64) << 30)
            | ((operation as u64) << 52),
    );
    StepAction::Continue
}

fn lower_atomg_reg(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let address = insn.bits(24, 8);
    let offset = ((insn.bits(40, 24) as i32) << 8) >> 8;
    if insn.words[2] != 0x001e_e100
        || (address & 1) != 0
        || address >= 246
        || !(-0x8_0000..=0x7_ffff).contains(&offset)
    {
        return fail(ctx, "native global atomic register mode");
    }
    ctx.emit(
        ctx.base(0xed00_0000_0000_0000u64)
            | ((address as u64) << 8)
            | ((insn.bits(32, 8) as u64) << 20)
            | (((offset as u32 & 0xf_ffff) as u64) << 28)
            | (1u64 << 48),
    );
    StepAction::Continue
}

fn lower_atomg_uniform(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let address = ctx.uniform_register(insn.bits(64, 6));
    let offset = ((insn.bits(40, 24) as i32) << 8) >> 8;
    if insn.bits(24, 8) != 255
        || !(-0x8_0000..=0x7_ffff).contains(&offset)
        || (insn.words[2] & !0x3fu32) != 0x081e_e140
        || address == u32::MAX
        || (address & 1) != 0
    {
        return fail(ctx, "native global atomic mode");
    }
    ctx.emit(
        ctx.base(0xed00_0000_0000_0000u64)
            | ((address as u64) << 8)
            | ((insn.bits(32, 8) as u64) << 20)
            | (((offset as u32 & 0xf_ffff) as u64) << 28)
            | (1u64 << 48),
    );
    StepAction::Continue
}

fn lower_suld(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let binding = insn.bits(54, 5);
    let offset = insn.bits(40, 14);
    let coordinate = insn.bits(24, 8);
    let channels = insn.bits(72, 4);
    let dimension = insn.bits(61, 3);
    if (insn.words[2] & !0xf00u32) != 0x001e_0000
        || channels == 0
        || !(3..=5).contains(&dimension)
        || binding >= 18
        || offset > 0x1fff
        || insn.bits(59, 2) != 0
        || ctx.dest == 255
        || coordinate > if dimension >= 4 { 252 } else { 253 }
    {
        return fail(ctx, "native formatted surface load mode");
    }
    if !ctx.bind_texture_buffer(binding) {
        return fail(ctx, "native surface descriptor buffer conflict");
    }
    ctx.emit(
        ctx.base(0xeb00_0000_0000_0000u64)
            | ((coordinate as u64) << 8)
            | ((channels as u64) << 20)
            | ((dimension as u64) << 33)
            | ((offset as u64) << 36)
            | (1u64 << 51),
    );
    StepAction::Continue
}

fn lower_sust(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let binding = insn.bits(54, 5);
    let offset = insn.bits(40, 14);
    let data = insn.bits(32, 8);
    let coordinate = insn.bits(24, 8);
    let dimension = insn.bits(61, 3);
    let channels = insn.bits(72, 4);
    let mode = insn.words[2] & !0xf00u32;
    let strong = mode == 0x0010_a000;
    if (mode != 0x0010_0000 && !strong)
        || channels == 0
        || !(3..=5).contains(&dimension)
        || binding >= 18
        || offset > 0x1fff
        || insn.bits(59, 2) != 0
        || data > 252
        || coordinate > if dimension >= 4 { 252 } else { 253 }
    {
        return fail(ctx, "native formatted surface store mode");
    }
    let bindless = !ctx.bind_texture_buffer(binding);
    if bindless {
        ctx.emit(
            0x4c98_0000_0007_0000u64
                | 250
                | (15u64 << 39)
                | ((offset as u64) << 20)
                | ((binding as u64) << 34),
        );
    }
    if strong {
        ctx.emit(0xef98_0000_0000_0000u64 | ctx.predicate | (1u64 << 8));
    }
    ctx.emit(
        0xeb20_0000_0000_0000u64
            | ctx.predicate
            | (data as u64)
            | ((coordinate as u64) << 8)
            | ((channels as u64) << 20)
            | ((dimension as u64) << 33)
            | if bindless {
                250u64 << 39
            } else {
                ((offset as u64) << 36) | (1u64 << 51)
            },
    );
    if strong {
        ctx.emit(0xef98_0000_0000_0000u64 | ctx.predicate | (1u64 << 8));
    }
    StepAction::Continue
}

fn lower_suatom(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let binding = insn.bits(54, 5);
    let offset = insn.bits(40, 14);
    let data = insn.bits(32, 8);
    let coordinate = insn.bits(24, 8);
    let dimension = insn.bits(61, 3);
    let operation = insn.bits(87, 3);
    if (insn.words[2] & !0x0380_0000u32) != 0x001e_e000u32
        || (operation != 0 && operation != 6)
        || (dimension != 3 && dimension != 5)
        || binding >= 18
        || offset > 0x1fff
        || insn.bits(59, 2) != 0
        || coordinate > if dimension == 5 { 252 } else { 253 }
    {
        return fail(ctx, "native surface reduction mode");
    }
    if !ctx.bind_texture_buffer(binding) {
        return fail(ctx, "native surface descriptor buffer conflict");
    }
    ctx.emit(
        ctx.base(0xea00_0000_0000_0000u64)
            | ((coordinate as u64) << 8)
            | ((data as u64) << 20)
            | ((operation as u64) << 29)
            | ((dimension as u64) << 33)
            | ((offset as u64) << 36),
    );
    StepAction::Continue
}

fn lower_sured(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let coordinate = insn.bits(24, 8);
    let data = insn.bits(32, 8);
    let offset = insn.bits(40, 14);
    let binding = insn.bits(54, 5);
    let dimension = insn.bits(61, 3);
    let type_field = insn.bits(73, 3);
    let operation = insn.bits(87, 4);
    let coordinates = if dimension <= 1 {
        1
    } else if dimension <= 3 {
        2
    } else {
        3
    };
    if (insn.words[2] & !((7u32 << 9) | (15u32 << 23))) != 0x0010_e000
        || insn.bits(59, 2) != 0
        || dimension > 5
        || type_field > 1
        || operation > 7
        || operation == 3
        || operation == 4
        || binding >= 18
        || offset > 0x1fff
        || (if coordinate == 255 {
            coordinates > 1
        } else {
            coordinate + coordinates > 255
        })
        || (data >= 253 && data != 255)
    {
        return fail(ctx, "native surface reduction mode");
    }
    if !ctx.bind_texture_buffer(binding) {
        return fail(ctx, "native surface descriptor buffer conflict");
    }
    ctx.emit(
        0xea00_0000_0000_0000u64
            | ctx.predicate
            | 255
            | ((coordinate as u64) << 8)
            | ((data as u64) << 20)
            | ((operation as u64) << 29)
            | ((dimension as u64) << 33)
            | ((offset as u64) << 36)
            | ((type_field as u64) << 51),
    );
    StepAction::Continue
}

fn lower_shared_mem(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let opcode = ctx.opcode;
    let width = insn.bits(73, 3);
    let stride = insn.bits(78, 2);
    let uniform_base = (opcode == 0x984 || opcode == 0x988) && insn.bits(91, 1) != 0;
    let base_register = if uniform_base {
        ctx.uniform_register(insn.bits(if opcode == 0x988 { 64 } else { 32 }, 8))
    } else {
        255
    };
    let ignored = if uniform_base {
        if opcode == 0x988 {
            0x0800_00ffu32
        } else {
            0x0800_0000u32
        }
    } else {
        0u32
    };
    if width > 6
        || (insn.words[2] & !ignored) != ((width << 9) | (stride << 14))
        || (opcode == 0x984 && !uniform_base && insn.bits(32, 8) != 0)
        || (opcode == 0x988 && !uniform_base)
        || base_register == u32::MAX
    {
        return fail(ctx, "native shared memory addressing mode");
    }
    let mut address = insn.bits(24, 8);
    if stride != 0 && address != 255 {
        ctx.emit(
            0x3848_0000_0000_0000u64
                | ctx.predicate
                | 250
                | ((address as u64) << 8)
                | (((stride + 1) as u64) << 20),
        );
        address = 250;
    }
    if base_register != 255 {
        ctx.emit(
            0x5c10_0000_0000_0000u64
                | ctx.predicate
                | 250
                | ((address as u64) << 8)
                | ((base_register as u64) << 20),
        );
        address = 250;
    }
    let store = opcode != 0x984;
    let data = if store {
        insn.bits(32, 8)
    } else {
        ctx.dest as u32
    };
    ctx.emit(
        (if store {
            0xef58_0000_0000_0000u64
        } else {
            0xef48_0000_0000_0000u64
        }) | ctx.predicate
            | (data as u64)
            | ((address as u64) << 8)
            | ((insn.bits(40, 24) as u64) << 20)
            | ((width as u64) << 48),
    );
    StepAction::Continue
}

fn lower_ldg(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let opcode = ctx.opcode;
    let size = insn.bits(73, 3);
    let mut address = insn.bits(24, 8);
    if size > 6
        || (opcode == 0x381 && insn.bits(32, 8) != 0)
        || (size == 5 && (ctx.dest & 1) != 0)
        || (size == 6 && (ctx.dest & 3) != 0)
    {
        return fail(ctx, "native global load mode");
    }
    if opcode == 0x381 {
        if (insn.words[2] & !0x2e00u32) != 0x001e_8100 || (address & 1) != 0 || address >= 246 {
            return fail(ctx, "native global load mode");
        }
    } else {
        let uniform_address = if insn.bits(38, 2) != 0 {
            u32::MAX
        } else {
            ctx.uniform_register(insn.bits(32, 6))
        };
        let descriptor_address = (insn.words[2] & !(0x3fu32 | 0xe00u32)) == 0x0c1e_1100;
        if (!descriptor_address
            && (insn.words[2] & !(0x0400_0000u32 | 0x3fu32 | 0xe00u32 | 0x2000u32)) != 0x081e_8100)
            || uniform_address == u32::MAX
            || (uniform_address & 1) != 0
        {
            return fail(ctx, "native global load uniform addressing mode");
        }
        if descriptor_address {
            if (address & 1) != 0 || address >= 246 {
                return fail(ctx, "native global load register address pair");
            }
        } else if address == 255 {
            address = uniform_address;
        } else {
            if insn.bits(90, 1) != 0 && ((address & 1) != 0 || address >= 246) {
                return fail(ctx, "native global load register address pair");
            }
            ctx.emit(
                0x5c10_0000_0000_0000u64
                    | ctx.predicate
                    | 250
                    | ((uniform_address as u64) << 8)
                    | ((address as u64) << 20)
                    | (1u64 << 47),
            );
            ctx.emit(
                0x5c10_0000_0000_0000u64
                    | ctx.predicate
                    | 251
                    | (((uniform_address + 1) as u64) << 8)
                    | ((if insn.bits(90, 1) != 0 {
                        address + 1
                    } else {
                        255
                    } as u64)
                        << 20)
                    | (1u64 << 43),
            );
            address = 250;
        }
    }
    ctx.emit(
        ctx.base(0xeed0_0000_0000_0000u64)
            | ((address as u64) << 8)
            | ((insn.bits(40, 24) as u64) << 20)
            | (1u64 << 45)
            | ((size as u64) << 48),
    );
    StepAction::Continue
}

fn lower_stg(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let opcode = ctx.opcode;
    let mut data = insn.bits(32, 8);
    let size = insn.bits(73, 3);
    if size > 6
        || (data != 255
            && ((size == 5 && (data & 1) != 0)
                || (size == 6 && (data & 3) != 0)
                || data + if size < 4 { 1 } else { 1u32 << (size - 4) } > 246))
    {
        return fail(ctx, "native global store size");
    }
    if data == 255 && size > 4 {
        data = if size == 6 { 248 } else { 250 };
        for i in 0..(1u32 << (size - 4)) {
            ctx.mov_imm(data + i, 0);
        }
    }
    let mut address = insn.bits(24, 8);
    let extended = insn.bits(72, 1) != 0;
    if opcode == 0x986 {
        let uniform_address = ctx.uniform_register(insn.bits(64, 6));
        let descriptor_address = (insn.words[2] & !(0x3fu32 | 0xe00u32)) == 0x0c10_1100;
        if (!descriptor_address
            && (insn.words[2] & !(0x0400_0000u32 | 0x3fu32 | 0xe00u32)) != 0x0810_a100)
            || uniform_address == u32::MAX
            || (uniform_address & 1) != 0
            || !extended
        {
            return fail(ctx, "native global store uniform addressing mode");
        }
        if descriptor_address {
            if (address & 1) != 0 || address >= 246 {
                return fail(ctx, "native global store register address pair");
            }
        } else if address == 255 {
            address = uniform_address;
        } else {
            if insn.bits(90, 1) != 0 && ((address & 1) != 0 || address >= 254) {
                return fail(ctx, "native global store register address pair");
            }
            ctx.emit(
                0x5c10_0000_0000_0000u64
                    | ctx.predicate
                    | 250
                    | ((uniform_address as u64) << 8)
                    | ((address as u64) << 20)
                    | (1u64 << 47),
            );
            ctx.emit(
                0x5c10_0000_0000_0000u64
                    | ctx.predicate
                    | 251
                    | (((uniform_address + 1) as u64) << 8)
                    | ((if insn.bits(90, 1) != 0 {
                        address + 1
                    } else {
                        255
                    } as u64)
                        << 20)
                    | (1u64 << 43),
            );
            address = 250;
        }
    } else if (insn.words[2] & !0xe00u32) != 0x0010_a100 {
        return fail(ctx, "native global store register addressing mode");
    }
    ctx.emit(
        0xeed8_0000_0000_0000u64
            | ctx.predicate
            | (data as u64)
            | ((address as u64) << 8)
            | ((insn.bits(40, 24) as u64) << 20)
            | ((extended as u64) << 45)
            | ((size as u64) << 48),
    );
    StepAction::Continue
}

fn lower_txq(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let bindless = ctx.opcode == 0x370;
    let binding = insn.bits(54, 5);
    let mask = insn.bits(72, 4);
    let components = mask.count_ones();
    let second_dest = insn.bits(64, 8);
    let direct = second_dest == 255;
    if insn.bits(59, 5) != if bindless { 1 } else { 0 }
        || mask == 0
        || (direct && components > 2)
        || insn.bits(76, 14) != 0
        || insn.bits(91, 1) != 0
        || (!bindless && binding >= 18)
        || (bindless && (insn.bits(32, 27) != 0 || insn.bits(24, 8) >= 245))
        || (!direct && (second_dest >= 245 || ctx.dest >= 245))
        || insn.bits(40, 14) > 0x1fff
        || (direct && components == 2 && (ctx.dest & 1) != 0)
        || ctx.dest >= 254
    {
        return fail(ctx, "native texture dimension query mode");
    }
    if !bindless && !ctx.bind_texture_buffer(binding) {
        return fail(ctx, "native texture descriptor buffer conflict");
    }
    ctx.emit(
        (if bindless {
            0xdf50_0000_0000_0000u64
        } else {
            0xdf48_0000_0000_0000u64
        }) | ctx.predicate
            | if direct { ctx.dest } else { 250 }
            | ((insn.bits(24, 8) as u64) << 8)
            | (1u64 << 22)
            | ((mask as u64) << 31)
            | ((if bindless { 0 } else { insn.bits(40, 14) } as u64) << 36)
            | ((insn.bits(90, 1) as u64) << 49),
    );
    if !direct {
        scatter_250(ctx, components, second_dest);
    }
    StepAction::Continue
}

fn lower_tld_scr(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let mut mask = insn.bits(72, 4);
    let level = insn.bits(87, 3) == 3;
    let offsets = insn.bits(76, 1) != 0;
    let dimension = insn.bits(61, 3);
    let array = dimension == 5;
    let volume = dimension == 2;
    let source_a = insn.bits(24, 8);
    let mut source_b = insn.bits(32, 8);
    let mut second_dest = insn.bits(64, 8);
    if second_dest == 255 && (mask == 5 || mask == 6) {
        mask |= 8;
        second_dest = 246;
    }
    let split = second_dest != 255;
    let swizzle = find_swizzle(mask, split);
    if insn.bits(60, 1) == 0
        || (dimension != 0 && dimension != 1 && !array && !volume)
        || swizzle.is_none()
        || (insn.bits(76, 5) & !1u32) != 0
        || (offsets
            && (dimension != 1
                || (source_a & 1) != 0
                || source_a >= 254
                || source_b == 255
                || (level && ((source_b & 1) != 0 || source_b >= 254))))
        || insn.bits(81, 3) != 7
        || (insn.bits(87, 3) != 1 && !level)
        || (level && volume)
        || (level && dimension == 1 && ((source_a & 1) != 0 || source_a >= 254))
        || (level && array && ((source_b & 1) != 0 || source_b >= 254))
        || insn.bits(59, 1) != 0
        || insn.bits(40, 14) > 0x1fff
        || insn.bits(54, 5) >= 18
        || ctx.dest == 255
        || ((mask & (mask - 1)) != 0 && ((ctx.dest & 1) != 0 || ctx.dest >= 254))
        || (split && mask == 15 && ((second_dest & 1) != 0 || second_dest >= 254))
        || (array && ((source_a & 1) != 0 || source_a >= 249))
        || (volume && ((source_a & 1) != 0 || source_a >= 254))
    {
        return fail(ctx, "native texel fetch mode or descriptor binding");
    }
    let swizzle = swizzle.unwrap_or(0);
    let binding = insn.bits(54, 5);
    if !ctx.bind_texture_buffer(binding) {
        if level || offsets {
            return fail(ctx, "native texture descriptor buffer conflict");
        }
        emit_mov(ctx, 250, source_a);
        if array || volume {
            emit_mov(ctx, 251, source_a + 1);
        }
        if dimension != 0 {
            emit_mov(ctx, if array || volume { 252 } else { 251 }, source_b);
        }
        ctx.emit(
            0x4c98_0000_0007_0000u64
                | 253
                | (15u64 << 39)
                | ((insn.bits(40, 14) as u64) << 20)
                | ((binding as u64) << 34),
        );
        let maxwell_dim = if array {
            3u64
        } else if volume {
            4u64
        } else if dimension == 0 {
            0u64
        } else {
            2u64
        };
        ctx.emit(
            0xdd00_0000_0000_0000u64
                | ctx.predicate
                | 250
                | (250u64 << 8)
                | (253u64 << 20)
                | (maxwell_dim << 28)
                | ((mask as u64) << 31)
                | ((insn.bits(90, 1) as u64) << 49)
                | (7u64 << 51),
        );
        let components = mask.count_ones();
        for i in 0..components {
            let target = if i < 2 || !split {
                (ctx.dest as u32) + i
            } else {
                second_dest + (i - 2)
            };
            emit_mov(ctx, target, 250 + i);
        }
        return StepAction::Continue;
    }
    if array && level {
        for i in 0..2u32 {
            emit_mov(ctx, 250 + i, source_a + i);
            emit_mov(ctx, 252 + i, source_b + i);
        }
        ctx.emit(
            0xdc00_0000_0000_0000u64
                | ctx.predicate
                | 246
                | (250u64 << 8)
                | (253u64 << 20)
                | (3u64 << 28)
                | ((mask as u64) << 31)
                | ((insn.bits(40, 14) as u64) << 36)
                | ((insn.bits(90, 1) as u64) << 49)
                | (7u64 << 51)
                | (1u64 << 55),
        );
        let components = mask.count_ones();
        for i in 0..components {
            let target = if !split || i < 2 {
                (ctx.dest as u32) + i
            } else {
                second_dest + (i - 2)
            };
            if target != 255 && (split || i < 2) {
                emit_mov(ctx, target, 246 + i);
            }
        }
        return StepAction::Continue;
    }
    if array {
        emit_mov(ctx, 250, source_a + 1);
        emit_mov(ctx, 251, source_b);
        source_b = 250;
    }
    let tlds_mode = if offsets {
        if level {
            12u64
        } else {
            4u64
        }
    } else if array {
        8u64
    } else if volume {
        7u64
    } else if dimension == 0 {
        if level {
            1u64
        } else {
            0u64
        }
    } else if level {
        5u64
    } else {
        2u64
    };
    ctx.emit(
        0xda00_0000_0000_0000u64
            | ctx.predicate
            | ctx.dest
            | ((source_a as u64) << 8)
            | ((source_b as u64) << 20)
            | ((second_dest as u64) << 28)
            | ((insn.bits(40, 14) as u64) << 36)
            | ((insn.bits(90, 1) as u64) << 49)
            | ((swizzle as u64) << 50)
            | (tlds_mode << 53),
    );
    StepAction::Continue
}

fn lower_tld_b(ctx: &mut LowerCtx<'_>) -> StepAction {
    const COORDINATE_COUNT: [u32; 8] = [1, 2, 3, 0, 0, 3, 0, 0];
    const MAXWELL_TYPE: [u32; 8] = [0, 2, 4, 0, 0, 3, 0, 0];
    let insn = ctx.insn;
    let mask = insn.bits(72, 4);
    let second_dest = insn.bits(64, 8);
    let source_a = insn.bits(24, 8);
    let source_b = insn.bits(32, 8);
    let dimension = insn.bits(61, 3);
    let coordinates = COORDINATE_COUNT[dimension as usize];
    let count = coordinates + 1;
    let in_a = count.div_ceil(2);
    if coordinates == 0
        || insn.bits(59, 1) == 0
        || insn.bits(60, 1) == 0
        || insn.bits(40, 19) != 0
        || insn.bits(76, 5) != 0
        || insn.bits(81, 3) != 7
        || insn.bits(87, 3) != 1
        || insn.bits(91, 1) != 0
        || mask == 0
        || source_a == 255
        || source_a + in_a > 246
        || source_b == 255
        || source_b + (count - in_a) > 246
    {
        return fail(ctx, "native bindless texel fetch mode");
    }
    let value = |i: u32| -> u32 {
        if i < in_a {
            source_a + i
        } else {
            source_b + (i - in_a)
        }
    };
    for i in 0..coordinates {
        emit_mov(ctx, 250 + i, value(i));
    }
    emit_mov(ctx, 246, value(coordinates));
    ctx.emit(
        0xdd00_0000_0000_0000u64
            | ctx.predicate
            | 250
            | (250u64 << 8)
            | (246u64 << 20)
            | ((MAXWELL_TYPE[dimension as usize] as u64) << 28)
            | ((mask as u64) << 31)
            | ((insn.bits(90, 1) as u64) << 49)
            | (7u64 << 51),
    );
    scatter_250(ctx, mask.count_ones(), second_dest);
    StepAction::Continue
}

fn lower_tld_uniform(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let handle = ctx.uniform_register(insn.bits(40, 6));
    let mask = insn.bits(72, 4);
    let second_dest = insn.bits(64, 8);
    let source_a = insn.bits(24, 8);
    let source_b = insn.bits(32, 8);
    let dimension = insn.bits(61, 3);
    let array = dimension == 5;
    let volume = dimension == 2;
    let split = second_dest != 255;
    let components = mask.count_ones();
    if handle == u32::MAX
        || handle >= 246
        || insn.bits(46, 13) != 0
        || insn.bits(91, 1) == 0
        || insn.bits(60, 1) == 0
        || insn.bits(59, 1) != 0
        || insn.bits(76, 5) != 0
        || insn.bits(81, 3) != 7
        || insn.bits(87, 3) != 1
        || (dimension != 0 && dimension != 1 && !array && !volume)
        || mask == 0
        || ((array || volume) && source_a >= 245)
        || (ctx.dest != 255 && (ctx.dest as u32) + if split { 2 } else { components } > 246)
        || (split && components > 2 && second_dest + components - 2 > 246)
    {
        return fail(ctx, "native bindless texel fetch mode");
    }
    emit_mov(ctx, 250, source_a);
    if array || volume {
        emit_mov(ctx, 251, source_a + 1);
    }
    if dimension != 0 {
        emit_mov(ctx, if array || volume { 252 } else { 251 }, source_b);
    }
    emit_mov(ctx, 246, handle);
    let maxwell_dim = if array {
        3u64
    } else if volume {
        4u64
    } else if dimension == 0 {
        0u64
    } else {
        2u64
    };
    ctx.emit(
        0xdd00_0000_0000_0000u64
            | ctx.predicate
            | 250
            | (250u64 << 8)
            | (246u64 << 20)
            | (maxwell_dim << 28)
            | ((mask as u64) << 31)
            | ((insn.bits(90, 1) as u64) << 49)
            | (7u64 << 51),
    );
    scatter_250(ctx, components, second_dest);
    StepAction::Continue
}

fn lower_ldgsts(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let global_address = insn.bits(24, 8);
    let shared_address = insn.bits(16, 8);
    let descriptor = insn.bits(64, 6);
    if ctx.native_predicate != 7
        || (global_address & 1) != 0
        || global_address >= 246
        || descriptor >= 63
        || (descriptor & 1) != 0
        || (insn.words[1] & 0xf000u32) != 0
        || (insn.words[2] & !(0x0380_0000u32 | 0x7fu32)) != 0x0c10_1c00
    {
        return fail(ctx, "native global-to-shared copy mode");
    }
    let enable = insn.bits(87, 3) | (insn.bits(70, 1) << 3);
    let global_immediate = insn.words[1] & 0xfff;
    let global_offset =
        (global_immediate as i32) - if (global_immediate & 0x800) != 0 { 0x1000 } else { 0 };
    let shared_offset = ((insn.words[1] >> 16) as u16 as i16 as i32) * 16;
    for component in 0..4u32 {
        ctx.mov_imm(248 + component, 0);
    }
    if enable != 15 {
        ctx.emit(
            0xeed0_0000_0000_0000u64
                | ((enable as u64) << 16)
                | 248
                | ((global_address as u64) << 8)
                | (((global_offset as u32 & 0xff_ffff) as u64) << 20)
                | (1u64 << 45)
                | (6u64 << 48),
        );
    }
    ctx.emit(
        0xef58_0000_0000_0000u64
            | ctx.predicate
            | 248
            | ((shared_address as u64) << 8)
            | (((shared_offset as u32 & 0xff_ffff) as u64) << 20)
            | (6u64 << 48),
    );
    StepAction::Continue
}

fn lower_ldgdepbar(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    if insn.words[0] != 0x79af || insn.words[1] != 0 || insn.words[2] != 0 {
        return fail(ctx, "native copy-group commit mode");
    }
    ctx.emit(0xef98_0000_0000_0000u64 | ctx.predicate);
    StepAction::Continue
}

fn lower_tmml(ctx: &mut LowerCtx<'_>) -> StepAction {
    const COORDINATE_COUNT: [u32; 4] = [1, 2, 3, 3];
    let insn = ctx.insn;
    let opcode = ctx.opcode;
    let bindless = opcode != 0xb69;
    let handle = if opcode == 0xf69 {
        ctx.uniform_register(insn.bits(40, 6))
    } else if opcode == 0x36a {
        insn.bits(32, 8)
    } else {
        255
    };
    let mask = insn.bits(72, 4);
    let second_dest = insn.bits(64, 8);
    let source_a = insn.bits(24, 8);
    let source_b = insn.bits(32, 8);
    let binding = insn.bits(54, 5);
    let dimension = insn.bits(61, 3);
    let coordinates = if dimension < 4 {
        COORDINATE_COUNT[dimension as usize]
    } else {
        0
    };
    let in_a = if source_b == 255 || opcode == 0x36a {
        coordinates
    } else {
        coordinates.div_ceil(2)
    };
    if coordinates == 0
        || insn.bits(76, 4) != 0
        || insn.bits(80, 10) != 0
        || mask == 0
        || insn.bits(59, 1) != if opcode == 0x36a { 1 } else { 0 }
        || source_a + in_a > 246
        || (in_a < coordinates && source_b + (coordinates - in_a) > 246)
        || (opcode == 0xf69
            && (handle >= 246 || insn.bits(46, 13) != 0 || insn.bits(91, 1) == 0))
        || (opcode == 0x36a
            && (handle >= 246 || insn.bits(40, 19) != 0 || insn.bits(91, 1) != 0))
        || (!bindless
            && (insn.bits(40, 14) > 0x1fff
                || binding >= 18
                || (ctx.result.required_texture_buffer != u32::MAX
                    && ctx.result.required_texture_buffer != binding)))
    {
        return fail(ctx, "native texture LOD query mode");
    }
    if !bindless {
        ctx.result.required_texture_buffer = binding;
    }
    for i in 0..coordinates {
        let src = if i < in_a {
            source_a + i
        } else {
            source_b + (i - in_a)
        };
        emit_mov(ctx, 250 + i, src);
    }
    if bindless {
        emit_mov(ctx, 246, handle);
    }
    ctx.emit(
        (if bindless {
            0xdf60_0000_0000_0000u64
        } else {
            0xdf58_0000_0000_0000u64
        }) | ctx.predicate
            | 250
            | (250u64 << 8)
            | ((if bindless { 246u64 } else { 255u64 }) << 20)
            | (((dimension * 2) as u64) << 28)
            | ((mask as u64) << 31)
            | ((if bindless { 0 } else { insn.bits(40, 14) } as u64) << 36)
            | ((insn.bits(90, 1) as u64) << 49),
    );
    scatter_250(ctx, mask.count_ones(), second_dest);
    StepAction::Continue
}

fn find_per_tap_offset_imm(ctx: &LowerCtx<'_>, reg: u32) -> Result<Option<u32>, StepAction> {
    let mut cursor = ctx.index;
    while cursor > 0 {
        if ctx.relocations.iter().any(|r| r.target == cursor as i64) {
            break;
        }
        cursor -= 1;
        let def = &ctx.code[cursor as usize];
        if def.bits(12, 4) == 15 {
            continue;
        }
        let op = def.bits(0, 12);
        let target = def.bits(16, 8);
        if matches!(op, 0x94d | 0x950 | 0x944 | 0x943) {
            break;
        }
        if matches!(op, 0x947 | 0x945 | 0x918 | 0x91a)
            || (op & 0x1ff) == 0x00c
            || (op & 0x1ff) == 0x00b
        {
            continue;
        }
        if op == 0xb63 {
            let count = def.bits(72, 4).count_ones();
            let high = def.bits(64, 8);
            let packed = def.bits(79, 1) != 0;
            let first_count = if packed {
                if high == 255 {
                    count.div_ceil(2)
                } else {
                    1
                }
            } else if high == 255 {
                count
            } else {
                count.min(2)
            };
            let second_count = if packed {
                if count > 2 {
                    1
                } else {
                    0
                }
            } else {
                count.saturating_sub(2)
            };
            if (target != 255 && reg >= target && reg < target + first_count)
                || (high != 255 && reg >= high && reg < high + second_count)
            {
                return Err(fail(ctx, "native per-tap gather offset clobber"));
            }
            continue;
        }
        if op == 0xb6c || op == 0xf6c {
            let channels = def.bits(72, 4).count_ones();
            let first_count = channels.clamp(1, 2);
            let second = def.bits(64, 8);
            let second_count = channels.saturating_sub(2);
            if (target != 255 && reg >= target && reg < target + first_count)
                || (second != 255 && reg >= second && reg < second + second_count)
            {
                return Err(fail(ctx, "native per-tap gather offset clobber"));
            }
            continue;
        }
        if matches!(
            op,
            0xb9b
                | 0x59f
                | 0x386
                | 0x986
                | 0x388
                | 0x988
                | 0x992
                | 0xb1d
                | 0x948
                | 0x348
                | 0x941
                | 0x942
                | 0x95b
        ) {
            continue;
        }
        let width = def.bits(73, 3);
        let count = if matches!(op, 0x225 | 0x625 | 0x825 | 0x805 | 0x23c)
            || (op == 0x305 && def.bits(75, 2) == 3)
        {
            2
        } else if matches!(op, 0x381 | 0x981 | 0x983 | 0x984) {
            if width == 6 {
                4
            } else if width == 5 {
                2
            } else {
                1
            }
        } else if op == 0x321 {
            def.bits(74, 2) + 1
        } else if op == 0x83b {
            1u32 << def.bits(72, 2)
        } else if op == 0xb82 {
            if width == 5 {
                2
            } else {
                1
            }
        } else {
            1
        };
        if target == 255 || reg < target || reg >= target + count {
            continue;
        }
        let is_move = op == 0x802 && def.bits(24, 8) == 0 && def.words[2] == 0xf00;
        let is_imad = op == 0x424 && def.bits(24, 8) == 255 && def.words[2] == 0x078e_00ff;
        if (!is_move && !is_imad)
            || (def.bits(12, 4) != 7 && def.bits(12, 4) != ctx.native_predicate)
            || (def.words[3] & 0xc000_01ffu32) != 0
        {
            return Err(fail(ctx, "native per-tap gather nonconstant offsets"));
        }
        for shift in (0..32u32).step_by(8) {
            let value = (def.words[1] >> shift) & 255;
            if value > 31 && value < 224 {
                return Err(fail(ctx, "native per-tap gather offset range"));
            }
        }
        return Ok(Some(def.words[1]));
    }
    Ok(None)
}

fn lower_tld4(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let bindless = ctx.opcode == 0xf63;
    let handle = if bindless {
        ctx.uniform_register(insn.bits(40, 6))
    } else {
        255
    };
    let mask = insn.bits(72, 4);
    let second_dest = insn.bits(64, 8);
    let source_a = insn.bits(24, 8);
    let source_b = insn.bits(32, 8);
    let binding = insn.bits(54, 5);
    let array = insn.bits(61, 3) == 5;
    let offset_mode = insn.bits(76, 2);
    let depth = insn.bits(78, 1) != 0;
    let coordinates = if array { 3u32 } else { 2u32 };
    let meta = (if offset_mode == 1 {
        1u32
    } else if offset_mode == 2 {
        2u32
    } else {
        0u32
    }) + if depth { 1u32 } else { 0u32 };
    let in_a = if meta != 0 {
        coordinates
    } else {
        coordinates.div_ceil(2)
    };
    let in_b = if meta != 0 { meta } else { coordinates - in_a };
    if (insn.bits(61, 3) != 1 && !array)
        || offset_mode == 3
        || insn.bits(79, 1) != 0
        || insn.bits(81, 3) != 7
        || insn.bits(59, 1) != 0
        || (bindless && (offset_mode != 0 || depth))
        || (offset_mode != 0 && source_b == 255)
        || (source_a != 255
            && source_a + in_a
                > if array || meta != 0 {
                    ctx.shadow_boundary
                } else {
                    246
                })
        || (source_b != 255 && source_b + in_b > 246)
        || insn.bits(89, 1) != 0
        || mask == 0
        || (if bindless {
            handle >= 246 || insn.bits(46, 13) != 0 || insn.bits(91, 1) == 0
        } else {
            insn.bits(40, 14) > 0x1fff || binding >= 18
        })
    {
        return fail(ctx, "native texture gather mode");
    }
    let second_buffer = !bindless && !ctx.bind_texture_buffer(binding);
    let element = |base: u32, offset: u32| -> u32 {
        if base == 255 {
            255
        } else {
            base + offset
        }
    };
    for i in 0..coordinates {
        let source = if i < in_a {
            element(source_a, i)
        } else {
            element(source_b, i - in_a)
        };
        emit_mov(ctx, 250 + i, source);
    }
    if second_buffer {
        ctx.emit(
            0x4c98_0000_0007_0000u64
                | 246
                | (15u64 << 39)
                | ((insn.bits(40, 14) as u64) << 20)
                | ((binding as u64) << 34),
        );
    }
    for i in 0..meta {
        let reg = element(source_b, i);
        let imm = if offset_mode == 2 && i < 2 {
            match find_per_tap_offset_imm(ctx, reg) {
                Ok(val) => val,
                Err(action) => return action,
            }
        } else {
            None
        };
        let meta_reg = 246 + if second_buffer { 1u32 } else { 0u32 } + i;
        if let Some(imm_val) = imm {
            ctx.emit(0x0100_0000_0007_f000u64 | (meta_reg as u64) | ((imm_val as u64) << 20));
        } else {
            emit_mov(ctx, meta_reg, reg);
        }
    }
    let dim_code = if array { 3u64 } else { 2u64 };
    if second_buffer {
        ctx.emit(
            0xdec0_0000_0000_0000u64
                | ctx.predicate
                | 250
                | (250u64 << 8)
                | (246u64 << 20)
                | (dim_code << 28)
                | ((mask as u64) << 31)
                | ((offset_mode as u64) << 36)
                | ((insn.bits(87, 2) as u64) << 38)
                | ((insn.bits(90, 1) as u64) << 49)
                | ((depth as u64) << 50)
                | (7u64 << 51),
        );
    } else if bindless {
        emit_mov(ctx, 246, handle);
        ctx.emit(
            0xdec0_0000_0000_0000u64
                | ctx.predicate
                | 250
                | (250u64 << 8)
                | (246u64 << 20)
                | (dim_code << 28)
                | ((mask as u64) << 31)
                | ((insn.bits(87, 2) as u64) << 38)
                | ((insn.bits(90, 1) as u64) << 49)
                | (7u64 << 51),
        );
    } else {
        ctx.emit(
            0xc800_0000_0000_0000u64
                | ctx.predicate
                | 250
                | (250u64 << 8)
                | ((if meta != 0 { 246u64 } else { 255u64 }) << 20)
                | (dim_code << 28)
                | ((mask as u64) << 31)
                | ((insn.bits(40, 14) as u64) << 36)
                | ((insn.bits(90, 1) as u64) << 49)
                | ((depth as u64) << 50)
                | (7u64 << 51)
                | ((offset_mode as u64) << 54)
                | ((insn.bits(87, 2) as u64) << 56),
        );
    }
    scatter_250(ctx, mask.count_ones(), second_dest);
    StepAction::Continue
}

fn lower_tex(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let opcode = ctx.opcode;
    let bindless = opcode != 0xb60;
    let handle = if opcode == 0xf60 {
        ctx.uniform_register(insn.bits(40, 6))
    } else {
        255
    };
    if bindless
        && ((opcode == 0xf60
            && (handle == u32::MAX || handle >= 246 || insn.bits(46, 13) != 0))
            || (opcode == 0x361 && (insn.bits(40, 19) != 0 || insn.bits(87, 3) > 3)))
    {
        return fail(ctx, "native bindless texture mode");
    }
    let mask = insn.bits(72, 4);
    let second_dest = insn.bits(64, 8);
    let split = second_dest != 255;
    let depth_compare = insn.bits(78, 1) != 0;
    let dimension = insn.bits(61, 3);
    let lod = insn.bits(87, 3);
    let source_a = insn.bits(24, 8);
    let source_b = insn.bits(32, 8);
    if !bindless && dimension == 5 && depth_compare && lod == 0 && !split && mask == 1 {
        if insn.bits(60, 1) == 0
            || insn.bits(76, 2) != 0
            || insn.bits(81, 3) != 7
            || insn.bits(59, 1) != 0
            || insn.bits(40, 14) > 0x1fff
            || insn.bits(54, 5) != 2
            || (source_a & 1) != 0
            || (source_b & 1) != 0
            || source_a >= 248
            || source_b >= 248
            || ctx.dest >= 248
        {
            return fail(ctx, "native array depth comparison sample mode");
        }
        if !ctx.bind_texture_buffer(2) {
            return fail(ctx, "native texture descriptor buffer conflict");
        }
        for i in 0..3u32 {
            let source = if i < 2 { source_a + i } else { source_b };
            emit_mov(ctx, 250 + i, source);
        }
        ctx.emit(
            0xc000_0000_0000_0000u64
                | ctx.predicate
                | ctx.dest
                | (250u64 << 8)
                | (((source_b + 1) as u64) << 20)
                | (3u64 << 28)
                | (1u64 << 31)
                | ((insn.bits(40, 14) as u64) << 36)
                | ((insn.bits(90, 1) as u64) << 49)
                | (1u64 << 50)
                | (7u64 << 51),
        );
        return StepAction::Continue;
    }

    let swizzle = find_swizzle(mask, split);
    if bindless
        || insn.bits(60, 1) == 0
        || (dimension != 1 && dimension != 5)
        || swizzle.is_none()
        || insn.bits(76, 2) != 0
        || insn.bits(81, 3) != 7
        || (lod > 1 && (lod != 3 || dimension != 1 || depth_compare))
        || (lod == 3 && ((source_a & 1) != 0 || source_a >= 249))
        || insn.bits(59, 1) != 0
        || insn.bits(40, 14) > 0x1fff
        || insn.bits(54, 5) != 2
        || ctx.dest == 255
        || ((mask & (mask - 1)) != 0 && ((ctx.dest & 1) != 0 || ctx.dest >= 254))
        || (split && mask == 15 && ((second_dest & 1) != 0 || second_dest >= 254))
        || (depth_compare
            && (dimension != 1 || split || mask != 1 || (source_a & 1) != 0 || source_a >= 249))
        || (dimension == 5 && ((source_a & 1) != 0 || source_a >= 249))
        || !ctx.bind_texture_buffer(2)
    {
        if !lower_general_tex(
            ctx,
            bindless,
            handle,
            mask,
            second_dest,
            split,
            depth_compare,
            dimension,
            lod,
            source_a,
            source_b,
        ) {
            return fail(ctx, "native texture mode or descriptor binding");
        }
        return StepAction::Continue;
    }

    let swizzle = swizzle.unwrap_or(0);
    let texs_mode = if dimension == 5 {
        if lod != 0 {
            8u64
        } else {
            7u64
        }
    } else if depth_compare {
        if lod != 0 {
            6u64
        } else {
            4u64
        }
    } else if lod == 3 {
        3u64
    } else if lod != 0 {
        2u64
    } else {
        1u64
    };
    ctx.emit(
        (0xd800_0000_0000_0000u64 & !((insn.bits(79, 1) as u64) << 59))
            | ctx.predicate
            | ctx.dest
            | ((insn.bits(24, 8) as u64) << 8)
            | ((insn.bits(32, 8) as u64) << 20)
            | ((insn.bits(64, 8) as u64) << 28)
            | ((insn.bits(40, 14) as u64) << 36)
            | ((insn.bits(90, 1) as u64) << 49)
            | ((swizzle as u64) << 50)
            | (texs_mode << 53),
    );
    StepAction::Continue
}

#[allow(clippy::too_many_arguments)]
fn lower_general_tex(
    ctx: &mut LowerCtx<'_>,
    bindless: bool,
    handle: u32,
    mask: u32,
    second_dest: u32,
    split: bool,
    depth_compare: bool,
    dimension: u32,
    lod: u32,
    source_a: u32,
    source_b: u32,
) -> bool {
    const COORDINATE_COUNT: [u32; 8] = [1, 2, 3, 3, 2, 3, 0, 4];
    const MAXWELL_TYPE: [u32; 8] = [0, 2, 4, 6, 1, 3, 0, 7];
    let insn = ctx.insn;
    let opcode = ctx.opcode;
    let offsets = insn.bits(76, 1) != 0;
    if dimension == 6
        || insn.bits(77, 1) != 0
        || insn.bits(81, 3) != 7
        || insn.bits(59, 1) != if opcode == 0x361 { 1 } else { 0 }
        || (!bindless && (insn.bits(40, 14) > 0x1fff || insn.bits(54, 5) >= 18))
        || (lod > 3 && lod != 6 && lod != 7)
        || (depth_compare && mask != 1)
        || mask == 0
    {
        return false;
    }
    let coordinates = COORDINATE_COUNT[dimension as usize];
    let has_lod = matches!(lod, 2 | 3 | 6 | 7);
    let handle_slot = if opcode == 0x361 { 1 } else { 0 };
    let count = coordinates
        + handle_slot
        + if has_lod { 1 } else { 0 }
        + if offsets { 1 } else { 0 }
        + if depth_compare { 1 } else { 0 };
    let in_a = if opcode == 0x361 && insn.bits(60, 1) == 0 {
        coordinates
    } else if count <= 4 {
        count.div_ceil(2)
    } else if opcode == 0x361 {
        coordinates
    } else {
        4
    };
    let value = |i: u32| -> u32 {
        if i < in_a {
            if source_a == 255 {
                255
            } else {
                source_a + i
            }
        } else if source_b == 255 {
            255
        } else {
            source_b + (i - in_a)
        }
    };
    if (source_a != 255 && source_a + in_a > 246)
        || (count > in_a && source_b != 255 && source_b + (count - in_a) > 246)
    {
        return false;
    }
    let texture_handle = if opcode == 0x361 {
        value(coordinates)
    } else {
        handle
    };
    if bindless && texture_handle >= 246 {
        return false;
    }
    let binding = insn.bits(54, 5);
    let second_buffer = !bindless && !ctx.bind_texture_buffer(binding);
    let tex_b = bindless || second_buffer;
    for i in 0..coordinates {
        emit_mov(ctx, 250 + i, value(i));
    }
    let mut meta = 246u32;
    let mut next = coordinates + handle_slot;
    if bindless {
        emit_mov(ctx, meta, texture_handle);
        meta += 1;
    } else if second_buffer {
        ctx.emit(
            0x4c98_0000_0007_0000u64
                | (meta as u64)
                | (15u64 << 39)
                | ((insn.bits(40, 14) as u64) << 20)
                | ((binding as u64) << 34),
        );
        meta += 1;
    }
    if has_lod {
        emit_mov(ctx, meta, value(next));
        meta += 1;
        next += 1;
    }
    if offsets {
        emit_mov(ctx, meta, value(next));
        meta += 1;
        next += 1;
    }
    if depth_compare {
        emit_mov(ctx, meta, value(next));
        meta += 1;
    }
    if tex_b {
        ctx.emit(
            0xde80_0000_0000_0000u64
                | ctx.predicate
                | 250
                | (250u64 << 8)
                | (246u64 << 20)
                | ((MAXWELL_TYPE[dimension as usize] as u64) << 28)
                | ((mask as u64) << 31)
                | ((offsets as u64) << 36)
                | ((lod as u64) << 37)
                | ((insn.bits(90, 1) as u64) << 49)
                | ((depth_compare as u64) << 50)
                | (7u64 << 51),
        );
    } else {
        ctx.emit(
            0xc000_0000_0000_0000u64
                | ctx.predicate
                | 250
                | (250u64 << 8)
                | ((if meta == 246 { 255u64 } else { 246u64 }) << 20)
                | ((MAXWELL_TYPE[dimension as usize] as u64) << 28)
                | ((mask as u64) << 31)
                | ((insn.bits(40, 14) as u64) << 36)
                | ((insn.bits(90, 1) as u64) << 49)
                | ((depth_compare as u64) << 50)
                | (7u64 << 51)
                | ((offsets as u64) << 54)
                | ((lod as u64) << 55),
        );
    }
    let components = mask.count_ones();
    if insn.bits(79, 1) != 0 {
        let pack = |ctx: &mut LowerCtx<'_>, target: u32, low: u32, high: u32| {
            if target == 255 {
                return;
            }
            ctx.emit(
                0x5ca8_0000_0000_0000u64
                    | ctx.predicate
                    | 246
                    | (1u64 << 8)
                    | (2u64 << 10)
                    | ((low as u64) << 20),
            );
            ctx.emit(
                0x5ca8_0000_0000_0000u64
                    | ctx.predicate
                    | 247
                    | (1u64 << 8)
                    | (2u64 << 10)
                    | ((high as u64) << 20),
            );
            ctx.emit(
                0x5c18_0000_0000_0000u64
                    | ctx.predicate
                    | (target as u64)
                    | (247u64 << 8)
                    | (246u64 << 20)
                    | (16u64 << 39),
            );
        };
        pack(
            ctx,
            ctx.dest as u32,
            250,
            if components > 1 { 251 } else { 255 },
        );
        if components > 2 {
            let second_target = if split {
                second_dest
            } else if ctx.dest == 255 {
                255
            } else {
                (ctx.dest as u32) + 1
            };
            pack(
                ctx,
                second_target,
                252,
                if components > 3 { 253 } else { 255 },
            );
        }
        return true;
    }
    scatter_250(ctx, components, second_dest);
    true
}

fn lower_attr(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let opcode = ctx.opcode;
    if insn.bits(76, 2) != 0 || insn.bits(79, 1) != 0 {
        return fail(ctx, "native indexed/patch attribute access");
    }
    let address = insn.bits(40, 10);
    let components = insn.bits(74, 2) + 1;
    if !address.is_multiple_of(4) || address + components * 4 > 0x400 || insn.bits(24, 8) != 255 {
        return fail(ctx, "native attribute address or indexing");
    }
    if opcode == 0x322 && address == 0x5c && components == 1 {
        return StepAction::Continue;
    }
    let mut value_reg = if opcode == 0x321 {
        ctx.dest as u32
    } else {
        insn.bits(32, 8)
    };
    if value_reg == 255 {
        if opcode == 0x321 {
            return StepAction::Continue;
        }
        for i in 0..components {
            ctx.mov_imm(250 + i, 0);
        }
        value_reg = 250;
    }
    if value_reg + components > 255 {
        return fail(ctx, "native attribute register range");
    }
    let vertex = if opcode == 0x321 {
        insn.bits(32, 8)
    } else {
        insn.bits(64, 8)
    };
    let translated = (if opcode == 0x321 {
        0xefd8_0000_0000_0000u64
    } else {
        0xeff0_0000_0000_0000u64
    }) | ctx.predicate
        | (value_reg as u64)
        | (255u64 << 8)
        | ((address as u64) << 20)
        | ((vertex as u64) << 39)
        | (((components - 1) as u64) << 47);
    ctx.emit(translated);
    StepAction::Continue
}

fn lower_local_spill(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let store = ctx.opcode == 0x387;
    let size = insn.bits(73, 3);
    let data = insn.bits(32, 8);
    let offset = insn.bits(40, 24);
    let address = insn.bits(24, 8);
    let words = if size == 6 {
        4u32
    } else if size == 5 {
        2u32
    } else {
        1u32
    };
    let value = if store { data } else { ctx.dest as u32 };
    let alignment = if size < 2 {
        1u32
    } else if size < 4 {
        2u32
    } else {
        4u32
    };
    if (address != 255 && address >= 246)
        || (store && insn.bits(16, 8) != 0)
        || (!store && data != 0)
        || (value != 255 && (value + words > 248 || (value & (words - 1)) != 0))
        || (!store && value == 255)
        || (offset & (alignment - 1)) != 0
        || offset + 4 * words > 0x80_0000
        || size > 6
        || (insn.words[2] & !if store { 0xe00u32 } else { 0x20_0e00u32 }) != 0x0010_0000u32
    {
        return fail(ctx, "native local scalar spill mode");
    }
    for word in 0..words {
        ctx.emit(
            (if store {
                0xef50_0000_0000_0000u64
            } else {
                0xef40_0000_0000_0000u64
            }) | ctx.predicate
                | (if value == 255 { 255 } else { value + word } as u64)
                | ((address as u64) << 8)
                | (((offset + 4 * word) as u64) << 20)
                | ((if size < 4 { size } else { 4 } as u64) << 48),
        );
    }
    StepAction::Continue
}

fn lower_ipa_indexed(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let index_register = ctx.uniform_register(insn.bits(24, 8));
    if index_register == u32::MAX
        || insn.bits(32, 8) != 255
        || (insn.bits(76, 4) != 0 && insn.bits(76, 4) != 4)
        || insn.bits(81, 3) != 7
    {
        return fail(ctx, "native indexed IPA mode");
    }
    if index_register == 255 {
        ctx.emit(
            ctx.base(0xe000_0000_0000_0000u64)
                | (255u64 << 8)
                | (255u64 << 20)
                | ((insn.bits(64, 8) as u64) << 30)
                | ((insn.bits(78, 1) as u64) << 55),
        );
        return StepAction::Continue;
    }
    ctx.emit(
        0x1c00_0000_0000_0000u64
            | ctx.predicate
            | 250
            | ((index_register as u64) << 8)
            | (((insn.bits(64, 8) * 4) as u64) << 20),
    );
    ctx.emit(
        ctx.base(0xe000_0000_0000_0000u64)
            | (250u64 << 8)
            | (255u64 << 20)
            | ((insn.bits(64, 8) as u64) << 30)
            | (1u64 << 38)
            | ((insn.bits(78, 1) as u64) << 55),
    );
    StepAction::Continue
}

fn lower_ipa(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    if insn.bits(32, 8) != 255
        || insn.bits(76, 2) > 1
        || insn.bits(79, 1) != 0
        || insn.bits(81, 3) != 7
    {
        return fail(ctx, "native IPA interpolation mode");
    }
    ctx.emit(
        ctx.base(0xe000_0000_0000_0000u64)
            | (255u64 << 8)
            | (255u64 << 20)
            | ((insn.bits(64, 8) as u64) << 30)
            | ((insn.bits(76, 2) as u64) << 52)
            | ((insn.bits(78, 1) as u64) << 55),
    );
    StepAction::Continue
}

fn lower_stage_buffer(ctx: &mut LowerCtx<'_>) -> StepAction {
    let insn = ctx.insn;
    let opcode = ctx.opcode;
    if ctx.options.geometry {
        if opcode != 0x923 || insn.bits(75, 5) != 0 || insn.bits(40, 24) != 0 {
            return fail(ctx, "native geometry ISBERD mode");
        }
        if ctx.dest != 255 {
            ctx.emit(
                ctx.base(0x5c98_0000_0000_0000u64)
                    | (15u64 << 39)
                    | ((insn.bits(24, 8) as u64) << 20),
            );
        }
        return StepAction::Continue;
    }
    let write = opcode == 0x927;
    let region = insn.bits(76, 2);
    let output = insn.bits(79, 1) != 0;
    if (insn.words[2] & !0xf800u32) != 0
        || (region != 0 && region != 3)
        || write != output
        || (!write && region != 3)
        || (!write && ctx.dest == 255)
    {
        return fail(ctx, "native stage buffer access mode");
    }
    ctx.emit(
        0xefc0_0000_0000_0000u64
            | ctx.predicate
            | if write {
                insn.bits(32, 8) as u64
            } else {
                ctx.dest
            }
            | ((insn.bits(24, 8) as u64) << 8)
            | ((insn.bits(40, 24) as u64) << 20)
            | ((insn.bits(75, 1) as u64) << 44)
            | (((region == 3) as u64) << 45)
            | ((insn.bits(78, 1) as u64) << 46)
            | ((output as u64) << 47)
            | ((write as u64) << 48),
    );
    StepAction::Continue
}

pub(super) fn try_lower(ctx: &mut LowerCtx<'_>) -> Option<StepAction> {
    let action = match ctx.opcode {
        0xab9 | 0xabb => lower_uldc(ctx),
        0xb82 => lower_ldc(ctx),
        0xb6c | 0xf6c => lower_txd(ctx),
        0x98e => lower_red(ctx),
        0xf8c => lower_atoms_popc(ctx),
        0x38c => lower_atoms(ctx),
        0x3a8 => lower_atomg_reg(ctx),
        0x9a8 => lower_atomg_uniform(ctx),
        0xb97 => lower_suld(ctx),
        0xb9b => lower_sust(ctx),
        0x593 => lower_suatom(ctx),
        0x59f => lower_sured(ctx),
        0x388 | 0x984 | 0x988 => lower_shared_mem(ctx),
        0x381 | 0x981 => lower_ldg(ctx),
        0x386 | 0x986 => lower_stg(ctx),
        0xb6f | 0x370 => lower_txq(ctx),
        0xb66 => lower_tld_scr(ctx),
        0x367 => lower_tld_b(ctx),
        0xf66 => lower_tld_uniform(ctx),
        0xfae => lower_ldgsts(ctx),
        0x9af => lower_ldgdepbar(ctx),
        0xb69 | 0xf69 | 0x36a => lower_tmml(ctx),
        0xb63 | 0xf63 => lower_tld4(ctx),
        0xb60 | 0xf60 | 0x361 => lower_tex(ctx),
        0x321 | 0x322 => lower_attr(ctx),
        0x387 | 0x983 => lower_local_spill(ctx),
        0x526 => lower_ipa_indexed(ctx),
        0x326 => lower_ipa(ctx),
        0x923 | 0x927 => lower_stage_buffer(ctx),
        _ => return None,
    };
    Some(action)
}
