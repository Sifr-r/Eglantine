// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::collections::{BTreeMap, BTreeSet, VecDeque};

use eglantine_ampere_rs::Instruction;

use crate::coop_matrix::CoopConfig;
use crate::decode_sm86::{
    decode_program, AtomicOp, BoolComb, DecodedInsn, DecodedOp, FloatCmp, FloatSrc, HalfSrc,
    IntCmp, MemBarrierScope, MemWidth, MufuOp, PredGuard, S2rSpecial, ShflMode, SrcOperand,
    VoteMode,
};
use crate::reflect::{
    Bindings, FallbackReason, Profile, ShaderInfo, SnapshotEnv, Stage, SPIRV_1_6_VERSION,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ValueId(pub u32);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ValType {
    Void,
    Bool,
    U32,
    F32,
    U64,
    F16x2,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GlslOp {
    Sin,
    Cos,
    Exp2,
    Log2,
    Sqrt,
    InverseSqrt,
    FMin,
    FMax,
    SMin,
    SMax,
    UMin,
    UMax,
    SAbs,
    FindILsb,
    FindSMsb,
    FindUMsb,
    UnpackHalf2x16,
    PackHalf2x16,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SsaOp {
    ConstU32(u32),
    ConstBool(bool),
    ConstU64(u64),
    Undef,
    ReadBuiltin(S2rSpecial),
    ReadCbuf { bank: u32, word_offset: u32 },
    ReadCbufDynamic { bank: u32, byte_offset: ValueId },
    Copy(ValueId),
    Phi(Vec<(usize, ValueId)>),
    Select { cond: ValueId, t: ValueId, f: ValueId },
    Bitcast { to: ValType, val: ValueId },
    IAdd(ValueId, ValueId),
    ISub(ValueId, ValueId),
    IMul(ValueId, ValueId),
    UMulHi(ValueId, ValueId),
    SMulHi(ValueId, ValueId),
    BitAnd(ValueId, ValueId),
    BitOr(ValueId, ValueId),
    BitXor(ValueId, ValueId),
    BitNot(ValueId),
    Shl(ValueId, ValueId),
    LShr(ValueId, ValueId),
    AShr(ValueId, ValueId),
    INeg(ValueId),
    BitCount(ValueId),
    BitReverse(ValueId),
    SBitFieldExtract { val: ValueId, offset: u32, count: u32 },
    UBitFieldExtract { val: ValueId, offset: u32, count: u32 },
    IntCmp { cmp: IntCmp, signed: bool, a: ValueId, b: ValueId },
    BoolAnd(ValueId, ValueId),
    BoolOr(ValueId, ValueId),
    BoolXor(ValueId, ValueId),
    BoolNot(ValueId),
    FAdd(ValueId, ValueId),
    FSub(ValueId, ValueId),
    FMul(ValueId, ValueId),
    FDiv(ValueId, ValueId),
    FNeg(ValueId),
    FAbs(ValueId),
    FSet { cmp: FloatCmp, a: ValueId, b: ValueId },
    Glsl1(GlslOp, ValueId),
    Glsl2(GlslOp, ValueId, ValueId),
    SConvertToF32(ValueId),
    UConvertToF32(ValueId),
    F32ConvertToS(ValueId),
    F32ConvertToU(ValueId),
    FConvertF32ToF16Lo(ValueId),
    FConvertF16LoToF32(ValueId),
    HAdd2 { a: ValueId, b: ValueId },
    HMul2 { a: ValueId, b: ValueId },
    HFma2 { a: ValueId, b: ValueId, c: ValueId },
    LoadSsboWord { ssbo_idx: usize, word_idx: ValueId },
    StoreSsboWord { ssbo_idx: usize, word_idx: ValueId, val: ValueId },
    AtomicSsboWord { ssbo_idx: usize, word_idx: ValueId, op: AtomicOp, data: ValueId },
    LoadSharedWord { word_idx: ValueId },
    StoreSharedWord { word_idx: ValueId, val: ValueId },
    AtomicSharedWord { word_idx: ValueId, op: AtomicOp, data: ValueId },
    LoadLocalWord { word_idx: ValueId },
    StoreLocalWord { word_idx: ValueId, val: ValueId },
    SubgroupShuffle { mode: ShflMode, val: ValueId, lane_or_delta: ValueId },
    SubgroupVote { mode: VoteMode, pred: ValueId },
    SubgroupBallot { pred: ValueId },
    ControlBarrier,
    MemoryBarrier { scope: MemBarrierScope },
    CoopHmma {
        depth: u32,
        a: [ValueId; 4],
        a_len: usize,
        b: [ValueId; 2],
        b_len: usize,
        c: [ValueId; 2],
    },
    CoopHmmaExtract { hmma: ValueId, word_idx: usize },
}

#[derive(Clone, Debug)]
pub struct SsaInsn {
    pub id: ValueId,
    pub ty: ValType,
    pub op: SsaOp,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MergeHint {
    None,
    Selection { merge_block: usize },
    Loop { merge_block: usize, continue_block: usize },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Terminator {
    Branch { target: usize },
    BranchCond { cond: ValueId, true_target: usize, false_target: usize },
    Return,
    Unreachable,
}

#[derive(Clone, Debug)]
pub struct SsaBlock {
    pub id: usize,
    pub insns: Vec<SsaInsn>,
    pub merge: MergeHint,
    pub terminator: Terminator,
}

/// Complete lowered and optimized SSA program ready for direct SPIR-V 1.6 emission.
#[derive(Clone, Debug)]
pub struct SsaProgram {
    pub stage: Stage,
    pub workgroup_size: [u32; 3],
    pub shared_memory_bytes: u32,
    pub local_memory_bytes: u32,
    pub coop_config: CoopConfig,
    pub profile: Profile,
    pub initial_bindings: Bindings,
    pub bindings: Bindings,
    pub info: ShaderInfo,
    pub blocks: Vec<SsaBlock>,
    pub next_value_id: u32,
    pub uses_dynamic_cbuf: bool,
}

/// Compile-time metadata passed in alongside SM86 instructions.
#[derive(Clone, Debug)]
pub struct LowerInput<'a> {
    pub stage: Stage,
    pub program_key: u64,
    pub code: &'a [Instruction],
    pub workgroup_size: [u32; 3],
    pub local_memory_size: u32,
    pub shared_memory_size: u32,
    pub profile: Profile,
    pub bindings: Bindings,
    pub snapshot: SnapshotEnv,
}

#[derive(Clone, Copy, Debug)]
struct PtrOrigin {
    bank: u32,
    cbuf_offset: u32,
    byte_offset: ValueId,
}

#[derive(Clone, Debug)]
struct RegState {
    r: [ValueId; 255],
    ur: [ValueId; 63],
    p: [ValueId; 7],
    up: [ValueId; 7],
    r_ptr: [Option<PtrOrigin>; 255],
    ur_ptr: [Option<PtrOrigin>; 63],
    r_cbuf_hi: [Option<(u32, u32)>; 255],
    ur_cbuf_hi: [Option<(u32, u32)>; 63],
}

#[derive(Clone, Debug)]
enum CfgNode {
    Range { start: usize, end_excl: usize },
    If {
        cond_block_end: usize,
        cond: PredGuard,
        then_nodes: Vec<CfgNode>,
        else_nodes: Vec<CfgNode>,
    },
    Loop {
        header_idx: usize,
        body_nodes: Vec<CfgNode>,
        continue_cond: PredGuard,
        continue_negated: bool,
    },
}

struct SsaBuilder<'a> {
    stage: Stage,
    program_key: u64,
    total_sm86_insns: usize,
    decoded: Vec<DecodedInsn>,
    profile: Profile,
    coop_config: CoopConfig,
    snapshot: &'a SnapshotEnv,
    info: ShaderInfo,
    blocks: Vec<SsaBlock>,
    next_id: u32,
    workgroup_size: [u32; 3],
    shared_bytes_needed: u32,
    local_bytes_needed: u32,
    uses_dynamic_cbuf: bool,
    zero_u32: ValueId,
    one_u32: ValueId,
    true_bool: ValueId,
    false_bool: ValueId,
}

impl<'a> SsaBuilder<'a> {
    fn alloc_val(&mut self, block_idx: usize, ty: ValType, op: SsaOp) -> ValueId {
        let id = ValueId(self.next_id);
        self.next_id += 1;
        self.blocks[block_idx].insns.push(SsaInsn { id, ty, op });
        id
    }

    fn emit_void(&mut self, block_idx: usize, op: SsaOp) {
        let id = ValueId(self.next_id);
        self.next_id += 1;
        self.blocks[block_idx].insns.push(SsaInsn {
            id,
            ty: ValType::Void,
            op,
        });
    }

    fn new_block(&mut self) -> usize {
        let id = self.blocks.len();
        self.blocks.push(SsaBlock {
            id,
            insns: Vec::new(),
            merge: MergeHint::None,
            terminator: Terminator::Return,
        });
        id
    }

    fn const_u32(&mut self, blk: usize, v: u32) -> ValueId {
        if v == 0 {
            return self.zero_u32;
        }
        if v == 1 {
            return self.one_u32;
        }
        self.alloc_val(blk, ValType::U32, SsaOp::ConstU32(v))
    }

    fn read_pred(&mut self, blk: usize, state: &RegState, pred: PredGuard) -> ValueId {
        let base = if pred.id == 7 {
            self.true_bool
        } else {
            state.p[pred.id as usize]
        };
        if pred.neg {
            if base == self.true_bool {
                self.false_bool
            } else if base == self.false_bool {
                self.true_bool
            } else {
                self.alloc_val(blk, ValType::Bool, SsaOp::BoolNot(base))
            }
        } else {
            base
        }
    }

    fn write_reg(&self, state: &mut RegState, dst: u8, val: ValueId, ptr: Option<PtrOrigin>) {
        if (dst as usize) < 255 {
            state.r[dst as usize] = val;
            state.r_ptr[dst as usize] = ptr;
            state.r_cbuf_hi[dst as usize] = None;
        }
    }

    fn write_ureg(&self, state: &mut RegState, dst_ur: u8, val: ValueId, ptr: Option<PtrOrigin>) {
        if (dst_ur as usize) < 63 {
            state.ur[dst_ur as usize] = val;
            state.ur_ptr[dst_ur as usize] = ptr;
            state.ur_cbuf_hi[dst_ur as usize] = None;
        }
    }

    fn eval_src(&mut self, blk: usize, state: &RegState, src: SrcOperand) -> ValueId {
        match src {
            SrcOperand::Zero => self.zero_u32,
            SrcOperand::Reg(r) => state.r[r as usize],
            SrcOperand::UReg(ur) => state.ur[ur as usize],
            SrcOperand::Imm32(v) => self.const_u32(blk, v),
            SrcOperand::Cbuf(cbuf) => {
                let bank = u32::from(cbuf.bank);
                let offset = u32::from(cbuf.offset);
                if let Some(imm) = self.snapshot.lookup_replacement(bank, offset) {
                    self.info.record_cbuf_read(bank, offset, 4);
                    return self.const_u32(blk, imm);
                }
                self.info.record_cbuf_read(bank, offset, 4);
                self.alloc_val(
                    blk,
                    ValType::U32,
                    SsaOp::ReadCbuf {
                        bank,
                        word_offset: offset / 4,
                    },
                )
            }
        }
    }

    fn src_ptr(&self, state: &RegState, src: SrcOperand) -> Option<PtrOrigin> {
        match src {
            SrcOperand::Reg(r) => state.r_ptr[r as usize],
            SrcOperand::UReg(ur) => state.ur_ptr[ur as usize],
            _ => None,
        }
    }

    fn eval_float_src(&mut self, blk: usize, state: &RegState, src: FloatSrc) -> ValueId {
        let bits = self.eval_src(blk, state, src.op);
        let mut f = self.alloc_val(blk, ValType::F32, SsaOp::Bitcast { to: ValType::F32, val: bits });
        if src.abs {
            f = self.alloc_val(blk, ValType::F32, SsaOp::FAbs(f));
        }
        if src.neg {
            f = self.alloc_val(blk, ValType::F32, SsaOp::FNeg(f));
        }
        f
    }

    fn to_u32_bits(&mut self, blk: usize, f: ValueId) -> ValueId {
        self.alloc_val(blk, ValType::U32, SsaOp::Bitcast { to: ValType::U32, val: f })
    }

    fn eval_half_src(&mut self, blk: usize, state: &RegState, src: HalfSrc) -> ValueId {
        let mut raw = self.eval_src(blk, state, src.op);
        if src.swizzle == 2 {
            let m = self.const_u32(blk, 0xffff);
            let lo = self.alloc_val(blk, ValType::U32, SsaOp::BitAnd(raw, m));
            let c16 = self.const_u32(blk, 16);
            let hi = self.alloc_val(blk, ValType::U32, SsaOp::Shl(lo, c16));
            raw = self.alloc_val(blk, ValType::U32, SsaOp::BitOr(lo, hi));
        } else if src.swizzle == 3 {
            let c16 = self.const_u32(blk, 16);
            let lo = self.alloc_val(blk, ValType::U32, SsaOp::LShr(raw, c16));
            let m = self.const_u32(blk, 0xffff_0000);
            let hi = self.alloc_val(blk, ValType::U32, SsaOp::BitAnd(raw, m));
            raw = self.alloc_val(blk, ValType::U32, SsaOp::BitOr(lo, hi));
        }
        let mut unpacked = self.alloc_val(
            blk,
            ValType::F16x2,
            SsaOp::Bitcast {
                to: ValType::F16x2,
                val: raw,
            },
        );
        if src.abs {
            unpacked = self.alloc_val(blk, ValType::F16x2, SsaOp::FAbs(unpacked));
        }
        if src.neg {
            unpacked = self.alloc_val(blk, ValType::F16x2, SsaOp::FNeg(unpacked));
        }
        unpacked
    }

    fn saturate_f32(&mut self, blk: usize, f: ValueId) -> ValueId {
        let z = self.alloc_val(blk, ValType::F32, SsaOp::Bitcast { to: ValType::F32, val: self.zero_u32 });
        let one_bits = self.const_u32(blk, 1.0f32.to_bits());
        let one = self.alloc_val(blk, ValType::F32, SsaOp::Bitcast { to: ValType::F32, val: one_bits });
        let lo = self.alloc_val(blk, ValType::F32, SsaOp::Glsl2(GlslOp::FMax, f, z));
        self.alloc_val(blk, ValType::F32, SsaOp::Glsl2(GlslOp::FMin, lo, one))
    }

    fn combine_pred(&mut self, blk: usize, state: &RegState, base: ValueId, comb: BoolComb, accum: PredGuard) -> ValueId {
        if accum.is_always_true() && comb == BoolComb::And {
            return base;
        }
        let acc = self.read_pred(blk, state, accum);
        match comb {
            BoolComb::And => self.alloc_val(blk, ValType::Bool, SsaOp::BoolAnd(base, acc)),
            BoolComb::Or => self.alloc_val(blk, ValType::Bool, SsaOp::BoolOr(base, acc)),
            BoolComb::Xor => self.alloc_val(blk, ValType::Bool, SsaOp::BoolXor(base, acc)),
        }
    }

    fn maybe_mkw_workaround(&mut self, sm86_index: usize, blk: usize, state: &mut RegState) {
        if self.program_key == 0x5a4f_3726_590e_fb17u64
            && self.total_sm86_insns * 16 == 0x3b0
            && sm86_index == (0x2d0 / 16)
        {
            let r8 = state.r[8];
            let r9 = state.r[9];
            let r4 = state.r[4];
            let any = self.alloc_val(blk, ValType::U32, SsaOp::BitOr(r8, r9));
            let empty = self.alloc_val(
                blk,
                ValType::Bool,
                SsaOp::IntCmp {
                    cmp: IntCmp::Eq,
                    signed: false,
                    a: any,
                    b: self.zero_u32,
                },
            );
            let one_f32_bits = self.const_u32(blk, 0x3f80_0000u32);
            let mask = self.alloc_val(
                blk,
                ValType::U32,
                SsaOp::Select {
                    cond: empty,
                    t: one_f32_bits,
                    f: self.zero_u32,
                },
            );
            let fixed = self.alloc_val(blk, ValType::U32, SsaOp::BitOr(r4, mask));
            self.write_reg(state, 4, fixed, state.r_ptr[4]);
        }
    }

    fn lower_node_seq(
        &mut self,
        nodes: &[CfgNode],
        cur_blk: &mut usize,
        state: &mut RegState,
        active: &mut bool,
    ) -> Result<(), FallbackReason> {
        for node in nodes {
            if !*active {
                break;
            }
            self.lower_node(node, cur_blk, state, active)?;
        }
        Ok(())
    }

    fn lower_node(
        &mut self,
        node: &CfgNode,
        cur_blk: &mut usize,
        state: &mut RegState,
        active: &mut bool,
    ) -> Result<(), FallbackReason> {
        match node {
            CfgNode::Range { start, end_excl } => {
                for idx in *start..*end_excl {
                    if !*active {
                        break;
                    }
                    self.maybe_mkw_workaround(idx, *cur_blk, state);
                    let decoded = self.decoded[idx];
                    self.lower_single_insn(decoded, cur_blk, state, active)?;
                }
                Ok(())
            }
            CfgNode::If {
                cond_block_end,
                cond,
                then_nodes,
                else_nodes,
            } => {
                let cond_val = self.read_pred(*cur_blk, state, *cond);
                let _ = cond_block_end;
                let then_blk = self.new_block();
                let else_blk = self.new_block();
                let merge_blk = self.new_block();

                self.blocks[*cur_blk].merge = MergeHint::Selection {
                    merge_block: merge_blk,
                };
                self.blocks[*cur_blk].terminator = Terminator::BranchCond {
                    cond: cond_val,
                    true_target: then_blk,
                    false_target: else_blk,
                };

                let mut then_cur = then_blk;
                let mut then_state = state.clone();
                let mut then_active = true;
                self.lower_node_seq(then_nodes, &mut then_cur, &mut then_state, &mut then_active)?;
                if then_active {
                    self.blocks[then_cur].terminator = Terminator::Branch { target: merge_blk };
                }

                let mut else_cur = else_blk;
                let mut else_state = state.clone();
                let mut else_active = true;
                self.lower_node_seq(else_nodes, &mut else_cur, &mut else_state, &mut else_active)?;
                if else_active {
                    self.blocks[else_cur].terminator = Terminator::Branch { target: merge_blk };
                }

                self.merge_states(
                    merge_blk,
                    state,
                    then_active.then_some((then_cur, &then_state)),
                    else_active.then_some((else_cur, &else_state)),
                    active,
                );
                *cur_blk = merge_blk;
                Ok(())
            }
            CfgNode::Loop {
                header_idx,
                body_nodes,
                continue_cond,
                continue_negated,
            } => {
                let modified = collect_modified_regs(body_nodes, &self.decoded);
                let preheader = *cur_blk;
                let header_blk = self.new_block();
                let body_entry = self.new_block();
                let continue_blk = self.new_block();
                let merge_blk = self.new_block();

                self.blocks[preheader].terminator = Terminator::Branch { target: header_blk };

                let mut r_phis = Vec::new();
                for &reg in &modified.r {
                    let phi = self.alloc_val(header_blk, ValType::U32, SsaOp::Phi(Vec::new()));
                    let init = state.r[reg as usize];
                    state.r[reg as usize] = phi;
                    r_phis.push((reg, phi, init));
                }
                let mut ur_phis = Vec::new();
                for &ur in &modified.ur {
                    let phi = self.alloc_val(header_blk, ValType::U32, SsaOp::Phi(Vec::new()));
                    let init = state.ur[ur as usize];
                    state.ur[ur as usize] = phi;
                    ur_phis.push((ur, phi, init));
                }
                let mut p_phis = Vec::new();
                for &pr in &modified.p {
                    let phi = self.alloc_val(header_blk, ValType::Bool, SsaOp::Phi(Vec::new()));
                    let init = state.p[pr as usize];
                    state.p[pr as usize] = phi;
                    p_phis.push((pr, phi, init));
                }

                self.blocks[header_blk].merge = MergeHint::Loop {
                    merge_block: merge_blk,
                    continue_block: continue_blk,
                };
                self.blocks[header_blk].terminator = Terminator::Branch { target: body_entry };

                let mut body_cur = body_entry;
                let mut body_active = true;
                self.lower_node_seq(body_nodes, &mut body_cur, state, &mut body_active)?;
                let _ = header_idx;

                if body_active {
                    let raw_cond = self.read_pred(body_cur, state, *continue_cond);
                    let loop_cond = if *continue_negated {
                        self.alloc_val(body_cur, ValType::Bool, SsaOp::BoolNot(raw_cond))
                    } else {
                        raw_cond
                    };
                    self.blocks[body_cur].terminator = Terminator::BranchCond {
                        cond: loop_cond,
                        true_target: continue_blk,
                        false_target: merge_blk,
                    };
                    self.blocks[continue_blk].terminator = Terminator::Branch { target: header_blk };

                    for (reg, phi_id, init) in r_phis {
                        patch_phi(&mut self.blocks[header_blk], phi_id, vec![(preheader, init), (continue_blk, state.r[reg as usize])]);
                    }
                    for (ur, phi_id, init) in ur_phis {
                        patch_phi(&mut self.blocks[header_blk], phi_id, vec![(preheader, init), (continue_blk, state.ur[ur as usize])]);
                    }
                    for (pr, phi_id, init) in p_phis {
                        patch_phi(&mut self.blocks[header_blk], phi_id, vec![(preheader, init), (continue_blk, state.p[pr as usize])]);
                    }
                } else {
                    self.blocks[continue_blk].terminator = Terminator::Branch { target: header_blk };
                    for (reg, phi_id, init) in r_phis {
                        let _ = reg;
                        patch_phi(&mut self.blocks[header_blk], phi_id, vec![(preheader, init), (continue_blk, init)]);
                    }
                    for (ur, phi_id, init) in ur_phis {
                        let _ = ur;
                        patch_phi(&mut self.blocks[header_blk], phi_id, vec![(preheader, init), (continue_blk, init)]);
                    }
                    for (pr, phi_id, init) in p_phis {
                        let _ = pr;
                        patch_phi(&mut self.blocks[header_blk], phi_id, vec![(preheader, init), (continue_blk, init)]);
                    }
                    *active = false;
                }

                *cur_blk = merge_blk;
                Ok(())
            }
        }
    }

    fn merge_states(
        &mut self,
        merge_blk: usize,
        out: &mut RegState,
        left: Option<(usize, &RegState)>,
        right: Option<(usize, &RegState)>,
        active: &mut bool,
    ) {
        match (left, right) {
            (None, None) => {
                *active = false;
                self.blocks[merge_blk].terminator = Terminator::Unreachable;
            }
            (Some((_, s)), None) | (None, Some((_, s))) => {
                *out = s.clone();
            }
            (Some((lb, ls)), Some((rb, rs))) => {
                *out = ls.clone();
                for i in 0..255 {
                    if ls.r[i] != rs.r[i] {
                        out.r[i] = self.alloc_val(
                            merge_blk,
                            ValType::U32,
                            SsaOp::Phi(vec![(lb, ls.r[i]), (rb, rs.r[i])]),
                        );
                    }
                    if ls.r_ptr[i].is_none() || rs.r_ptr[i].is_none() {
                        out.r_ptr[i] = None;
                    }
                }
                for i in 0..63 {
                    if ls.ur[i] != rs.ur[i] {
                        out.ur[i] = self.alloc_val(
                            merge_blk,
                            ValType::U32,
                            SsaOp::Phi(vec![(lb, ls.ur[i]), (rb, rs.ur[i])]),
                        );
                    }
                    if ls.ur_ptr[i].is_none() || rs.ur_ptr[i].is_none() {
                        out.ur_ptr[i] = None;
                    }
                }
                for i in 0..7 {
                    if ls.p[i] != rs.p[i] {
                        out.p[i] = self.alloc_val(
                            merge_blk,
                            ValType::Bool,
                            SsaOp::Phi(vec![(lb, ls.p[i]), (rb, rs.p[i])]),
                        );
                    }
                    if ls.up[i] != rs.up[i] {
                        out.up[i] = self.alloc_val(
                            merge_blk,
                            ValType::Bool,
                            SsaOp::Phi(vec![(lb, ls.up[i]), (rb, rs.up[i])]),
                        );
                    }
                }
            }
        }
    }

    fn lower_single_insn(
        &mut self,
        insn: DecodedInsn,
        cur_blk: &mut usize,
        state: &mut RegState,
        active: &mut bool,
    ) -> Result<(), FallbackReason> {
        if insn.guard.is_always_false() || matches!(insn.op, DecodedOp::Nop) {
            return Ok(());
        }
        if let DecodedOp::Unsupported { raw_opcode, reason } = insn.op {
            return Err(FallbackReason {
                offset: (insn.index * 16) as u32,
                opcode: raw_opcode,
                reason,
            });
        }
        if matches!(insn.op, DecodedOp::Exit) {
            if insn.guard.is_always_true() {
                self.blocks[*cur_blk].terminator = Terminator::Return;
                *active = false;
                return Ok(());
            }
            // Predicated EXIT: continue in !guard branch; guarded branch returns.
            let cond = self.read_pred(*cur_blk, state, insn.guard);
            let exit_blk = self.new_block();
            let cont_blk = self.new_block();
            self.blocks[*cur_blk].merge = MergeHint::Selection {
                merge_block: cont_blk,
            };
            self.blocks[*cur_blk].terminator = Terminator::BranchCond {
                cond,
                true_target: exit_blk,
                false_target: cont_blk,
            };
            self.blocks[exit_blk].terminator = Terminator::Return;
            *cur_blk = cont_blk;
            return Ok(());
        }

        // Barriers cannot safely run inside divergent conditional branches.
        if !insn.guard.is_always_true()
            && matches!(insn.op, DecodedOp::BarSync { .. } | DecodedOp::Membar { .. })
        {
            return Err(FallbackReason {
                offset: (insn.index * 16) as u32,
                opcode: insn.raw_opcode,
                reason: "conditional barrier cannot be safely structured",
            });
        }

        if insn.guard.is_always_true() {
            return self.lower_unpredicated_op(insn, *cur_blk, state);
        }

        // General predicated instruction: clone state before/after or emit structured selection.
        let has_side_effects = matches!(
            insn.op,
            DecodedOp::Stg { .. }
                | DecodedOp::Sts { .. }
                | DecodedOp::Stl { .. }
                | DecodedOp::Ldg { .. }
                | DecodedOp::Lds { .. }
                | DecodedOp::Atoms { .. }
                | DecodedOp::Atomg { .. }
                | DecodedOp::Hmma { .. }
                | DecodedOp::Shfl { .. }
        );
        if has_side_effects {
            let cond = self.read_pred(*cur_blk, state, insn.guard);
            let then_blk = self.new_block();
            let merge_blk = self.new_block();
            self.blocks[*cur_blk].merge = MergeHint::Selection {
                merge_block: merge_blk,
            };
            self.blocks[*cur_blk].terminator = Terminator::BranchCond {
                cond,
                true_target: then_blk,
                false_target: merge_blk,
            };
            let pre_state = state.clone();
            let mut then_state = state.clone();
            self.lower_unpredicated_op(insn, then_blk, &mut then_state)?;
            self.blocks[then_blk].terminator = Terminator::Branch { target: merge_blk };
            let mut dummy_active = true;
            self.merge_states(
                merge_blk,
                state,
                Some((then_blk, &then_state)),
                Some((*cur_blk, &pre_state)),
                &mut dummy_active,
            );
            *cur_blk = merge_blk;
            Ok(())
        } else {
            let cond = self.read_pred(*cur_blk, state, insn.guard);
            let prev = state.clone();
            self.lower_unpredicated_op(insn, *cur_blk, state)?;
            for i in 0..255 {
                if state.r[i] != prev.r[i] {
                    state.r[i] = self.alloc_val(
                        *cur_blk,
                        ValType::U32,
                        SsaOp::Select {
                            cond,
                            t: state.r[i],
                            f: prev.r[i],
                        },
                    );
                }
            }
            for i in 0..63 {
                if state.ur[i] != prev.ur[i] {
                    state.ur[i] = self.alloc_val(
                        *cur_blk,
                        ValType::U32,
                        SsaOp::Select {
                            cond,
                            t: state.ur[i],
                            f: prev.ur[i],
                        },
                    );
                }
            }
            for i in 0..7 {
                if state.p[i] != prev.p[i] {
                    state.p[i] = self.alloc_val(
                        *cur_blk,
                        ValType::Bool,
                        SsaOp::Select {
                            cond,
                            t: state.p[i],
                            f: prev.p[i],
                        },
                    );
                }
            }
            Ok(())
        }
    }

    fn lower_unpredicated_op(
        &mut self,
        insn: DecodedInsn,
        blk: usize,
        state: &mut RegState,
    ) -> Result<(), FallbackReason> {
        let off = (insn.index * 16) as u32;
        let op = insn.op;
        let fail = |reason: &'static str| {
            Err(FallbackReason {
                offset: off,
                opcode: insn.raw_opcode,
                reason,
            })
        };

        match op {
            DecodedOp::Nop | DecodedOp::Exit | DecodedOp::Bra { .. } => {}
            DecodedOp::Mov { dst, src } => {
                let v = self.eval_src(blk, state, src);
                let ptr = self.src_ptr(state, src);
                self.write_reg(state, dst, v, ptr);
                if let SrcOperand::Cbuf(cbuf) = src {
                    if (dst as usize) < 255 {
                        state.r_cbuf_hi[dst as usize] = Some((u32::from(cbuf.bank), u32::from(cbuf.offset)));
                    }
                }
            }
            DecodedOp::Umov { dst_ur, src } => {
                let v = self.eval_src(blk, state, src);
                let ptr = self.src_ptr(state, src);
                self.write_ureg(state, dst_ur, v, ptr);
                if let SrcOperand::Cbuf(cbuf) = src {
                    if (dst_ur as usize) < 63 {
                        state.ur_cbuf_hi[dst_ur as usize] = Some((u32::from(cbuf.bank), u32::from(cbuf.offset)));
                    }
                }
            }
            DecodedOp::Sel { dst, a, b, pred } => {
                let cond = self.read_pred(blk, state, pred);
                let va = self.eval_src(blk, state, a);
                let vb = self.eval_src(blk, state, b);
                let v = self.alloc_val(blk, ValType::U32, SsaOp::Select { cond, t: va, f: vb });
                self.write_reg(state, dst, v, None);
            }
            DecodedOp::Cs2rZero { dst } => {
                self.write_reg(state, dst, self.zero_u32, None);
            }
            DecodedOp::S2r { dst, special } => {
                let v = self.lower_special(blk, special, off, insn.raw_opcode)?;
                self.write_reg(state, dst, v, None);
            }
            DecodedOp::S2ur { dst_ur, special } => {
                let v = self.lower_special(blk, special, off, insn.raw_opcode)?;
                self.write_ureg(state, dst_ur, v, None);
            }
            DecodedOp::R2ur { dst_ur, src } => {
                let v = state.r[src as usize];
                self.write_ureg(state, dst_ur, v, state.r_ptr[src as usize]);
            }
            DecodedOp::Iadd3 {
                dst,
                a,
                b,
                c,
                neg_a,
                neg_b,
                neg_c,
                x_mode,
                carry_in0,
                carry_in1,
                carry_out0,
                carry_out1,
            } => {
                let va_raw = self.eval_src(blk, state, a);
                let vb_raw = self.eval_src(blk, state, b);
                let vc_raw = self.eval_src(blk, state, c);
                let va = if neg_a { self.alloc_val(blk, ValType::U32, SsaOp::BitNot(va_raw)) } else { va_raw };
                let vb = if neg_b { self.alloc_val(blk, ValType::U32, SsaOp::BitNot(vb_raw)) } else { vb_raw };
                let vc = if neg_c { self.alloc_val(blk, ValType::U32, SsaOp::BitNot(vc_raw)) } else { vc_raw };

                let mut sum = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(va, vb));
                let c0 = self.alloc_val(
                    blk,
                    ValType::Bool,
                    SsaOp::IntCmp { cmp: IntCmp::Lt, signed: false, a: sum, b: va },
                );
                let sum_ab_c = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(sum, vc));
                let c1 = self.alloc_val(
                    blk,
                    ValType::Bool,
                    SsaOp::IntCmp { cmp: IntCmp::Lt, signed: false, a: sum_ab_c, b: sum },
                );
                sum = sum_ab_c;

                let c0u = self.select_u32_bool(blk, c0);
                let c1u = self.select_u32_bool(blk, c1);
                let mut total_carries = self.alloc_val(
                    blk,
                    ValType::U32,
                    SsaOp::IAdd(c0u, c1u),
                );

                if !x_mode && (neg_a || neg_b || neg_c) {
                    let s2 = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(sum, self.one_u32));
                    let c2 = self.alloc_val(
                        blk,
                        ValType::Bool,
                        SsaOp::IntCmp { cmp: IntCmp::Lt, signed: false, a: s2, b: sum },
                    );
                    let c2u = self.select_u32_bool(blk, c2);
                    total_carries = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(total_carries, c2u));
                    sum = s2;
                } else if x_mode {
                    for cin in [carry_in0, carry_in1] {
                        if cin == 15 {
                            continue;
                        }
                        let g = PredGuard { neg: (cin & 8) != 0, id: cin & 7 };
                        let p = self.read_pred(blk, state, g);
                        let pu = self.select_u32_bool(blk, p);
                        let s2 = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(sum, pu));
                        let c2 = self.alloc_val(
                            blk,
                            ValType::Bool,
                            SsaOp::IntCmp { cmp: IntCmp::Lt, signed: false, a: s2, b: sum },
                        );
                        let c2u = self.select_u32_bool(blk, c2);
                        total_carries = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(total_carries, c2u));
                        sum = s2;
                    }
                }

                if carry_out0 < 7 {
                    state.p[carry_out0 as usize] = self.alloc_val(
                        blk,
                        ValType::Bool,
                        SsaOp::IntCmp { cmp: IntCmp::Ge, signed: false, a: total_carries, b: self.one_u32 },
                    );
                }
                if carry_out1 < 7 {
                    let two = self.const_u32(blk, 2);
                    state.p[carry_out1 as usize] = self.alloc_val(
                        blk,
                        ValType::Bool,
                        SsaOp::IntCmp { cmp: IntCmp::Ge, signed: false, a: total_carries, b: two },
                    );
                }

                // Track pointer base provenance when adding an offset to a pointer low word.
                let ptr = self.propagate_add_ptr(blk, state, a, b, c, neg_a, neg_b, neg_c);
                self.write_reg(state, dst, sum, ptr);
            }
            DecodedOp::Uiadd3 {
                dst_ur,
                a,
                b,
                c,
                neg_a,
                neg_b,
                x_mode,
                carry_in,
                carry_out,
            } => {
                let va_raw = self.eval_src(blk, state, a);
                let vb_raw = self.eval_src(blk, state, b);
                let vc = self.eval_src(blk, state, c);
                let va = if neg_a { self.alloc_val(blk, ValType::U32, SsaOp::BitNot(va_raw)) } else { va_raw };
                let vb = if neg_b { self.alloc_val(blk, ValType::U32, SsaOp::BitNot(vb_raw)) } else { vb_raw };
                let s1 = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(va, vb));
                let c0 = self.alloc_val(blk, ValType::Bool, SsaOp::IntCmp { cmp: IntCmp::Lt, signed: false, a: s1, b: va });
                let mut sum = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(s1, vc));
                if !x_mode && (neg_a || neg_b) {
                    sum = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(sum, self.one_u32));
                } else if x_mode && carry_in != 15 {
                    let g = PredGuard { neg: (carry_in & 8) != 0, id: carry_in & 7 };
                    let p = self.read_pred(blk, state, g);
                    let pu = self.select_u32_bool(blk, p);
                    sum = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(sum, pu));
                }
                if carry_out < 7 {
                    state.up[carry_out as usize] = c0;
                }
                let ptr = self.propagate_add_ptr(blk, state, a, b, c, neg_a, neg_b, false);
                self.write_ureg(state, dst_ur, sum, ptr);
            }
            DecodedOp::Imad { dst, a, b, c, neg_c, signed, hi, wide } => {
                let va = self.eval_src(blk, state, a);
                let vb = self.eval_src(blk, state, b);
                let vc_raw = self.eval_src(blk, state, c);
                let vc = if neg_c { self.alloc_val(blk, ValType::U32, SsaOp::INeg(vc_raw)) } else { vc_raw };
                if wide {
                    let prod = if signed {
                        self.alloc_val(blk, ValType::U32, SsaOp::SMulHi(va, vb))
                    } else {
                        self.alloc_val(blk, ValType::U32, SsaOp::UMulHi(va, vb))
                    };
                    let lo = self.alloc_val(blk, ValType::U32, SsaOp::IMul(va, vb));
                    let lo_sum = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(lo, vc));
                    let carry = self.alloc_val(blk, ValType::Bool, SsaOp::IntCmp { cmp: IntCmp::Lt, signed: false, a: lo_sum, b: lo });
                    let carry_u = self.select_u32_bool(blk, carry);
                    let hi_val = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(prod, carry_u));
                    self.write_reg(state, dst, lo_sum, None);
                    self.write_reg(state, dst.saturating_add(1), hi_val, None);
                } else if hi {
                    let prod = if signed {
                        self.alloc_val(blk, ValType::U32, SsaOp::SMulHi(va, vb))
                    } else {
                        self.alloc_val(blk, ValType::U32, SsaOp::UMulHi(va, vb))
                    };
                    let sum = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(prod, vc));
                    self.write_reg(state, dst, sum, None);
                } else {
                    let prod = self.alloc_val(blk, ValType::U32, SsaOp::IMul(va, vb));
                    let sum = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(prod, vc));
                    let ptr = if !neg_c {
                        self.src_ptr(state, c).map(|orig| {
                            let new_off = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(orig.byte_offset, prod));
                            PtrOrigin { byte_offset: new_off, ..orig }
                        })
                    } else {
                        None
                    };
                    self.write_reg(state, dst, sum, ptr);
                }
            }
            DecodedOp::Uimad { dst_ur, a, b, c, neg_c, signed, wide } => {
                let va = self.eval_src(blk, state, a);
                let vb = self.eval_src(blk, state, b);
                let vc_raw = self.eval_src(blk, state, c);
                let vc = if neg_c { self.alloc_val(blk, ValType::U32, SsaOp::INeg(vc_raw)) } else { vc_raw };
                if wide {
                    let prod = if signed {
                        self.alloc_val(blk, ValType::U32, SsaOp::SMulHi(va, vb))
                    } else {
                        self.alloc_val(blk, ValType::U32, SsaOp::UMulHi(va, vb))
                    };
                    let lo = self.alloc_val(blk, ValType::U32, SsaOp::IMul(va, vb));
                    let lo_sum = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(lo, vc));
                    let carry = self.alloc_val(blk, ValType::Bool, SsaOp::IntCmp { cmp: IntCmp::Lt, signed: false, a: lo_sum, b: lo });
                    let carry_u = self.select_u32_bool(blk, carry);
                    let hi_val = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(prod, carry_u));
                    self.write_ureg(state, dst_ur, lo_sum, None);
                    self.write_ureg(state, dst_ur.saturating_add(1), hi_val, None);
                } else {
                    let prod = self.alloc_val(blk, ValType::U32, SsaOp::IMul(va, vb));
                    let sum = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(prod, vc));
                    self.write_ureg(state, dst_ur, sum, None);
                }
            }
            DecodedOp::Lea { dst, a, b, c_hi, scale, hi, neg_b, x_mode, carry_out } => {
                if hi || x_mode {
                    let va = self.eval_src(blk, state, a);
                    let vb_raw = self.eval_src(blk, state, b);
                    let vc = self.eval_src(blk, state, c_hi);
                    let vb = if neg_b { self.alloc_val(blk, ValType::U32, SsaOp::INeg(vb_raw)) } else { vb_raw };
                    let sh = u32::from(scale);
                    let shifted = if sh == 0 {
                        va
                    } else if sh >= 32 {
                        self.zero_u32
                    } else {
                        let lo_shift = self.const_u32(blk, sh);
                        let hi_shift = self.const_u32(blk, 32 - sh);
                        let top_c = self.alloc_val(blk, ValType::U32, SsaOp::Shl(vc, lo_shift));
                        let hi_a = self.alloc_val(blk, ValType::U32, SsaOp::LShr(va, hi_shift));
                        self.alloc_val(blk, ValType::U32, SsaOp::BitOr(top_c, hi_a))
                    };
                    let sum = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(shifted, vb));
                    self.write_reg(state, dst, sum, None);
                } else {
                    let va = self.eval_src(blk, state, a);
                    let vb_raw = self.eval_src(blk, state, b);
                    let vb = if neg_b { self.alloc_val(blk, ValType::U32, SsaOp::INeg(vb_raw)) } else { vb_raw };
                    let sh = self.const_u32(blk, u32::from(scale));
                    let scaled = if scale == 0 {
                        va
                    } else {
                        self.alloc_val(blk, ValType::U32, SsaOp::Shl(va, sh))
                    };
                    let sum = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(scaled, vb));
                    if carry_out < 7 {
                        state.p[carry_out as usize] = self.alloc_val(
                            blk,
                            ValType::Bool,
                            SsaOp::IntCmp { cmp: IntCmp::Lt, signed: false, a: sum, b: scaled },
                        );
                    }
                    let ptr = if !neg_b {
                        self.src_ptr(state, b).map(|orig| {
                            let new_off = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(orig.byte_offset, scaled));
                            PtrOrigin { byte_offset: new_off, ..orig }
                        })
                    } else {
                        None
                    };
                    self.write_reg(state, dst, sum, ptr);
                }
            }
            DecodedOp::Ulea { dst_ur, a, b, scale, hi, neg_b } => {
                if hi {
                    return fail("ULEA.HI unimplemented in direct SSA");
                }
                let va = self.eval_src(blk, state, a);
                let vb_raw = self.eval_src(blk, state, b);
                let vb = if neg_b { self.alloc_val(blk, ValType::U32, SsaOp::INeg(vb_raw)) } else { vb_raw };
                let sh = self.const_u32(blk, u32::from(scale));
                let scaled = if scale == 0 { va } else { self.alloc_val(blk, ValType::U32, SsaOp::Shl(va, sh)) };
                let sum = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(scaled, vb));
                let ptr = if !neg_b {
                    self.src_ptr(state, b).map(|orig| {
                        let new_off = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(orig.byte_offset, scaled));
                        PtrOrigin { byte_offset: new_off, ..orig }
                    })
                } else {
                    None
                };
                self.write_ureg(state, dst_ur, sum, ptr);
            }
            DecodedOp::Lop3 { dst, a, b, c, lut, pred_out } => {
                let va = self.eval_src(blk, state, a);
                let vb = self.eval_src(blk, state, b);
                let vc = self.eval_src(blk, state, c);
                let res = self.lower_lop3_lut(blk, va, vb, vc, lut);
                self.write_reg(state, dst, res, None);
                if pred_out < 7 {
                    state.p[pred_out as usize] = self.alloc_val(
                        blk,
                        ValType::Bool,
                        SsaOp::IntCmp { cmp: IntCmp::Ne, signed: false, a: res, b: self.zero_u32 },
                    );
                }
            }
            DecodedOp::Ulop3 { dst_ur, a, b, c, lut, pred_out } => {
                let va = self.eval_src(blk, state, a);
                let vb = self.eval_src(blk, state, b);
                let vc = self.eval_src(blk, state, c);
                let res = self.lower_lop3_lut(blk, va, vb, vc, lut);
                self.write_ureg(state, dst_ur, res, None);
                if pred_out < 7 {
                    state.up[pred_out as usize] = self.alloc_val(
                        blk,
                        ValType::Bool,
                        SsaOp::IntCmp { cmp: IntCmp::Ne, signed: false, a: res, b: self.zero_u32 },
                    );
                }
            }
            DecodedOp::Shf { dst, a, shift, c, right, hi, signed, wrap } => {
                let va = self.eval_src(blk, state, a);
                let vsh = self.eval_src(blk, state, shift);
                let vc = self.eval_src(blk, state, c);
                let res = self.lower_shf(blk, va, vsh, vc, right, hi, signed, wrap);
                self.write_reg(state, dst, res, None);
            }
            DecodedOp::Ushf { dst_ur, a, shift, c, right, hi, signed, wrap } => {
                let va = self.eval_src(blk, state, a);
                let vsh = self.eval_src(blk, state, shift);
                let vc = self.eval_src(blk, state, c);
                let res = self.lower_shf(blk, va, vsh, vc, right, hi, signed, wrap);
                self.write_ureg(state, dst_ur, res, None);
            }
            DecodedOp::Isetp { dst_p, dst_q, a, b, cmp, signed, comb, accum } => {
                let va = self.eval_src(blk, state, a);
                let vb = self.eval_src(blk, state, b);
                let c_val = self.alloc_val(blk, ValType::Bool, SsaOp::IntCmp { cmp, signed, a: va, b: vb });
                let p_val = self.combine_pred(blk, state, c_val, comb, accum);
                let nc_val = self.alloc_val(blk, ValType::Bool, SsaOp::BoolNot(c_val));
                let q_val = self.combine_pred(blk, state, nc_val, comb, accum);
                if dst_p < 7 {
                    state.p[dst_p as usize] = p_val;
                }
                if dst_q < 7 {
                    state.p[dst_q as usize] = q_val;
                }
            }
            DecodedOp::Uisetp { dst_up, a, b, cmp, signed, comb, accum } => {
                let va = self.eval_src(blk, state, a);
                let vb = self.eval_src(blk, state, b);
                let c_val = self.alloc_val(blk, ValType::Bool, SsaOp::IntCmp { cmp, signed, a: va, b: vb });
                let acc = if accum.id == 7 {
                    self.true_bool
                } else {
                    let b_up = state.up[accum.id as usize];
                    if accum.neg {
                        self.alloc_val(blk, ValType::Bool, SsaOp::BoolNot(b_up))
                    } else {
                        b_up
                    }
                };
                let res = match comb {
                    BoolComb::And => self.alloc_val(blk, ValType::Bool, SsaOp::BoolAnd(c_val, acc)),
                    BoolComb::Or => self.alloc_val(blk, ValType::Bool, SsaOp::BoolOr(c_val, acc)),
                    BoolComb::Xor => self.alloc_val(blk, ValType::Bool, SsaOp::BoolXor(c_val, acc)),
                };
                if dst_up < 7 {
                    state.up[dst_up as usize] = res;
                }
            }
            DecodedOp::Imnmx { dst, a, b, pred_min, signed } => {
                let va = self.eval_src(blk, state, a);
                let vb = self.eval_src(blk, state, b);
                let use_min = self.read_pred(blk, state, pred_min);
                let min_v = self.alloc_val(blk, ValType::U32, SsaOp::Glsl2(if signed { GlslOp::SMin } else { GlslOp::UMin }, va, vb));
                let max_v = self.alloc_val(blk, ValType::U32, SsaOp::Glsl2(if signed { GlslOp::SMax } else { GlslOp::UMax }, va, vb));
                let res = self.alloc_val(blk, ValType::U32, SsaOp::Select { cond: use_min, t: min_v, f: max_v });
                self.write_reg(state, dst, res, None);
            }
            DecodedOp::Iabs { dst, src } => {
                let v = self.eval_src(blk, state, src);
                let abs_v = self.alloc_val(blk, ValType::U32, SsaOp::Glsl1(GlslOp::SAbs, v));
                self.write_reg(state, dst, abs_v, None);
            }
            DecodedOp::Popc { dst, src } => {
                let v = self.eval_src(blk, state, src);
                let bc = self.alloc_val(blk, ValType::U32, SsaOp::BitCount(v));
                self.write_reg(state, dst, bc, None);
            }
            DecodedOp::Brev { dst, src } => {
                let v = self.eval_src(blk, state, src);
                let br = self.alloc_val(blk, ValType::U32, SsaOp::BitReverse(v));
                self.write_reg(state, dst, br, None);
            }
            DecodedOp::Flo { dst, src, signed, inv, shamt } => {
                let mut v = self.eval_src(blk, state, src);
                if inv {
                    v = self.alloc_val(blk, ValType::U32, SsaOp::BitNot(v));
                }
                let msb = self.alloc_val(
                    blk,
                    ValType::U32,
                    SsaOp::Glsl1(if signed { GlslOp::FindSMsb } else { GlslOp::FindUMsb }, v),
                );
                let res = if shamt {
                    let neg1 = self.const_u32(blk, u32::MAX);
                    let is_neg1 = self.alloc_val(
                        blk,
                        ValType::Bool,
                        SsaOp::IntCmp { cmp: IntCmp::Eq, signed: false, a: msb, b: neg1 },
                    );
                    let c31 = self.const_u32(blk, 31);
                    let diff = self.alloc_val(blk, ValType::U32, SsaOp::ISub(c31, msb));
                    self.alloc_val(blk, ValType::U32, SsaOp::Select { cond: is_neg1, t: neg1, f: diff })
                } else {
                    msb
                };
                self.write_reg(state, dst, res, None);
            }
            DecodedOp::Uflo { dst_ur, src, shamt } => {
                let v = self.eval_src(blk, state, src);
                let msb = self.alloc_val(blk, ValType::U32, SsaOp::Glsl1(GlslOp::FindUMsb, v));
                let res = if shamt {
                    let neg1 = self.const_u32(blk, u32::MAX);
                    let is_neg1 = self.alloc_val(
                        blk,
                        ValType::Bool,
                        SsaOp::IntCmp { cmp: IntCmp::Eq, signed: false, a: msb, b: neg1 },
                    );
                    let c31 = self.const_u32(blk, 31);
                    let diff = self.alloc_val(blk, ValType::U32, SsaOp::ISub(c31, msb));
                    self.alloc_val(blk, ValType::U32, SsaOp::Select { cond: is_neg1, t: neg1, f: diff })
                } else {
                    msb
                };
                self.write_ureg(state, dst_ur, res, None);
            }
            DecodedOp::Sgxt { dst, src, width, signed } => {
                let v = self.eval_src(blk, state, src);
                let res = if width == 0 {
                    self.zero_u32
                } else if width >= 32 {
                    v
                } else if signed {
                    self.alloc_val(blk, ValType::U32, SsaOp::SBitFieldExtract { val: v, offset: 0, count: width })
                } else {
                    self.alloc_val(blk, ValType::U32, SsaOp::UBitFieldExtract { val: v, offset: 0, count: width })
                };
                self.write_reg(state, dst, res, None);
            }
            DecodedOp::Prmt { dst, a, sel, c } => {
                let va = self.eval_src(blk, state, a);
                let vc = self.eval_src(blk, state, c);
                let mut acc = self.zero_u32;
                for lane in 0..4u32 {
                    let nibble = (sel >> (lane * 4)) & 0xf;
                    let src_word = if (nibble & 4) != 0 { vc } else { va };
                    let byte_idx = (nibble & 3) as u32;
                    let sign_extend = (nibble & 8) != 0;
                    let extracted = if sign_extend {
                        let sbyte = self.alloc_val(
                            blk,
                            ValType::U32,
                            SsaOp::SBitFieldExtract { val: src_word, offset: byte_idx * 8 + 7, count: 1 },
                        );
                        let mask_ff = self.const_u32(blk, 0xff);
                        self.alloc_val(blk, ValType::U32, SsaOp::BitAnd(sbyte, mask_ff))
                    } else {
                        self.alloc_val(
                            blk,
                            ValType::U32,
                            SsaOp::UBitFieldExtract { val: src_word, offset: byte_idx * 8, count: 8 },
                        )
                    };
                    let shifted = if lane == 0 {
                        extracted
                    } else {
                        let s = self.const_u32(blk, lane * 8);
                        self.alloc_val(blk, ValType::U32, SsaOp::Shl(extracted, s))
                    };
                    acc = self.alloc_val(blk, ValType::U32, SsaOp::BitOr(acc, shifted));
                }
                self.write_reg(state, dst, acc, None);
            }
            DecodedOp::Fadd { dst, a, b, sat } => {
                let fa = self.eval_float_src(blk, state, a);
                let fb = self.eval_float_src(blk, state, b);
                let mut f = self.alloc_val(blk, ValType::F32, SsaOp::FAdd(fa, fb));
                if sat {
                    f = self.saturate_f32(blk, f);
                }
                let bits = self.to_u32_bits(blk, f);
                self.write_reg(state, dst, bits, None);
            }
            DecodedOp::Fmul { dst, a, b, sat } => {
                let fa = self.eval_float_src(blk, state, a);
                let fb = self.eval_float_src(blk, state, b);
                let mut f = self.alloc_val(blk, ValType::F32, SsaOp::FMul(fa, fb));
                if sat {
                    f = self.saturate_f32(blk, f);
                }
                let bits = self.to_u32_bits(blk, f);
                self.write_reg(state, dst, bits, None);
            }
            DecodedOp::Ffma { dst, a, b, c, sat } => {
                let fa = self.eval_float_src(blk, state, a);
                let fb = self.eval_float_src(blk, state, b);
                let fc = self.eval_float_src(blk, state, c);
                let prod = self.alloc_val(blk, ValType::F32, SsaOp::FMul(fa, fb));
                let mut f = self.alloc_val(blk, ValType::F32, SsaOp::FAdd(prod, fc));
                if sat {
                    f = self.saturate_f32(blk, f);
                }
                let bits = self.to_u32_bits(blk, f);
                self.write_reg(state, dst, bits, None);
            }
            DecodedOp::Fmnmx { dst, a, b, pred_min } => {
                let fa = self.eval_float_src(blk, state, a);
                let fb = self.eval_float_src(blk, state, b);
                let use_min = self.read_pred(blk, state, pred_min);
                let min_v = self.alloc_val(blk, ValType::F32, SsaOp::Glsl2(GlslOp::FMin, fa, fb));
                let max_v = self.alloc_val(blk, ValType::F32, SsaOp::Glsl2(GlslOp::FMax, fa, fb));
                let min_u = self.to_u32_bits(blk, min_v);
                let max_u = self.to_u32_bits(blk, max_v);
                let sel = self.alloc_val(blk, ValType::U32, SsaOp::Select { cond: use_min, t: min_u, f: max_u });
                self.write_reg(state, dst, sel, None);
            }
            DecodedOp::Fsel { dst, a, b, pred } => {
                let cond = self.read_pred(blk, state, pred);
                let fa = self.eval_float_src(blk, state, a);
                let fb = self.eval_float_src(blk, state, b);
                let ua = self.to_u32_bits(blk, fa);
                let ub = self.to_u32_bits(blk, fb);
                let v = self.alloc_val(blk, ValType::U32, SsaOp::Select { cond, t: ua, f: ub });
                self.write_reg(state, dst, v, None);
            }
            DecodedOp::Fsetp { dst_p, dst_q, a, b, cmp, comb, accum } => {
                let fa = self.eval_float_src(blk, state, a);
                let fb = self.eval_float_src(blk, state, b);
                let c_val = self.alloc_val(blk, ValType::Bool, SsaOp::FSet { cmp, a: fa, b: fb });
                let p_val = self.combine_pred(blk, state, c_val, comb, accum);
                let nc_val = self.alloc_val(blk, ValType::Bool, SsaOp::BoolNot(c_val));
                let q_val = self.combine_pred(blk, state, nc_val, comb, accum);
                if dst_p < 7 {
                    state.p[dst_p as usize] = p_val;
                }
                if dst_q < 7 {
                    state.p[dst_q as usize] = q_val;
                }
            }
            DecodedOp::Mufu { dst, src, op: m_op, abs, neg, sat } => {
                let mut f = self.eval_float_src(blk, state, FloatSrc { op: src, abs, neg });
                f = match m_op {
                    MufuOp::CosTurns | MufuOp::SinTurns => {
                        let two_pi_bits = self.const_u32(blk, 0x40c9_0fdbu32);
                        let two_pi = self.alloc_val(blk, ValType::F32, SsaOp::Bitcast { to: ValType::F32, val: two_pi_bits });
                        let rad = self.alloc_val(blk, ValType::F32, SsaOp::FMul(f, two_pi));
                        let gl = if m_op == MufuOp::CosTurns { GlslOp::Cos } else { GlslOp::Sin };
                        self.alloc_val(blk, ValType::F32, SsaOp::Glsl1(gl, rad))
                    }
                    MufuOp::Ex2 => self.alloc_val(blk, ValType::F32, SsaOp::Glsl1(GlslOp::Exp2, f)),
                    MufuOp::Lg2 => self.alloc_val(blk, ValType::F32, SsaOp::Glsl1(GlslOp::Log2, f)),
                    MufuOp::Sqrt => self.alloc_val(blk, ValType::F32, SsaOp::Glsl1(GlslOp::Sqrt, f)),
                    MufuOp::Rsq => self.alloc_val(blk, ValType::F32, SsaOp::Glsl1(GlslOp::InverseSqrt, f)),
                    MufuOp::Rcp => {
                        let one_bits = self.const_u32(blk, 1.0f32.to_bits());
                        let one = self.alloc_val(blk, ValType::F32, SsaOp::Bitcast { to: ValType::F32, val: one_bits });
                        self.alloc_val(blk, ValType::F32, SsaOp::FDiv(one, f))
                    }
                };
                if sat {
                    f = self.saturate_f32(blk, f);
                }
                let bits = self.to_u32_bits(blk, f);
                self.write_reg(state, dst, bits, None);
            }
            DecodedOp::I2f { dst, src, src_signed, src_bits, byte_lane, dst_f16, abs } => {
                let raw = self.eval_src(blk, state, src);
                let narrow = match src_bits {
                    8 => {
                        let off_b = u32::from(byte_lane) * 8;
                        if src_signed {
                            self.alloc_val(blk, ValType::U32, SsaOp::SBitFieldExtract { val: raw, offset: off_b, count: 8 })
                        } else {
                            self.alloc_val(blk, ValType::U32, SsaOp::UBitFieldExtract { val: raw, offset: off_b, count: 8 })
                        }
                    }
                    16 => {
                        let off_b = u32::from(byte_lane & 1) * 16;
                        if src_signed {
                            self.alloc_val(blk, ValType::U32, SsaOp::SBitFieldExtract { val: raw, offset: off_b, count: 16 })
                        } else {
                            self.alloc_val(blk, ValType::U32, SsaOp::UBitFieldExtract { val: raw, offset: off_b, count: 16 })
                        }
                    }
                    _ => raw,
                };
                let conv = if src_signed {
                    let abs_v = if abs {
                        self.alloc_val(blk, ValType::U32, SsaOp::Glsl1(GlslOp::SAbs, narrow))
                    } else {
                        narrow
                    };
                    self.alloc_val(blk, ValType::F32, SsaOp::SConvertToF32(abs_v))
                } else {
                    self.alloc_val(blk, ValType::F32, SsaOp::UConvertToF32(narrow))
                };
                if dst_f16 {
                    self.info.uses_fp16 = true;
                    let half_bits = self.alloc_val(blk, ValType::U32, SsaOp::FConvertF32ToF16Lo(conv));
                    self.write_reg(state, dst, half_bits, None);
                } else {
                    let bits = self.to_u32_bits(blk, conv);
                    self.write_reg(state, dst, bits, None);
                }
            }
            DecodedOp::F2i { dst, src, dst_signed, dst_bits, src_f16, half_lane, abs, neg, round_mode: _ } => {
                let raw = self.eval_src(blk, state, src);
                let mut f = if src_f16 {
                    let shifted = if (half_lane & 1) != 0 {
                        let s16 = self.const_u32(blk, 16);
                        self.alloc_val(blk, ValType::U32, SsaOp::LShr(raw, s16))
                    } else {
                        raw
                    };
                    self.alloc_val(blk, ValType::F32, SsaOp::FConvertF16LoToF32(shifted))
                } else {
                    self.alloc_val(blk, ValType::F32, SsaOp::Bitcast { to: ValType::F32, val: raw })
                };
                if abs {
                    f = self.alloc_val(blk, ValType::F32, SsaOp::FAbs(f));
                }
                if neg {
                    f = self.alloc_val(blk, ValType::F32, SsaOp::FNeg(f));
                }
                let conv = if dst_signed {
                    self.alloc_val(blk, ValType::U32, SsaOp::F32ConvertToS(f))
                } else {
                    self.alloc_val(blk, ValType::U32, SsaOp::F32ConvertToU(f))
                };
                let masked = if dst_bits == 8 {
                    let m = self.const_u32(blk, 0xff);
                    self.alloc_val(blk, ValType::U32, SsaOp::BitAnd(conv, m))
                } else if dst_bits == 16 {
                    let m = self.const_u32(blk, 0xffff);
                    self.alloc_val(blk, ValType::U32, SsaOp::BitAnd(conv, m))
                } else {
                    conv
                };
                self.write_reg(state, dst, masked, None);
            }
            DecodedOp::F2f { dst, src, src_bits, dst_bits, half_lane, sat, abs, neg } => {
                if src_bits == 16 && dst_bits == 32 {
                    let raw = self.eval_src(blk, state, src);
                    let shifted = if (half_lane & 1) != 0 {
                        let s16 = self.const_u32(blk, 16);
                        self.alloc_val(blk, ValType::U32, SsaOp::LShr(raw, s16))
                    } else {
                        raw
                    };
                    let mut f = self.alloc_val(blk, ValType::F32, SsaOp::FConvertF16LoToF32(shifted));
                    if abs {
                        f = self.alloc_val(blk, ValType::F32, SsaOp::FAbs(f));
                    }
                    if neg {
                        f = self.alloc_val(blk, ValType::F32, SsaOp::FNeg(f));
                    }
                    if sat {
                        f = self.saturate_f32(blk, f);
                    }
                    let bits = self.to_u32_bits(blk, f);
                    self.write_reg(state, dst, bits, None);
                } else {
                    return fail("unsupported F2F conversion width");
                }
            }
            DecodedOp::Hadd2 { dst, a, b, sat, f32_out } => {
                if !self.profile.support_float16 {
                    return fail("HADD2 requires Float16 device capability");
                }
                self.info.uses_fp16 = true;
                if f32_out || sat {
                    return fail("HADD2 float32/sat variant");
                }
                let va = self.eval_half_src(blk, state, a);
                let vb = self.eval_half_src(blk, state, b);
                let r = self.alloc_val(blk, ValType::F16x2, SsaOp::HAdd2 { a: va, b: vb });
                let bits = self.alloc_val(blk, ValType::U32, SsaOp::Bitcast { to: ValType::U32, val: r });
                self.write_reg(state, dst, bits, None);
            }
            DecodedOp::Hmul2 { dst, a, b, sat } => {
                if !self.profile.support_float16 {
                    return fail("HMUL2 requires Float16 device capability");
                }
                self.info.uses_fp16 = true;
                if sat {
                    return fail("HMUL2 sat variant");
                }
                let va = self.eval_half_src(blk, state, a);
                let vb = self.eval_half_src(blk, state, b);
                let r = self.alloc_val(blk, ValType::F16x2, SsaOp::HMul2 { a: va, b: vb });
                let bits = self.alloc_val(blk, ValType::U32, SsaOp::Bitcast { to: ValType::U32, val: r });
                self.write_reg(state, dst, bits, None);
            }
            DecodedOp::Hfma2 { dst, a, b, c, sat, relu } => {
                if !self.profile.support_float16 {
                    return fail("HFMA2 requires Float16 device capability");
                }
                self.info.uses_fp16 = true;
                if sat || relu {
                    return fail("HFMA2 sat/relu variant");
                }
                let va = self.eval_half_src(blk, state, a);
                let vb = self.eval_half_src(blk, state, b);
                let vc = self.eval_half_src(blk, state, c);
                let r = self.alloc_val(blk, ValType::F16x2, SsaOp::HFma2 { a: va, b: vb, c: vc });
                let bits = self.alloc_val(blk, ValType::U32, SsaOp::Bitcast { to: ValType::U32, val: r });
                self.write_reg(state, dst, bits, None);
            }
            DecodedOp::Ldc { dst, bank, offset, index_reg, width } => {
                let bank_u = u32::from(bank);
                let base_off = u32::from(offset);
                let count = width.word_count();
                if index_reg.is_none() {
                    self.info.record_cbuf_read(bank_u, base_off, count * 4);
                    for w in 0..count {
                        let byte_off = base_off + w * 4;
                        let v = if let Some(imm) = self.snapshot.lookup_replacement(bank_u, byte_off) {
                            self.const_u32(blk, imm)
                        } else {
                            self.alloc_val(blk, ValType::U32, SsaOp::ReadCbuf { bank: bank_u, word_offset: byte_off / 4 })
                        };
                        let ptr = (w == 0 && width == MemWidth::B64).then_some(PtrOrigin {
                            bank: bank_u,
                            cbuf_offset: base_off,
                            byte_offset: self.zero_u32,
                        });
                        self.write_reg(state, dst.saturating_add(w as u8), v, ptr);
                    }
                } else {
                    self.uses_dynamic_cbuf = true;
                    self.info.uses_cbuf_indirect = true;
                    self.info.record_cbuf_read(bank_u, base_off, 64);
                    let idx_val = state.r[index_reg.unwrap() as usize];
                    let base_v = self.const_u32(blk, base_off);
                    let byte_idx = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(idx_val, base_v));
                    for w in 0..count {
                        let w_off = if w == 0 {
                            byte_idx
                        } else {
                            let add_b = self.const_u32(blk, w * 4);
                            self.alloc_val(blk, ValType::U32, SsaOp::IAdd(byte_idx, add_b))
                        };
                        let v = self.alloc_val(blk, ValType::U32, SsaOp::ReadCbufDynamic { bank: bank_u, byte_offset: w_off });
                        self.write_reg(state, dst.saturating_add(w as u8), v, None);
                    }
                }
            }
            DecodedOp::Uldc { dst_ur, bank, offset, width } => {
                let bank_u = u32::from(bank);
                let base_off = u32::from(offset);
                let count = width.word_count();
                self.info.record_cbuf_read(bank_u, base_off, count * 4);
                for w in 0..count {
                    let byte_off = base_off + w * 4;
                    let v = if let Some(imm) = self.snapshot.lookup_replacement(bank_u, byte_off) {
                        self.const_u32(blk, imm)
                    } else {
                        self.alloc_val(blk, ValType::U32, SsaOp::ReadCbuf { bank: bank_u, word_offset: byte_off / 4 })
                    };
                    let ptr = (w == 0 && width == MemWidth::B64).then_some(PtrOrigin {
                        bank: bank_u,
                        cbuf_offset: base_off,
                        byte_offset: self.zero_u32,
                    });
                    self.write_ureg(state, dst_ur.saturating_add(w as u8), v, ptr);
                }
            }
            DecodedOp::Ldg { dst, base_reg, uniform_reg, imm_offset, width, reg_64bit: _, coherent, descriptor_mode } => {
                if descriptor_mode {
                    return fail("descriptor-indexed LDG unimplemented");
                }
                if coherent {
                    self.info.requires_coherent_global_memory = true;
                }
                let Some((ssbo_idx, byte_addr)) =
                    self.resolve_global_ptr(blk, state, base_reg, uniform_reg, imm_offset, false, width)
                else {
                    return fail("unresolved global memory pointer for LDG");
                };
                self.info.uses_global_memory = true;
                self.lower_ssbo_load(blk, state, ssbo_idx, byte_addr, dst, width);
            }
            DecodedOp::Stg { src_reg, base_reg, uniform_reg, imm_offset, width, reg_64bit: _, descriptor_mode } => {
                if descriptor_mode {
                    return fail("descriptor-indexed STG unimplemented");
                }
                let Some((ssbo_idx, byte_addr)) =
                    self.resolve_global_ptr(blk, state, base_reg, uniform_reg, imm_offset, true, width)
                else {
                    return fail("unresolved global memory pointer for STG");
                };
                self.info.uses_global_memory = true;
                self.info.stores_global_memory = true;
                if matches!(width, MemWidth::U8 | MemWidth::S8 | MemWidth::U16 | MemWidth::S16) {
                    return fail("sub-word STG atomic/masked store");
                }
                let two = self.const_u32(blk, 2);
                let word_base = self.alloc_val(blk, ValType::U32, SsaOp::LShr(byte_addr, two));
                let words = width.word_count();
                for w in 0..words {
                    let w_idx = if w == 0 {
                        word_base
                    } else {
                        let cw = self.const_u32(blk, w);
                        self.alloc_val(blk, ValType::U32, SsaOp::IAdd(word_base, cw))
                    };
                    let val = if src_reg == 255 {
                        self.zero_u32
                    } else {
                        state.r[src_reg.saturating_add(w as u8) as usize]
                    };
                    self.emit_void(blk, SsaOp::StoreSsboWord { ssbo_idx, word_idx: w_idx, val });
                }
            }
            DecodedOp::Lds { dst, base_reg, uniform_reg, imm_offset, stride, width } => {
                let Some(byte_addr) = self.resolve_shared_addr(blk, state, base_reg, uniform_reg, imm_offset, stride) else {
                    return fail("invalid shared address");
                };
                let max_bytes = imm_offset.max(0) as u32 + width.word_count() * 4;
                if max_bytes > self.shared_bytes_needed {
                    self.shared_bytes_needed = (max_bytes + 3) & !3;
                }
                let two = self.const_u32(blk, 2);
                let word_base = self.alloc_val(blk, ValType::U32, SsaOp::LShr(byte_addr, two));
                for w in 0..width.word_count() {
                    let w_idx = if w == 0 {
                        word_base
                    } else {
                        let cw = self.const_u32(blk, w);
                        self.alloc_val(blk, ValType::U32, SsaOp::IAdd(word_base, cw))
                    };
                    let loaded = self.alloc_val(blk, ValType::U32, SsaOp::LoadSharedWord { word_idx: w_idx });
                    let final_val = self.extract_subword(blk, loaded, byte_addr, width);
                    self.write_reg(state, dst.saturating_add(w as u8), final_val, None);
                }
            }
            DecodedOp::Sts { src_reg, base_reg, uniform_reg, imm_offset, stride, width } => {
                if matches!(width, MemWidth::U8 | MemWidth::S8 | MemWidth::U16 | MemWidth::S16) {
                    return fail("sub-word STS store");
                }
                let Some(byte_addr) = self.resolve_shared_addr(blk, state, base_reg, uniform_reg, imm_offset, stride) else {
                    return fail("invalid shared address");
                };
                let max_bytes = imm_offset.max(0) as u32 + width.word_count() * 4;
                if max_bytes > self.shared_bytes_needed {
                    self.shared_bytes_needed = (max_bytes + 3) & !3;
                }
                let two = self.const_u32(blk, 2);
                let word_base = self.alloc_val(blk, ValType::U32, SsaOp::LShr(byte_addr, two));
                for w in 0..width.word_count() {
                    let w_idx = if w == 0 {
                        word_base
                    } else {
                        let cw = self.const_u32(blk, w);
                        self.alloc_val(blk, ValType::U32, SsaOp::IAdd(word_base, cw))
                    };
                    let val = if src_reg == 255 {
                        self.zero_u32
                    } else {
                        state.r[src_reg.saturating_add(w as u8) as usize]
                    };
                    self.emit_void(blk, SsaOp::StoreSharedWord { word_idx: w_idx, val });
                }
            }
            DecodedOp::Ldl { dst, base_reg, offset, width } => {
                if matches!(width, MemWidth::U8 | MemWidth::S8 | MemWidth::U16 | MemWidth::S16) {
                    return fail("sub-word LDL");
                }
                self.info.uses_local_memory = true;
                let base_v = if base_reg == 255 { self.zero_u32 } else { state.r[base_reg as usize] };
                let off_v = self.const_u32(blk, offset);
                let byte_addr = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(base_v, off_v));
                let end = offset + width.word_count() * 4;
                if end > self.local_bytes_needed {
                    self.local_bytes_needed = (end + 3) & !3;
                }
                let two = self.const_u32(blk, 2);
                let word_base = self.alloc_val(blk, ValType::U32, SsaOp::LShr(byte_addr, two));
                for w in 0..width.word_count() {
                    let w_idx = if w == 0 {
                        word_base
                    } else {
                        let cw = self.const_u32(blk, w);
                        self.alloc_val(blk, ValType::U32, SsaOp::IAdd(word_base, cw))
                    };
                    let v = self.alloc_val(blk, ValType::U32, SsaOp::LoadLocalWord { word_idx: w_idx });
                    self.write_reg(state, dst.saturating_add(w as u8), v, None);
                }
            }
            DecodedOp::Stl { src_reg, base_reg, offset, width } => {
                if matches!(width, MemWidth::U8 | MemWidth::S8 | MemWidth::U16 | MemWidth::S16) {
                    return fail("sub-word STL");
                }
                self.info.uses_local_memory = true;
                let base_v = if base_reg == 255 { self.zero_u32 } else { state.r[base_reg as usize] };
                let off_v = self.const_u32(blk, offset);
                let byte_addr = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(base_v, off_v));
                let end = offset + width.word_count() * 4;
                if end > self.local_bytes_needed {
                    self.local_bytes_needed = (end + 3) & !3;
                }
                let two = self.const_u32(blk, 2);
                let word_base = self.alloc_val(blk, ValType::U32, SsaOp::LShr(byte_addr, two));
                for w in 0..width.word_count() {
                    let w_idx = if w == 0 {
                        word_base
                    } else {
                        let cw = self.const_u32(blk, w);
                        self.alloc_val(blk, ValType::U32, SsaOp::IAdd(word_base, cw))
                    };
                    let val = if src_reg == 255 {
                        self.zero_u32
                    } else {
                        state.r[src_reg.saturating_add(w as u8) as usize]
                    };
                    self.emit_void(blk, SsaOp::StoreLocalWord { word_idx: w_idx, val });
                }
            }
            DecodedOp::Atoms { dst, base_reg, imm_offset, src_a, op: atom_op, is_64bit } => {
                if is_64bit || matches!(atom_op, AtomicOp::Inc | AtomicOp::Dec) {
                    return fail("64-bit or inc/dec ATOMS");
                }
                let Some(byte_addr) = self.resolve_shared_addr(blk, state, base_reg, None, imm_offset, 0) else {
                    return fail("invalid ATOMS address");
                };
                let max_bytes = imm_offset.max(0) as u32 + 4;
                if max_bytes > self.shared_bytes_needed {
                    self.shared_bytes_needed = (max_bytes + 3) & !3;
                }
                let two = self.const_u32(blk, 2);
                let word_idx = self.alloc_val(blk, ValType::U32, SsaOp::LShr(byte_addr, two));
                let data = if src_a == 255 { self.zero_u32 } else { state.r[src_a as usize] };
                let res = self.alloc_val(blk, ValType::U32, SsaOp::AtomicSharedWord { word_idx, op: atom_op, data });
                self.write_reg(state, dst, res, None);
            }
            DecodedOp::Atomg { dst, base_reg, uniform_reg, imm_offset, src_a, op: atom_op } => {
                let Some((ssbo_idx, byte_addr)) =
                    self.resolve_global_ptr(blk, state, base_reg, uniform_reg, imm_offset, true, MemWidth::B32)
                else {
                    return fail("unresolved ATOMG base pointer");
                };
                self.info.uses_global_memory = true;
                self.info.stores_global_memory = true;
                let two = self.const_u32(blk, 2);
                let word_idx = self.alloc_val(blk, ValType::U32, SsaOp::LShr(byte_addr, two));
                let data = if src_a == 255 { self.zero_u32 } else { state.r[src_a as usize] };
                let res = self.alloc_val(blk, ValType::U32, SsaOp::AtomicSsboWord { ssbo_idx, word_idx, op: atom_op, data });
                if let Some(d) = dst {
                    self.write_reg(state, d, res, None);
                }
            }
            DecodedOp::Shfl { dst, pred_out, src, lane, mask: _, mode } => {
                if !self.profile.support_vote && self.profile.supported_subgroup_stages == 0 {
                    return fail("subgroup shuffle unsupported");
                }
                self.info.uses_subgroup_shuffles = true;
                let val = if src == 255 { self.zero_u32 } else { state.r[src as usize] };
                let lane_val = self.eval_src(blk, state, lane);
                let res = self.alloc_val(blk, ValType::U32, SsaOp::SubgroupShuffle { mode, val, lane_or_delta: lane_val });
                self.write_reg(state, dst, res, None);
                if pred_out < 7 {
                    state.p[pred_out as usize] = self.true_bool;
                }
            }
            DecodedOp::Vote { dst_reg, dst_uniform, pred_out, src_pred, mode } => {
                if !self.profile.support_vote {
                    return fail("subgroup vote unsupported");
                }
                self.info.uses_subgroup_vote = true;
                let p = self.read_pred(blk, state, src_pred);
                let ballot = self.alloc_val(blk, ValType::U32, SsaOp::SubgroupBallot { pred: p });
                let vp = self.alloc_val(blk, ValType::Bool, SsaOp::SubgroupVote { mode, pred: p });
                if dst_uniform {
                    self.write_ureg(state, dst_reg, ballot, None);
                    if pred_out < 7 {
                        state.up[pred_out as usize] = vp;
                    }
                } else {
                    self.write_reg(state, dst_reg, ballot, None);
                    if pred_out < 7 {
                        state.p[pred_out as usize] = vp;
                    }
                }
            }
            DecodedOp::BarSync { defer_blocking: _ } => {
                self.emit_void(blk, SsaOp::ControlBarrier);
            }
            DecodedOp::Membar { scope } => {
                self.emit_void(blk, SsaOp::MemoryBarrier { scope });
            }
            DecodedOp::Hmma { dst, ra, rb, rc, depth } => {
                if self.stage != Stage::Compute
                    || self.workgroup_size[0] == 0
                    || (self.workgroup_size[0] % 32) != 0
                    || self.info.requires_coherent_global_memory
                    || self.info.requires_coherent_image_memory
                {
                    return fail("cooperative matrix requires compute shader with full 32-lane workgroups");
                }
                if let Err(reason) = self.coop_config.validate(depth) {
                    return fail(reason);
                }
                self.info.uses_fp16 = true;
                self.info.uses_cooperative_matrix = true;
                self.info.requires_full_subgroups = true;
                let a_len = (depth / 4) as usize;
                let b_len = (depth / 8) as usize;
                let mut a_vals = [self.zero_u32; 4];
                for (i, slot) in a_vals.iter_mut().take(a_len).enumerate() {
                    let r = ra.saturating_add(i as u8);
                    *slot = if r >= 255 { self.zero_u32 } else { state.r[r as usize] };
                }
                let mut b_vals = [self.zero_u32; 2];
                for (i, slot) in b_vals.iter_mut().take(b_len).enumerate() {
                    let r = rb.saturating_add(i as u8);
                    *slot = if r >= 255 { self.zero_u32 } else { state.r[r as usize] };
                }
                let c_vals = if rc == 255 {
                    [self.zero_u32; 2]
                } else {
                    [state.r[rc as usize], state.r[rc.saturating_add(1) as usize]]
                };
                let hmma = self.alloc_val(
                    blk,
                    ValType::U64,
                    SsaOp::CoopHmma {
                        depth,
                        a: a_vals,
                        a_len,
                        b: b_vals,
                        b_len,
                        c: c_vals,
                    },
                );
                let d0 = self.alloc_val(blk, ValType::U32, SsaOp::CoopHmmaExtract { hmma, word_idx: 0 });
                let d1 = self.alloc_val(blk, ValType::U32, SsaOp::CoopHmmaExtract { hmma, word_idx: 1 });
                self.write_reg(state, dst, d0, None);
                self.write_reg(state, dst.saturating_add(1), d1, None);
            }
            DecodedOp::Unsupported { raw_opcode, reason } => {
                return Err(FallbackReason {
                    offset: off,
                    opcode: raw_opcode,
                    reason,
                });
            }
        }
        Ok(())
    }

    fn lower_special(
        &mut self,
        blk: usize,
        special: S2rSpecial,
        offset: u32,
        raw_opcode: u32,
    ) -> Result<ValueId, FallbackReason> {
        match special {
            S2rSpecial::LaneId => {
                self.info.uses_subgroup_invocation_id = true;
            }
            S2rSpecial::TidX | S2rSpecial::TidY | S2rSpecial::TidZ => {
                self.info.uses_local_invocation_id = true;
            }
            S2rSpecial::CtaIdX | S2rSpecial::CtaIdY | S2rSpecial::CtaIdZ => {
                self.info.uses_workgroup_id = true;
            }
            S2rSpecial::InvocationId => {
                self.info.uses_invocation_id = true;
            }
            S2rSpecial::LaneMaskEq
            | S2rSpecial::LaneMaskLt
            | S2rSpecial::LaneMaskLe
            | S2rSpecial::LaneMaskGt
            | S2rSpecial::LaneMaskGe => {
                self.info.uses_subgroup_mask = true;
            }
            S2rSpecial::VariableRate => return Ok(self.const_u32(blk, 0x0001_0100)),
            S2rSpecial::Unsupported(_) => {
                return Err(FallbackReason {
                    offset,
                    opcode: raw_opcode,
                    reason: "unsupported special register",
                });
            }
        }
        Ok(self.alloc_val(blk, ValType::U32, SsaOp::ReadBuiltin(special)))
    }

    fn select_u32_bool(&mut self, blk: usize, cond: ValueId) -> ValueId {
        self.alloc_val(
            blk,
            ValType::U32,
            SsaOp::Select {
                cond,
                t: self.one_u32,
                f: self.zero_u32,
            },
        )
    }

    fn propagate_add_ptr(
        &mut self,
        blk: usize,
        state: &RegState,
        a: SrcOperand,
        b: SrcOperand,
        c: SrcOperand,
        neg_a: bool,
        neg_b: bool,
        neg_c: bool,
    ) -> Option<PtrOrigin> {
        let candidates = [(a, b, c, neg_a), (b, a, c, neg_b), (c, a, b, neg_c)];
        for (base_op, other1, other2, is_neg) in candidates {
            if is_neg {
                continue;
            }
            if let Some(orig) = self.src_ptr(state, base_op) {
                let o1 = self.eval_src(blk, state, other1);
                let o2 = self.eval_src(blk, state, other2);
                let add12 = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(o1, o2));
                let new_off = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(orig.byte_offset, add12));
                return Some(PtrOrigin {
                    byte_offset: new_off,
                    ..orig
                });
            }
        }
        None
    }

    fn resolve_global_ptr(
        &mut self,
        blk: usize,
        state: &RegState,
        base_reg: u8,
        uniform_reg: Option<u8>,
        imm_offset: i32,
        is_written: bool,
        width: MemWidth,
    ) -> Option<(usize, ValueId)> {
        let (orig, reg_add) = if let Some(ur) = uniform_reg {
            let orig = state.ur_ptr[ur as usize]?;
            let add = if base_reg == 255 {
                self.zero_u32
            } else {
                state.r[base_reg as usize]
            };
            (orig, add)
        } else {
            if base_reg == 255 {
                return None;
            }
            let orig = state.r_ptr[base_reg as usize]?;
            (orig, self.zero_u32)
        };

        if self.snapshot.is_mapped_storage_ptr(orig.bank, orig.cbuf_offset) == Some(false) {
            return None;
        }
        self.info.record_cbuf_read(orig.bank, orig.cbuf_offset, 8);
        let type_mask = match width {
            MemWidth::U8 | MemWidth::S8 => 1u32 << 1,
            MemWidth::U16 | MemWidth::S16 => 1u32 << 1,
            MemWidth::B64 => 1u32 << 3,
            _ => 1u32 << 2,
        };
        let ssbo_idx = self.info.record_ssbo(orig.bank, orig.cbuf_offset, is_written, type_mask)?;
        let off1 = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(orig.byte_offset, reg_add));
        let total = if imm_offset == 0 {
            off1
        } else {
            let imm_v = self.const_u32(blk, imm_offset as u32);
            self.alloc_val(blk, ValType::U32, SsaOp::IAdd(off1, imm_v))
        };
        Some((ssbo_idx, total))
    }

    fn resolve_shared_addr(
        &mut self,
        blk: usize,
        state: &RegState,
        base_reg: u8,
        uniform_reg: Option<u8>,
        imm_offset: i32,
        stride: u8,
    ) -> Option<ValueId> {
        let mut addr = if base_reg == 255 {
            self.zero_u32
        } else if stride != 0 {
            let sh = self.const_u32(blk, u32::from(stride) + 1);
            self.alloc_val(blk, ValType::U32, SsaOp::Shl(state.r[base_reg as usize], sh))
        } else {
            state.r[base_reg as usize]
        };
        if let Some(ur) = uniform_reg {
            let uval = state.ur[ur as usize];
            addr = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(addr, uval));
        }
        if imm_offset != 0 {
            let iv = self.const_u32(blk, imm_offset as u32);
            addr = self.alloc_val(blk, ValType::U32, SsaOp::IAdd(addr, iv));
        }
        Some(addr)
    }

    fn lower_ssbo_load(
        &mut self,
        blk: usize,
        state: &mut RegState,
        ssbo_idx: usize,
        byte_addr: ValueId,
        dst: u8,
        width: MemWidth,
    ) {
        let two = self.const_u32(blk, 2);
        let word_base = self.alloc_val(blk, ValType::U32, SsaOp::LShr(byte_addr, two));
        let words = width.word_count();
        for w in 0..words {
            let w_idx = if w == 0 {
                word_base
            } else {
                let cw = self.const_u32(blk, w);
                self.alloc_val(blk, ValType::U32, SsaOp::IAdd(word_base, cw))
            };
            let loaded = self.alloc_val(blk, ValType::U32, SsaOp::LoadSsboWord { ssbo_idx, word_idx: w_idx });
            let final_val = self.extract_subword(blk, loaded, byte_addr, width);
            self.write_reg(state, dst.saturating_add(w as u8), final_val, None);
        }
    }

    fn extract_subword(&mut self, blk: usize, word: ValueId, byte_addr: ValueId, width: MemWidth) -> ValueId {
        match width {
            MemWidth::B32 | MemWidth::B64 | MemWidth::B128 => word,
            MemWidth::U8 | MemWidth::S8 => {
                let m3 = self.const_u32(blk, 3);
                let byte_pos = self.alloc_val(blk, ValType::U32, SsaOp::BitAnd(byte_addr, m3));
                let c8 = self.const_u32(blk, 3);
                let bit_shift = self.alloc_val(blk, ValType::U32, SsaOp::Shl(byte_pos, c8));
                let shifted = self.alloc_val(blk, ValType::U32, SsaOp::LShr(word, bit_shift));
                if width == MemWidth::U8 {
                    let m = self.const_u32(blk, 0xff);
                    self.alloc_val(blk, ValType::U32, SsaOp::BitAnd(shifted, m))
                } else {
                    self.alloc_val(blk, ValType::U32, SsaOp::SBitFieldExtract { val: shifted, offset: 0, count: 8 })
                }
            }
            MemWidth::U16 | MemWidth::S16 => {
                let m2 = self.const_u32(blk, 2);
                let byte_pos = self.alloc_val(blk, ValType::U32, SsaOp::BitAnd(byte_addr, m2));
                let c3 = self.const_u32(blk, 3);
                let bit_shift = self.alloc_val(blk, ValType::U32, SsaOp::Shl(byte_pos, c3));
                let shifted = self.alloc_val(blk, ValType::U32, SsaOp::LShr(word, bit_shift));
                if width == MemWidth::U16 {
                    let m = self.const_u32(blk, 0xffff);
                    self.alloc_val(blk, ValType::U32, SsaOp::BitAnd(shifted, m))
                } else {
                    self.alloc_val(blk, ValType::U32, SsaOp::SBitFieldExtract { val: shifted, offset: 0, count: 16 })
                }
            }
        }
    }

    fn lower_lop3_lut(&mut self, blk: usize, a: ValueId, b: ValueId, c: ValueId, lut: u8) -> ValueId {
        match lut {
            0x00 => return self.zero_u32,
            0xff => return self.const_u32(blk, u32::MAX),
            0xf0 => return a,
            0xcc => return b,
            0xaa => return c,
            0x0f => return self.alloc_val(blk, ValType::U32, SsaOp::BitNot(a)),
            0x33 => return self.alloc_val(blk, ValType::U32, SsaOp::BitNot(b)),
            0x55 => return self.alloc_val(blk, ValType::U32, SsaOp::BitNot(c)),
            0xc0 => return self.alloc_val(blk, ValType::U32, SsaOp::BitAnd(a, b)),
            0xfc => return self.alloc_val(blk, ValType::U32, SsaOp::BitOr(a, b)),
            0x3c => return self.alloc_val(blk, ValType::U32, SsaOp::BitXor(a, b)),
            0x66 => return self.alloc_val(blk, ValType::U32, SsaOp::BitXor(b, c)),
            0x5a => return self.alloc_val(blk, ValType::U32, SsaOp::BitXor(a, c)),
            0x96 => {
                let ab = self.alloc_val(blk, ValType::U32, SsaOp::BitXor(a, b));
                return self.alloc_val(blk, ValType::U32, SsaOp::BitXor(ab, c));
            }
            0xf8 => {
                let bc = self.alloc_val(blk, ValType::U32, SsaOp::BitAnd(b, c));
                return self.alloc_val(blk, ValType::U32, SsaOp::BitOr(a, bc));
            }
            0xea => {
                let ab = self.alloc_val(blk, ValType::U32, SsaOp::BitAnd(a, b));
                return self.alloc_val(blk, ValType::U32, SsaOp::BitOr(ab, c));
            }
            0x80 => {
                let ab = self.alloc_val(blk, ValType::U32, SsaOp::BitAnd(a, b));
                return self.alloc_val(blk, ValType::U32, SsaOp::BitAnd(ab, c));
            }
            0xfe => {
                let ab = self.alloc_val(blk, ValType::U32, SsaOp::BitOr(a, b));
                return self.alloc_val(blk, ValType::U32, SsaOp::BitOr(ab, c));
            }
            _ => {}
        }
        let na = self.alloc_val(blk, ValType::U32, SsaOp::BitNot(a));
        let nb = self.alloc_val(blk, ValType::U32, SsaOp::BitNot(b));
        let nc = self.alloc_val(blk, ValType::U32, SsaOp::BitNot(c));
        let mut acc = self.zero_u32;
        let mut first = true;
        for row in 0..8u8 {
            if (lut & (1 << row)) == 0 {
                continue;
            }
            let ta = if (row & 4) != 0 { a } else { na };
            let tb = if (row & 2) != 0 { b } else { nb };
            let tc = if (row & 1) != 0 { c } else { nc };
            let ab = self.alloc_val(blk, ValType::U32, SsaOp::BitAnd(ta, tb));
            let term = self.alloc_val(blk, ValType::U32, SsaOp::BitAnd(ab, tc));
            if first {
                acc = term;
                first = false;
            } else {
                acc = self.alloc_val(blk, ValType::U32, SsaOp::BitOr(acc, term));
            }
        }
        acc
    }

    fn lower_shf(
        &mut self,
        blk: usize,
        a: ValueId,
        shift_raw: ValueId,
        c: ValueId,
        right: bool,
        hi: bool,
        signed: bool,
        wrap: bool,
    ) -> ValueId {
        let mask31 = self.const_u32(blk, 31);
        let c32 = self.const_u32(blk, 32);
        let mask63 = self.const_u32(blk, 63);
        let shift = if wrap {
            self.alloc_val(blk, ValType::U32, SsaOp::BitAnd(shift_raw, mask31))
        } else if hi {
            self.alloc_val(blk, ValType::U32, SsaOp::Glsl2(GlslOp::UMin, shift_raw, c32))
        } else {
            self.alloc_val(blk, ValType::U32, SsaOp::Glsl2(GlslOp::UMin, shift_raw, mask63))
        };

        if right {
            if hi {
                // funnel right by shift on c (high 32) -> if shift == 0: c; else (c >> shift)
                if signed {
                    self.alloc_val(blk, ValType::U32, SsaOp::AShr(c, shift))
                } else {
                    self.alloc_val(blk, ValType::U32, SsaOp::LShr(c, shift))
                }
            } else {
                let is_zero = self.alloc_val(blk, ValType::Bool, SsaOp::IntCmp { cmp: IntCmp::Eq, signed: false, a: shift, b: self.zero_u32 });
                let lo_shr = self.alloc_val(blk, ValType::U32, SsaOp::LShr(a, shift));
                let inv_sh = self.alloc_val(blk, ValType::U32, SsaOp::ISub(c32, shift));
                let c_shl = self.alloc_val(blk, ValType::U32, SsaOp::Shl(c, inv_sh));
                let comb = self.alloc_val(blk, ValType::U32, SsaOp::BitOr(lo_shr, c_shl));
                self.alloc_val(blk, ValType::U32, SsaOp::Select { cond: is_zero, t: a, f: comb })
            }
        } else if !hi {
            self.alloc_val(blk, ValType::U32, SsaOp::Shl(a, shift))
        } else {
            let is_zero = self.alloc_val(blk, ValType::Bool, SsaOp::IntCmp { cmp: IntCmp::Eq, signed: false, a: shift, b: self.zero_u32 });
            let c_shl = self.alloc_val(blk, ValType::U32, SsaOp::Shl(c, shift));
            let inv_sh = self.alloc_val(blk, ValType::U32, SsaOp::ISub(c32, shift));
            let a_shr = self.alloc_val(blk, ValType::U32, SsaOp::LShr(a, inv_sh));
            let comb = self.alloc_val(blk, ValType::U32, SsaOp::BitOr(c_shl, a_shr));
            self.alloc_val(blk, ValType::U32, SsaOp::Select { cond: is_zero, t: c, f: comb })
        }
    }
}

