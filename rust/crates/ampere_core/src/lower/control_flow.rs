// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use crate::types::Instruction;

use super::{LowerCtx, Relocation, StepAction};

pub(super) fn try_lower(ctx: &mut LowerCtx<'_>) -> Option<StepAction> {
    match ctx.opcode {
        0x95b => {
            // Fragment discard; execution predicate is preserved.
            if ctx.insn.words[1] != 0 || ctx.insn.words[2] != 0x0380_0000 {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native fragment discard mode".to_owned(),
                ));
            }
            ctx.emit(0xe330_0000_0000_000fu64 | ctx.predicate);
            Some(StepAction::Continue)
        }
        0x949 | 0x958 => {
            // Captured BRX / BRXU with absolute native-PC table values
            let expected_w2 = if ctx.opcode == 0x958 {
                0x0b80_0000u32
            } else {
                0x0380_0000u32
            };
            if ctx.insn.bits(12, 4) != 7 || (ctx.insn.words[2] & !0x3_ffffu32) != expected_w2 {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native computed branch mode".to_owned(),
                ));
            }
            let raw =
                (ctx.insn.words[1] as u64) | (((ctx.insn.words[2] & 0x3_ffffu32) as u64) << 32);
            let relative = ((raw << 14) as i64) >> 14;
            if (ctx.index as i64) * 16 + 16 + relative != 0 {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native computed branch relative base".to_owned(),
                ));
            }
            let offset =
                -(LowerCtx::address_of(ctx.result.instructions.len() as u32) as i64) - 8;
            if offset < -0x80_0000 {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native computed branch range".to_owned(),
                ));
            }
            let source = if ctx.opcode == 0x958 {
                ctx.uniform_register(ctx.insn.bits(24, 8))
            } else {
                ctx.insn.bits(24, 8)
            };
            if source == u32::MAX {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native uniform computed branch source".to_owned(),
                ));
            }
            ctx.emit(
                0xe250_0000_0000_000fu64
                    | ctx.predicate
                    | ((source as u64) << 8)
                    | (((offset as u64) & 0xff_ffffu64) << 20),
            );
            ctx.computed_branch = true;
            Some(StepAction::Continue)
        }
        0x944 => {
            // Verified CALL.REL.NOINC with R55 holding the return PC
            if ctx.native_predicate != 7
                && ctx.insn.words[1] == 16
                && ctx.insn.words[2] == 0x03c0_0000
                && ((ctx.index + 2) as usize) < ctx.code.len()
                && ctx.code[(ctx.index + 1) as usize].bits(0, 12) == 0x947
            {
                ctx.relocations.push(Relocation::new(
                    ctx.result.instructions.len() as u32,
                    (ctx.index + 2) as i64,
                ));
                ctx.emit(0xe240_0000_0000_000fu64 | ctx.predicate);
                return Some(StepAction::Continue);
            }
            let mut link = u32::MAX;
            let mut written = [false; 256];
            let mut prior = ctx.index;
            while prior > 0 {
                prior -= 1;
                if ctx.index - prior > 64 {
                    break;
                }
                let candidate = ctx.code[prior as usize];
                let prior_opcode = candidate.bits(0, 12);
                if (candidate.words[0] & !0x00ff_0000u32) == 0x0000_7802
                    && candidate.words[1] == (ctx.index + 1) * 16
                    && candidate.words[2] == 0xf00
                {
                    link = candidate.bits(16, 8);
                    break;
                }
                if matches!(
                    prior_opcode,
                    0x947 | 0x949 | 0x958 | 0x944 | 0x950 | 0x94d | 0x945 | 0x941 | 0x942
                ) {
                    break;
                }
                let written_reg = candidate.bits(16, 8);
                written[written_reg as usize] = true;
                if matches!(prior_opcode, 0x225 | 0x625 | 0x825) {
                    written[((written_reg + 1) & 0xff) as usize] = true;
                }
            }
            if ctx.main_exited
                || ctx.insn.words[0] != 0x7944
                || (ctx.insn.words[2] & !0x3_ffffu32) != 0x03c0_0000
                || link >= 250
                || written[link as usize]
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native call linkage mode".to_owned(),
                ));
            }
            let raw =
                (ctx.insn.words[1] as u64) | (((ctx.insn.words[2] & 0x3_ffffu32) as u64) << 32);
            let relative = ((raw << 14) as i64) >> 14;
            let target = (ctx.index as i64) * 16 + 16 + relative;
            if target < 0 || target % 16 != 0 {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native call destination alignment".to_owned(),
                ));
            }
            let target_index = target / 16;
            for call in 0..ctx.call_targets.len() {
                if (ctx.call_targets[call] as i64) == target_index && ctx.call_links[call] != link {
                    return Some(StepAction::Fail(
                        ctx.index,
                        "native inconsistent call linkage".to_owned(),
                    ));
                }
            }
            ctx.call_targets.push(target_index as u32);
            ctx.call_links.push(link);
            ctx.relocations.push(Relocation::new_call(
                ctx.result.instructions.len() as u32,
                target_index,
            ));
            ctx.emit(0xe260_0000_0007_000fu64);
            Some(StepAction::Continue)
        }
        0x950 => {
            // Verified RET.REL.NODEC epilogue restoring the call link
            let mut active_link: Option<u32> = None;
            let mut active_entry = 0u32;
            if ctx.main_exited {
                for call in 0..ctx.call_targets.len() {
                    if ctx.call_targets[call] <= ctx.index
                        && ctx.call_targets[call] >= active_entry
                    {
                        active_entry = ctx.call_targets[call];
                        active_link = Some(ctx.call_links[call]);
                    }
                }
            }
            let return_reg = ctx.insn.bits(24, 8);
            let mut linked = active_link == Some(return_reg);
            if let Some(link_reg) = active_link {
                if !linked {
                    let mut prior = ctx.index;
                    while prior > active_entry {
                        prior -= 1;
                        let prior_insn = ctx.code[prior as usize];
                        let copies_link = prior_insn.bits(16, 8) == return_reg
                            && prior_insn.bits(12, 4) == 7
                            && (((prior_insn.words[0] & 0xff00_ffffu32) == 0x0000_7202u32
                                && prior_insn.words[1] == link_reg
                                && prior_insn.words[2] == 0xf00)
                                || ((prior_insn.words[0] & 0xff00_ffffu32) == 0xff00_7224u32
                                    && prior_insn.words[1] == 0xff
                                    && prior_insn.words[2] == (0x078e_0000u32 | link_reg)));
                        if copies_link {
                            linked = true;
                            break;
                        } else if prior_insn.bits(16, 8) == return_reg {
                            break;
                        }
                    }
                }
            }
            if !ctx.main_exited
                || !linked
                || (ctx.insn.words[0] & 0x00ff_ffffu32) != 0x0000_7950
                || return_reg >= 249
                || (ctx.insn.words[2] & !0x3_ffffu32) != 0x03c0_0000
                || !ctx.barrier_tokens.is_empty()
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native return linkage mode".to_owned(),
                ));
            }
            let raw =
                (ctx.insn.words[1] as u64) | (((ctx.insn.words[2] & 0x3_ffffu32) as u64) << 32);
            let relative = ((raw << 14) as i64) >> 14;
            if (ctx.index as i64) * 16 + 16 + relative != 0 {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native return relative base".to_owned(),
                ));
            }
            ctx.emit(0xe320_0000_0007_000fu64);
            let mut complete = !ctx.computed_branch;
            for &target in &ctx.call_targets {
                complete &= target <= ctx.index;
            }
            if complete {
                return Some(StepAction::Finish(ctx.index));
            }
            Some(StepAction::Continue)
        }
        0x945 | 0x941 | 0x942 | 0x948 | 0x348 => Some(StepAction::Continue),
        0x947 => {
            // BRA
            let mode = ctx.insn.bits(32, 2);
            if mode == 0 && ctx.insn.bits(91, 1) != 0 {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native branch convergence mode".to_owned(),
                ));
            }
            if mode == 2 {
                return Some(StepAction::Continue);
            }
            let mut branch_predicate = ctx.predicate;
            let extra_pred = ctx.insn.bits(87, 4);
            if ctx.opcode == 0x947 && extra_pred != 7 {
                let insn_pred = ctx.insn.bits(12, 4);
                if insn_pred != 7 {
                    ctx.relocations.push(Relocation::new(
                        ctx.result.instructions.len() as u32,
                        (ctx.index + 1) as i64,
                    ));
                    ctx.emit(0xe240_0000_0000_000fu64 | (((insn_pred ^ 8) as u64) << 16));
                }
                branch_predicate = (extra_pred as u64) << 16;
            } else if extra_pred != 7 {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native extra branch condition".to_owned(),
                ));
            }
            let raw = ((ctx.insn.words[1] & !3u32) as u64)
                | (((ctx.insn.words[2] & 0x3_ffffu32) as u64) << 32);
            let relative = ((raw << 14) as i64) >> 14;
            let target = (ctx.index as i64) * 16 + 16 + relative;
            if target < 0 || target % 16 != 0 {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native branch destination alignment".to_owned(),
                ));
            }
            ctx.relocations.push(Relocation::new(
                ctx.result.instructions.len() as u32,
                target / 16,
            ));
            ctx.emit(0xe240_0000_0000_000fu64 | branch_predicate);
            if ctx.computed_branch
                && target == (ctx.index as i64) * 16
                && branch_predicate == (7u64 << 16)
            {
                return Some(StepAction::Finish(ctx.index));
            }
            Some(StepAction::Continue)
        }
        0x324 | 0x924 => {
            // OUT: geometry output (register or immediate stream).
            if !ctx.options.geometry {
                return Some(StepAction::Fail(
                    ctx.index,
                    "unsupported native opcode".to_owned(),
                ));
            }
            let out_type = ctx.insn.bits(78, 2);
            if ctx.insn.bits(80, 12) != 0 || ctx.insn.bits(64, 14) != 0 {
                return Some(StepAction::Fail(ctx.index, "native OUT mode".to_owned()));
            }
            let a = ctx.insn.bits(24, 8);
            if out_type == 0 {
                if ctx.dest != 255 {
                    ctx.emit(
                        ctx.base(0x5c98_0000_0000_0000u64) | (15u64 << 39) | ((a as u64) << 20),
                    );
                }
                return Some(StepAction::Continue);
            }
            if ctx.opcode == 0x324 {
                ctx.emit(
                    ctx.base(0xfbe0_0000_0000_0000u64)
                        | ((a as u64) << 8)
                        | ((ctx.insn.bits(32, 8) as u64) << 20)
                        | ((out_type as u64) << 39),
                );
            } else {
                if ctx.insn.words[1] >= (1u32 << 19) {
                    return Some(StepAction::Fail(
                        ctx.index,
                        "native OUT stream immediate".to_owned(),
                    ));
                }
                ctx.emit(
                    ctx.base(0xf6e0_0000_0000_0000u64)
                        | ((a as u64) << 8)
                        | ((ctx.insn.words[1] as u64) << 20)
                        | ((out_type as u64) << 39),
                );
            }
            Some(StepAction::Continue)
        }
        0x34e => {
            // LEPC: load this native instruction's address as a register pair.
            if !ctx.options.geometry {
                return Some(StepAction::Fail(
                    ctx.index,
                    "unsupported native opcode".to_owned(),
                ));
            }
            if ctx.insn.bits(24, 8) != 0
                || ctx.insn.words[1] != 0
                || ctx.insn.words[2] != 0
                || ctx.dest == 254
                || ctx.native_predicate != 7
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native program counter load mode".to_owned(),
                ));
            }
            if ctx.dest == 255 {
                return Some(StepAction::Continue);
            }
            let address = ctx
                .options
                .native_code_address
                .wrapping_add((ctx.index as u64) * (std::mem::size_of::<Instruction>() as u64));
            ctx.mov_imm(ctx.dest as u32, address as u32);
            ctx.mov_imm((ctx.dest as u32) + 1, (address >> 32) as u32);
            Some(StepAction::Continue)
        }
        0x357 => {
            // Set the native geometry exit stub; host geometry invocations need no stub.
            if !ctx.options.geometry {
                return Some(StepAction::Fail(
                    ctx.index,
                    "unsupported native opcode".to_owned(),
                ));
            }
            if ctx.insn.bits(16, 8) != 0
                || ctx.insn.words[1] != 0
                || ctx.insn.words[2] != 0
                || ctx.native_predicate != 7
            {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native geometry exit setup mode".to_owned(),
                ));
            }
            Some(StepAction::Continue)
        }
        0x957 => {
            // Finish the geometry exit stub; the shader's own EXIT follows.
            if !ctx.options.geometry {
                return Some(StepAction::Fail(
                    ctx.index,
                    "unsupported native opcode".to_owned(),
                ));
            }
            ctx.emit(0x50b0_0000_0000_0f00u64 | ctx.predicate);
            Some(StepAction::Continue)
        }
        0x94d => {
            // EXIT
            if ctx.insn.bits(84, 2) != 0 {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native conditional/special EXIT".to_owned(),
                ));
            }
            let exit_pred = ctx.insn.bits(87, 4);
            if exit_pred != 7 {
                if ctx.native_predicate != 7 {
                    ctx.relocations.push(Relocation::new(
                        ctx.result.instructions.len() as u32,
                        (ctx.index + 1) as i64,
                    ));
                    ctx.emit(0xe240_0000_0000_000fu64 | (((ctx.native_predicate ^ 8) as u64) << 16));
                }
                ctx.emit(0xe300_0000_0000_000fu64 | ((exit_pred as u64) << 16));
                return Some(StepAction::Continue);
            }
            ctx.emit(0xe300_0000_0000_000fu64 | ctx.predicate);
            if ctx.insn.bits(12, 4) != 7 {
                return Some(StepAction::Continue);
            }
            if !ctx.barrier_tokens.is_empty() {
                return Some(StepAction::Fail(
                    ctx.index,
                    "native unclosed reconvergence scope".to_owned(),
                ));
            }
            if ctx.call_targets.is_empty() {
                let pending_branch = ctx
                    .relocations
                    .iter()
                    .any(|relocation| relocation.target > (ctx.index as i64));
                if !pending_branch && !ctx.computed_branch {
                    return Some(StepAction::Finish(ctx.index));
                }
                return Some(StepAction::Continue);
            }
            let pending_main_branch = ctx
                .relocations
                .iter()
                .any(|relocation| !relocation.call && relocation.target > (ctx.index as i64));
            if !ctx.main_exited && pending_main_branch {
                return Some(StepAction::Continue);
            }
            if ctx.main_exited {
                let mut complete = !ctx
                    .relocations
                    .iter()
                    .any(|relocation| relocation.target > (ctx.index as i64));
                for &target in &ctx.call_targets {
                    complete &= target <= ctx.index;
                }
                if complete && !ctx.computed_branch {
                    return Some(StepAction::Finish(ctx.index));
                }
                return Some(StepAction::Continue);
            }
            for &target in &ctx.call_targets {
                if target <= ctx.index {
                    return Some(StepAction::Fail(
                        ctx.index,
                        "native call target overlaps main program".to_owned(),
                    ));
                }
            }
            let pending_branch = ctx.relocations.iter().any(|relocation| {
                relocation.target > (ctx.index as i64)
                    && !ctx.call_targets.contains(&(relocation.target as u32))
            });
            if !pending_branch {
                ctx.main_exited = true;
            }
            Some(StepAction::Continue)
        }
        _ => None,
    }
}