fn patch_phi(block: &mut SsaBlock, phi_id: ValueId, entries: Vec<(usize, ValueId)>) {
    for insn in &mut block.insns {
        if insn.id == phi_id {
            insn.op = SsaOp::Phi(entries);
            break;
        }
    }
}

#[derive(Default)]
struct ModifiedSet {
    r: BTreeSet<u8>,
    ur: BTreeSet<u8>,
    p: BTreeSet<u8>,
}

fn collect_modified_regs(nodes: &[CfgNode], decoded: &[DecodedInsn]) -> ModifiedSet {
    let mut set = ModifiedSet::default();
    fn visit(node: &CfgNode, decoded: &[DecodedInsn], set: &mut ModifiedSet) {
        match node {
            CfgNode::Range { start, end_excl } => {
                for insn in &decoded[*start..*end_excl] {
                    match insn.op {
                        DecodedOp::Mov { dst, .. }
                        | DecodedOp::Sel { dst, .. }
                        | DecodedOp::Cs2rZero { dst }
                        | DecodedOp::S2r { dst, .. }
                        | DecodedOp::Imnmx { dst, .. }
                        | DecodedOp::Iabs { dst, .. }
                        | DecodedOp::Popc { dst, .. }
                        | DecodedOp::Brev { dst, .. }
                        | DecodedOp::Flo { dst, .. }
                        | DecodedOp::Sgxt { dst, .. }
                        | DecodedOp::Prmt { dst, .. }
                        | DecodedOp::Fadd { dst, .. }
                        | DecodedOp::Fmul { dst, .. }
                        | DecodedOp::Ffma { dst, .. }
                        | DecodedOp::Fmnmx { dst, .. }
                        | DecodedOp::Fsel { dst, .. }
                        | DecodedOp::Mufu { dst, .. }
                        | DecodedOp::I2f { dst, .. }
                        | DecodedOp::F2i { dst, .. }
                        | DecodedOp::F2f { dst, .. }
                        | DecodedOp::Hadd2 { dst, .. }
                        | DecodedOp::Hmul2 { dst, .. }
                        | DecodedOp::Hfma2 { dst, .. }
                        | DecodedOp::Atoms { dst, .. }
                        | DecodedOp::Shfl { dst, .. } => {
                            if dst < 255 {
                                set.r.insert(dst);
                            }
                        }
                        DecodedOp::Iadd3 { dst, carry_out0, carry_out1, .. } => {
                            if dst < 255 { set.r.insert(dst); }
                            if carry_out0 < 7 { set.p.insert(carry_out0); }
                            if carry_out1 < 7 { set.p.insert(carry_out1); }
                        }
                        DecodedOp::Lea { dst, carry_out, .. } => {
                            if dst < 255 { set.r.insert(dst); }
                            if carry_out < 7 { set.p.insert(carry_out); }
                        }
                        DecodedOp::Lop3 { dst, pred_out, .. } => {
                            if dst < 255 { set.r.insert(dst); }
                            if pred_out < 7 { set.p.insert(pred_out); }
                        }
                        DecodedOp::Shf { dst, .. } => {
                            if dst < 255 { set.r.insert(dst); }
                        }
                        DecodedOp::Imad { dst, wide, .. } => {
                            if dst < 255 { set.r.insert(dst); }
                            if wide && dst.saturating_add(1) < 255 {
                                set.r.insert(dst + 1);
                            }
                        }
                        DecodedOp::Isetp { dst_p, dst_q, .. } | DecodedOp::Fsetp { dst_p, dst_q, .. } => {
                            if dst_p < 7 { set.p.insert(dst_p); }
                            if dst_q < 7 { set.p.insert(dst_q); }
                        }
                        DecodedOp::Umov { dst_ur, .. }
                        | DecodedOp::S2ur { dst_ur, .. }
                        | DecodedOp::R2ur { dst_ur, .. }
                        | DecodedOp::Ushf { dst_ur, .. }
                        | DecodedOp::Ulea { dst_ur, .. }
                        | DecodedOp::Uflo { dst_ur, .. } => {
                            if dst_ur < 63 { set.ur.insert(dst_ur); }
                        }
                        DecodedOp::Uiadd3 { dst_ur, .. } | DecodedOp::Ulop3 { dst_ur, .. } => {
                            if dst_ur < 63 { set.ur.insert(dst_ur); }
                        }
                        DecodedOp::Uimad { dst_ur, wide, .. } => {
                            if dst_ur < 63 { set.ur.insert(dst_ur); }
                            if wide && dst_ur.saturating_add(1) < 63 {
                                set.ur.insert(dst_ur + 1);
                            }
                        }
                        DecodedOp::Ldc { dst, width, .. }
                        | DecodedOp::Ldg { dst, width, .. }
                        | DecodedOp::Lds { dst, width, .. }
                        | DecodedOp::Ldl { dst, width, .. } => {
                            for w in 0..width.word_count() {
                                if dst.saturating_add(w as u8) < 255 {
                                    set.r.insert(dst + w as u8);
                                }
                            }
                        }
                        DecodedOp::Uldc { dst_ur, width, .. } => {
                            for w in 0..width.word_count() {
                                if dst_ur.saturating_add(w as u8) < 63 {
                                    set.ur.insert(dst_ur + w as u8);
                                }
                            }
                        }
                        DecodedOp::Atomg { dst: Some(d), .. } => {
                            if d < 255 { set.r.insert(d); }
                        }
                        DecodedOp::Vote { dst_reg, dst_uniform, pred_out, .. } => {
                            if dst_uniform {
                                if dst_reg < 63 { set.ur.insert(dst_reg); }
                            } else {
                                if dst_reg < 255 { set.r.insert(dst_reg); }
                                if pred_out < 7 { set.p.insert(pred_out); }
                            }
                        }
                        DecodedOp::Hmma { dst, .. } => {
                            if dst < 255 { set.r.insert(dst); }
                            if dst.saturating_add(1) < 255 { set.r.insert(dst + 1); }
                        }
                        _ => {}
                    }
                }
            }
            CfgNode::If { then_nodes, else_nodes, .. } => {
                for n in then_nodes { visit(n, decoded, set); }
                for n in else_nodes { visit(n, decoded, set); }
            }
            CfgNode::Loop { body_nodes, .. } => {
                for n in body_nodes { visit(n, decoded, set); }
            }
        }
    }
    for n in nodes {
        visit(n, decoded, &mut set);
    }
    set
}

fn find_reachable(decoded: &[DecodedInsn]) -> Vec<bool> {
    let n = decoded.len();
    let mut reachable = vec![false; n];
    if n == 0 {
        return reachable;
    }
    let mut q = VecDeque::new();
    reachable[0] = true;
    q.push_back(0usize);
    while let Some(idx) = q.pop_front() {
        let insn = decoded[idx];
        match insn.op {
            DecodedOp::Exit => {
                if !insn.guard.is_always_true() && idx + 1 < n && !reachable[idx + 1] {
                    reachable[idx + 1] = true;
                    q.push_back(idx + 1);
                }
            }
            DecodedOp::Bra { target_index, extra_pred } => {
                if target_index >= 0 && (target_index as usize) < n {
                    let t = target_index as usize;
                    if !reachable[t] {
                        reachable[t] = true;
                        q.push_back(t);
                    }
                }
                let unconditional = insn.guard.is_always_true() && extra_pred.is_always_true();
                if !unconditional && idx + 1 < n && !reachable[idx + 1] {
                    reachable[idx + 1] = true;
                    q.push_back(idx + 1);
                }
            }
            _ => {
                if idx + 1 < n && !reachable[idx + 1] {
                    reachable[idx + 1] = true;
                    q.push_back(idx + 1);
                }
            }
        }
    }
    reachable
}

/// Builds a clean structured region tree over linear range `[start, end_excl)`.
fn structure_range(
    decoded: &[DecodedInsn],
    start: usize,
    end_excl: usize,
) -> Result<Vec<CfgNode>, FallbackReason> {
    let mut nodes = Vec::new();
    let mut cur = start;
    let mut range_start = start;

    while cur < end_excl {
        let insn = decoded[cur];
        match insn.op {
            DecodedOp::Unsupported { raw_opcode, reason } => {
                return Err(FallbackReason {
                    offset: (insn.index * 16) as u32,
                    opcode: raw_opcode,
                    reason,
                });
            }
            DecodedOp::Bra { target_index, extra_pred } => {
                if target_index < 0 || (target_index as usize) >= decoded.len() {
                    return Err(FallbackReason {
                        offset: (cur * 16) as u32,
                        opcode: insn.raw_opcode,
                        reason: "branch target out of range",
                    });
                }
                let t = target_index as usize;
                let effective_guard = match (insn.guard.is_always_true(), extra_pred.is_always_true()) {
                    (true, true) => PredGuard::always_true(),
                    (false, true) => insn.guard,
                    (true, false) => extra_pred,
                    (false, false) => {
                        return Err(FallbackReason {
                            offset: (cur * 16) as u32,
                            opcode: insn.raw_opcode,
                            reason: "dual-predicated branch",
                        });
                    }
                };
                if t == cur && effective_guard.is_always_true() {
                    // Self-loop idle trap / post-compute spin treated as terminator.
                    nodes.push(CfgNode::Range { start: range_start, end_excl: cur });
                    return Ok(nodes);
                }
                if t < cur && t >= start {
                    // Structured do-while loop: flush range before `t`, make `[t..cur]` loop body!
                    if t > range_start {
                        nodes.push(CfgNode::Range { start: range_start, end_excl: t });
                    }
                    // Rebuild body excluding already-collected earlier nodes inside `[t..cur]`
                    // only when `t >= range_start`. If `t < range_start` (backedge crosses earlier if/loop), fall back.
                    if t < range_start {
                        return Err(FallbackReason {
                            offset: (cur * 16) as u32,
                            opcode: insn.raw_opcode,
                            reason: "overlapping backward branch",
                        });
                    }
                    let body = structure_range(decoded, t, cur)?;
                    nodes.push(CfgNode::Loop {
                        header_idx: t,
                        body_nodes: body,
                        continue_cond: effective_guard,
                        continue_negated: false,
                    });
                    cur += 1;
                    range_start = cur;
                    continue;
                } else if t > cur && t <= end_excl {
                    // Forward branch: if unconditional (`t > cur + 1`), skip or check diamond.
                    if cur > range_start {
                        nodes.push(CfgNode::Range { start: range_start, end_excl: cur });
                    }
                    // Check if end of `then` (`t - 1`) is an unconditional forward BRA to `else_end`.
                    if effective_guard.is_always_true() {
                        // Unconditional skip `[cur+1..t]`
                        cur = t;
                        range_start = cur;
                        continue;
                    }
                    // Conditional `@P BRA t`: body between `cur+1..t` runs when `!P` (or up to unconditional else jump).
                    let (else_nodes, then_nodes, resume_at) = if t > cur + 1 {
                        if let DecodedOp::Bra { target_index: jmp_t, extra_pred: jmp_extra } = decoded[t - 1].op {
                            if decoded[t - 1].guard.is_always_true()
                                && jmp_extra.is_always_true()
                                && jmp_t >= t as i32
                                && (jmp_t as usize) <= end_excl
                            {
                                let fallthrough = structure_range(decoded, cur + 1, t - 1)?;
                                let taken = structure_range(decoded, t, jmp_t as usize)?;
                                (fallthrough, taken, jmp_t as usize)
                            } else {
                                let fallthrough = structure_range(decoded, cur + 1, t)?;
                                (fallthrough, Vec::new(), t)
                            }
                        } else {
                            let fallthrough = structure_range(decoded, cur + 1, t)?;
                            (fallthrough, Vec::new(), t)
                        }
                    } else {
                        (Vec::new(), Vec::new(), t)
                    };
                    nodes.push(CfgNode::If {
                        cond_block_end: cur,
                        cond: effective_guard,
                        then_nodes,
                        else_nodes,
                    });
                    cur = resume_at;
                    range_start = cur;
                    continue;
                } else {
                    return Err(FallbackReason {
                        offset: (cur * 16) as u32,
                        opcode: insn.raw_opcode,
                        reason: "irreducible cross-region branch",
                    });
                }
            }
            DecodedOp::Exit if insn.guard.is_always_true() => {
                nodes.push(CfgNode::Range {
                    start: range_start,
                    end_excl: cur + 1,
                });
                return Ok(nodes);
            }
            _ => {
                cur += 1;
            }
        }
    }
    if cur > range_start {
        nodes.push(CfgNode::Range {
            start: range_start,
            end_excl: cur,
        });
    }
    Ok(nodes)
}

/// Lowers SM86 native program input to optimized SSA IR, or cleanly returns `FallbackReason`.
pub fn lower_to_ssa(input: &LowerInput<'_>) -> Result<SsaProgram, FallbackReason> {
    if input.profile.supported_spirv < SPIRV_1_6_VERSION {
        return Err(FallbackReason {
            offset: 0,
            opcode: 0,
            reason: "driver SPIR-V version below 1.6",
        });
    }
    if input.stage != Stage::Compute {
        return Err(FallbackReason {
            offset: 0,
            opcode: 0,
            reason: "non-compute stage routed to Maxwell lowering",
        });
    }
    if input.code.is_empty() {
        return Err(FallbackReason {
            offset: 0,
            opcode: 0,
            reason: "empty SM86 shader code",
        });
    }

    let decoded = decode_program(input.code);
    let reachable = find_reachable(&decoded);

    // Ensure every reachable instruction is supported before structuring.
    for (idx, insn) in decoded.iter().enumerate() {
        if reachable[idx] {
            if let DecodedOp::Unsupported { raw_opcode, reason } = insn.op {
                return Err(FallbackReason {
                    offset: (idx * 16) as u32,
                    opcode: raw_opcode,
                    reason,
                });
            }
        }
    }

    // Find highest reachable index + 1 so dead padding after EXIT does not trigger errors.
    let mut reach_end = 0usize;
    for (i, &r) in reachable.iter().enumerate() {
        if r {
            reach_end = i + 1;
        }
    }

    let structured = structure_range(&decoded, 0, reach_end)?;
    let coop_config = input.profile.coop_layout.normalized();

    let mut builder = SsaBuilder {
        stage: input.stage,
        program_key: input.program_key,
        total_sm86_insns: input.code.len(),
        decoded,
        profile: input.profile,
        coop_config,
        snapshot: &input.snapshot,
        info: ShaderInfo::default(),
        blocks: Vec::new(),
        next_id: 1,
        workgroup_size: [
            input.workgroup_size[0].max(1),
            input.workgroup_size[1].max(1),
            input.workgroup_size[2].max(1),
        ],
        shared_bytes_needed: input.shared_memory_size,
        local_bytes_needed: input.local_memory_size,
        uses_dynamic_cbuf: false,
        zero_u32: ValueId(0),
        one_u32: ValueId(0),
        true_bool: ValueId(0),
        false_bool: ValueId(0),
    };

    let entry_blk = builder.new_block();
    builder.zero_u32 = builder.alloc_val(entry_blk, ValType::U32, SsaOp::ConstU32(0));
    builder.one_u32 = builder.alloc_val(entry_blk, ValType::U32, SsaOp::ConstU32(1));
    builder.true_bool = builder.alloc_val(entry_blk, ValType::Bool, SsaOp::ConstBool(true));
    builder.false_bool = builder.alloc_val(entry_blk, ValType::Bool, SsaOp::ConstBool(false));

    let mut state = RegState {
        r: [builder.zero_u32; 255],
        ur: [builder.zero_u32; 63],
        p: [builder.false_bool; 7],
        up: [builder.false_bool; 7],
        r_ptr: [None; 255],
        ur_ptr: [None; 63],
        r_cbuf_hi: [None; 255],
        ur_cbuf_hi: [None; 63],
    };

    let mut cur_blk = entry_blk;
    let mut active = true;
    builder.lower_node_seq(&structured, &mut cur_blk, &mut state, &mut active)?;
    if active {
        builder.blocks[cur_blk].terminator = Terminator::Return;
    }

    let mut bindings = input.bindings;
    builder
        .info
        .finalize_descriptors(&mut bindings, input.profile.unified_descriptor_binding);

    let mut program = SsaProgram {
        stage: input.stage,
        workgroup_size: builder.workgroup_size,
        shared_memory_bytes: (builder.shared_bytes_needed + 3) & !3,
        local_memory_bytes: (builder.local_bytes_needed + 3) & !3,
        coop_config: builder.coop_config,
        profile: builder.profile,
        initial_bindings: input.bindings,
        bindings,
        info: builder.info,
        blocks: builder.blocks,
        next_value_id: builder.next_id,
        uses_dynamic_cbuf: builder.uses_dynamic_cbuf,
    };

    optimize_ssa(&mut program);
    Ok(program)
}

/// Performs copy propagation, constant folding, algebraic simplification, and dead-code elimination.
fn optimize_ssa(program: &mut SsaProgram) {
    let mut copies: BTreeMap<ValueId, ValueId> = BTreeMap::new();
    let mut const_u32: BTreeMap<ValueId, u32> = BTreeMap::new();
    let mut const_bool: BTreeMap<ValueId, bool> = BTreeMap::new();

    for _pass in 0..3 {
        copies.clear();
        const_u32.clear();
        const_bool.clear();

        for block in &mut program.blocks {
            for insn in &mut block.insns {
                rewrite_operands(&mut insn.op, &copies);
                match &insn.op {
                    SsaOp::ConstU32(v) => {
                        const_u32.insert(insn.id, *v);
                    }
                    SsaOp::ConstBool(b) => {
                        const_bool.insert(insn.id, *b);
                    }
                    SsaOp::Copy(src) => {
                        copies.insert(insn.id, *src);
                    }
                    SsaOp::Phi(entries) => {
                        if let Some((_, first)) = entries.first() {
                            if entries.iter().all(|(_, v)| v == first || *v == insn.id) {
                                copies.insert(insn.id, *first);
                            }
                        }
                    }
                    SsaOp::IAdd(a, b) => {
                        if let (Some(&ca), Some(&cb)) = (const_u32.get(a), const_u32.get(b)) {
                            let r = ca.wrapping_add(cb);
                            insn.op = SsaOp::ConstU32(r);
                            const_u32.insert(insn.id, r);
                        } else if const_u32.get(b) == Some(&0) {
                            copies.insert(insn.id, *a);
                            insn.op = SsaOp::Copy(*a);
                        } else if const_u32.get(a) == Some(&0) {
                            copies.insert(insn.id, *b);
                            insn.op = SsaOp::Copy(*b);
                        }
                    }
                    SsaOp::IMul(a, b) => {
                        if let (Some(&ca), Some(&cb)) = (const_u32.get(a), const_u32.get(b)) {
                            let r = ca.wrapping_mul(cb);
                            insn.op = SsaOp::ConstU32(r);
                            const_u32.insert(insn.id, r);
                        } else if const_u32.get(b) == Some(&1) {
                            copies.insert(insn.id, *a);
                            insn.op = SsaOp::Copy(*a);
                        } else if const_u32.get(a) == Some(&1) {
                            copies.insert(insn.id, *b);
                            insn.op = SsaOp::Copy(*b);
                        } else if const_u32.get(a) == Some(&0) || const_u32.get(b) == Some(&0) {
                            insn.op = SsaOp::ConstU32(0);
                            const_u32.insert(insn.id, 0);
                        }
                    }
                    SsaOp::BitOr(a, b) => {
                        if let (Some(&ca), Some(&cb)) = (const_u32.get(a), const_u32.get(b)) {
                            let r = ca | cb;
                            insn.op = SsaOp::ConstU32(r);
                            const_u32.insert(insn.id, r);
                        } else if const_u32.get(b) == Some(&0) {
                            copies.insert(insn.id, *a);
                            insn.op = SsaOp::Copy(*a);
                        } else if const_u32.get(a) == Some(&0) {
                            copies.insert(insn.id, *b);
                            insn.op = SsaOp::Copy(*b);
                        }
                    }
                    SsaOp::BitAnd(a, b) => {
                        if let (Some(&ca), Some(&cb)) = (const_u32.get(a), const_u32.get(b)) {
                            let r = ca & cb;
                            insn.op = SsaOp::ConstU32(r);
                            const_u32.insert(insn.id, r);
                        } else if const_u32.get(b) == Some(&u32::MAX) {
                            copies.insert(insn.id, *a);
                            insn.op = SsaOp::Copy(*a);
                        } else if const_u32.get(a) == Some(&0) || const_u32.get(b) == Some(&0) {
                            insn.op = SsaOp::ConstU32(0);
                            const_u32.insert(insn.id, 0);
                        }
                    }
                    SsaOp::BitXor(a, b) if a == b => {
                        insn.op = SsaOp::ConstU32(0);
                        const_u32.insert(insn.id, 0);
                    }
                    SsaOp::Shl(a, b) | SsaOp::LShr(a, b) if const_u32.get(b) == Some(&0) => {
                        copies.insert(insn.id, *a);
                        insn.op = SsaOp::Copy(*a);
                    }
                    SsaOp::Select { cond, t, f } => {
                        if let Some(&cb) = const_bool.get(cond) {
                            let chosen = if cb { *t } else { *f };
                            copies.insert(insn.id, chosen);
                            insn.op = SsaOp::Copy(chosen);
                        } else if t == f {
                            copies.insert(insn.id, *t);
                            insn.op = SsaOp::Copy(*t);
                        }
                    }
                    SsaOp::BoolNot(v) => {
                        if let Some(&cb) = const_bool.get(v) {
                            insn.op = SsaOp::ConstBool(!cb);
                            const_bool.insert(insn.id, !cb);
                        }
                    }
                    _ => {}
                }
            }
            if let Terminator::BranchCond { cond, .. } = &mut block.terminator {
                *cond = resolve_copy(&copies, *cond);
            }
        }
    }

    // Dead code elimination from roots (side effects and branch conditions).
    let mut live: BTreeSet<ValueId> = BTreeSet::new();
    let mut def_map: BTreeMap<ValueId, Vec<ValueId>> = BTreeMap::new();
    let mut worklist: VecDeque<ValueId> = VecDeque::new();

    for block in &program.blocks {
        if let Terminator::BranchCond { cond, .. } = block.terminator {
            if live.insert(cond) {
                worklist.push_back(cond);
            }
        }
        for insn in &block.insns {
            let mut deps = Vec::new();
            collect_operands(&insn.op, &mut deps);
            let side_effect = is_side_effect(&insn.op);
            def_map.insert(insn.id, deps);
            if side_effect && live.insert(insn.id) {
                worklist.push_back(insn.id);
            }
        }
    }

    while let Some(v) = worklist.pop_front() {
        if let Some(deps) = def_map.get(&v) {
            for &dep in deps {
                if live.insert(dep) {
                    worklist.push_back(dep);
                }
            }
        }
    }

    for block in &mut program.blocks {
        block.insns.retain(|i| live.contains(&i.id));
    }

    // Update `uses_int64` / `uses_fp16` flags accurately after DCE.
    program.info.uses_int64 = false;
    for block in &program.blocks {
        for insn in &block.insns {
            if insn.ty == ValType::U64 && !matches!(insn.op, SsaOp::CoopHmma { .. }) {
                program.info.uses_int64 = true;
            }
        }
    }
}

fn is_side_effect(op: &SsaOp) -> bool {
    matches!(
        op,
        SsaOp::StoreSsboWord { .. }
            | SsaOp::AtomicSsboWord { .. }
            | SsaOp::StoreSharedWord { .. }
            | SsaOp::AtomicSharedWord { .. }
            | SsaOp::StoreLocalWord { .. }
            | SsaOp::ControlBarrier
            | SsaOp::MemoryBarrier { .. }
            | SsaOp::CoopHmma { .. }
    )
}

fn resolve_copy(copies: &BTreeMap<ValueId, ValueId>, mut v: ValueId) -> ValueId {
    for _ in 0..32 {
        match copies.get(&v) {
            Some(&next) if next != v => v = next,
            _ => break,
        }
    }
    v
}

fn rewrite_operands(op: &mut SsaOp, copies: &BTreeMap<ValueId, ValueId>) {
    let r = |v: &mut ValueId| *v = resolve_copy(copies, *v);
    match op {
        SsaOp::ConstU32(_)
        | SsaOp::ConstBool(_)
        | SsaOp::ConstU64(_)
        | SsaOp::Undef
        | SsaOp::ReadBuiltin(_)
        | SsaOp::ReadCbuf { .. }
        | SsaOp::ControlBarrier
        | SsaOp::MemoryBarrier { .. } => {}
        SsaOp::ReadCbufDynamic { byte_offset, .. } => r(byte_offset),
        SsaOp::Copy(a)
        | SsaOp::Bitcast { val: a, .. }
        | SsaOp::BitNot(a)
        | SsaOp::INeg(a)
        | SsaOp::BitCount(a)
        | SsaOp::BitReverse(a)
        | SsaOp::SBitFieldExtract { val: a, .. }
        | SsaOp::UBitFieldExtract { val: a, .. }
        | SsaOp::BoolNot(a)
        | SsaOp::FNeg(a)
        | SsaOp::FAbs(a)
        | SsaOp::Glsl1(_, a)
        | SsaOp::SConvertToF32(a)
        | SsaOp::UConvertToF32(a)
        | SsaOp::F32ConvertToS(a)
        | SsaOp::F32ConvertToU(a)
        | SsaOp::FConvertF32ToF16Lo(a)
        | SsaOp::FConvertF16LoToF32(a)
        | SsaOp::LoadSsboWord { word_idx: a, .. }
        | SsaOp::LoadSharedWord { word_idx: a }
        | SsaOp::LoadLocalWord { word_idx: a }
        | SsaOp::SubgroupVote { pred: a, .. }
        | SsaOp::SubgroupBallot { pred: a }
        | SsaOp::CoopHmmaExtract { hmma: a, .. } => r(a),
        SsaOp::Phi(entries) => {
            for (_, v) in entries {
                r(v);
            }
        }
        SsaOp::Select { cond, t, f } => {
            r(cond);
            r(t);
            r(f);
        }
        SsaOp::IAdd(a, b)
        | SsaOp::ISub(a, b)
        | SsaOp::IMul(a, b)
        | SsaOp::UMulHi(a, b)
        | SsaOp::SMulHi(a, b)
        | SsaOp::BitAnd(a, b)
        | SsaOp::BitOr(a, b)
        | SsaOp::BitXor(a, b)
        | SsaOp::Shl(a, b)
        | SsaOp::LShr(a, b)
        | SsaOp::AShr(a, b)
        | SsaOp::IntCmp { a, b, .. }
        | SsaOp::BoolAnd(a, b)
        | SsaOp::BoolOr(a, b)
        | SsaOp::BoolXor(a, b)
        | SsaOp::FAdd(a, b)
        | SsaOp::FSub(a, b)
        | SsaOp::FMul(a, b)
        | SsaOp::FDiv(a, b)
        | SsaOp::FSet { a, b, .. }
        | SsaOp::Glsl2(_, a, b)
        | SsaOp::HAdd2 { a, b }
        | SsaOp::HMul2 { a, b }
        | SsaOp::StoreSsboWord { word_idx: a, val: b, .. }
        | SsaOp::AtomicSsboWord { word_idx: a, data: b, .. }
        | SsaOp::StoreSharedWord { word_idx: a, val: b }
        | SsaOp::AtomicSharedWord { word_idx: a, data: b, .. }
        | SsaOp::StoreLocalWord { word_idx: a, val: b }
        | SsaOp::SubgroupShuffle { val: a, lane_or_delta: b, .. } => {
            r(a);
            r(b);
        }
        SsaOp::HFma2 { a, b, c } => {
            r(a);
            r(b);
            r(c);
        }
        SsaOp::CoopHmma { a, a_len, b, b_len, c, .. } => {
            for slot in &mut a[..*a_len] {
                r(slot);
            }
            for slot in &mut b[..*b_len] {
                r(slot);
            }
            for slot in c {
                r(slot);
            }
        }
    }
}

fn collect_operands(op: &SsaOp, out: &mut Vec<ValueId>) {
    match op {
        SsaOp::ConstU32(_)
        | SsaOp::ConstBool(_)
        | SsaOp::ConstU64(_)
        | SsaOp::Undef
        | SsaOp::ReadBuiltin(_)
        | SsaOp::ReadCbuf { .. }
        | SsaOp::ControlBarrier
        | SsaOp::MemoryBarrier { .. } => {}
        SsaOp::ReadCbufDynamic { byte_offset, .. } => out.push(*byte_offset),
        SsaOp::Copy(a)
        | SsaOp::Bitcast { val: a, .. }
        | SsaOp::BitNot(a)
        | SsaOp::INeg(a)
        | SsaOp::BitCount(a)
        | SsaOp::BitReverse(a)
        | SsaOp::SBitFieldExtract { val: a, .. }
        | SsaOp::UBitFieldExtract { val: a, .. }
        | SsaOp::BoolNot(a)
        | SsaOp::FNeg(a)
        | SsaOp::FAbs(a)
        | SsaOp::Glsl1(_, a)
        | SsaOp::SConvertToF32(a)
        | SsaOp::UConvertToF32(a)
        | SsaOp::F32ConvertToS(a)
        | SsaOp::F32ConvertToU(a)
        | SsaOp::FConvertF32ToF16Lo(a)
        | SsaOp::FConvertF16LoToF32(a)
        | SsaOp::LoadSsboWord { word_idx: a, .. }
        | SsaOp::LoadSharedWord { word_idx: a }
        | SsaOp::LoadLocalWord { word_idx: a }
        | SsaOp::SubgroupVote { pred: a, .. }
        | SsaOp::SubgroupBallot { pred: a }
        | SsaOp::CoopHmmaExtract { hmma: a, .. } => out.push(*a),
        SsaOp::Phi(entries) => {
            for (_, v) in entries {
                out.push(*v);
            }
        }
        SsaOp::Select { cond, t, f } => {
            out.push(*cond);
            out.push(*t);
            out.push(*f);
        }
        SsaOp::IAdd(a, b)
        | SsaOp::ISub(a, b)
        | SsaOp::IMul(a, b)
        | SsaOp::UMulHi(a, b)
        | SsaOp::SMulHi(a, b)
        | SsaOp::BitAnd(a, b)
        | SsaOp::BitOr(a, b)
        | SsaOp::BitXor(a, b)
        | SsaOp::Shl(a, b)
        | SsaOp::LShr(a, b)
        | SsaOp::AShr(a, b)
        | SsaOp::IntCmp { a, b, .. }
        | SsaOp::BoolAnd(a, b)
        | SsaOp::BoolOr(a, b)
        | SsaOp::BoolXor(a, b)
        | SsaOp::FAdd(a, b)
        | SsaOp::FSub(a, b)
        | SsaOp::FMul(a, b)
        | SsaOp::FDiv(a, b)
        | SsaOp::FSet { a, b, .. }
        | SsaOp::Glsl2(_, a, b)
        | SsaOp::HAdd2 { a, b }
        | SsaOp::HMul2 { a, b }
        | SsaOp::StoreSsboWord { word_idx: a, val: b, .. }
        | SsaOp::AtomicSsboWord { word_idx: a, data: b, .. }
        | SsaOp::StoreSharedWord { word_idx: a, val: b }
        | SsaOp::AtomicSharedWord { word_idx: a, data: b, .. }
        | SsaOp::StoreLocalWord { word_idx: a, val: b }
        | SsaOp::SubgroupShuffle { val: a, lane_or_delta: b, .. } => {
            out.push(*a);
            out.push(*b);
        }
        SsaOp::HFma2 { a, b, c } => {
            out.push(*a);
            out.push(*b);
            out.push(*c);
        }
        SsaOp::CoopHmma { a, a_len, b, b_len, c, .. } => {
            out.extend_from_slice(&a[..*a_len]);
            out.extend_from_slice(&b[..*b_len]);
            out.extend_from_slice(c);
        }
    }
}
