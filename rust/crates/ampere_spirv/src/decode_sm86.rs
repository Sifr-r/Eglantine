// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use eglantine_ampere_rs::Instruction;

/// Predicate register guard on every SM86 instruction (`bits(12, 4)`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PredGuard {
    pub neg: bool,
    pub id: u8,
}

impl PredGuard {
    #[inline]
    pub const fn always_true() -> Self {
        Self { neg: false, id: 7 }
    }

    #[inline]
    pub const fn is_always_true(self) -> bool {
        !self.neg && self.id == 7
    }

    #[inline]
    pub const fn is_always_false(self) -> bool {
        self.neg && self.id == 7
    }
}

/// Remapped SM86 constant buffer bank (`bank == 24` aliases to `17`) and 16-bit byte offset.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct CbufRef {
    pub bank: u8,
    pub offset: u16,
}

/// Source operand decoded from SM86 orthogonal ALU encodings.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SrcOperand {
    Zero,
    Reg(u8),
    UReg(u8),
    Imm32(u32),
    Cbuf(CbufRef),
}

impl SrcOperand {
    #[inline]
    pub const fn reg(id: u32) -> Self {
        if id == 255 {
            Self::Zero
        } else {
            Self::Reg(id as u8)
        }
    }

    #[inline]
    pub const fn ureg(id: u32) -> Self {
        if id == 63 {
            Self::Zero
        } else {
            Self::UReg(id as u8)
        }
    }
}

/// Float operand with optional `abs` and `negate` modifiers.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FloatSrc {
    pub op: SrcOperand,
    pub abs: bool,
    pub neg: bool,
}

/// Packed FP16x2 source operand (`swizzle`: 0=H0_H1, 1=F32_broadcast?, 2=H0_H0, 3=H1_H1).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct HalfSrc {
    pub op: SrcOperand,
    pub swizzle: u8,
    pub abs: bool,
    pub neg: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum S2rSpecial {
    LaneId,
    InvocationId,
    TidX,
    TidY,
    TidZ,
    CtaIdX,
    CtaIdY,
    CtaIdZ,
    LaneMaskEq,
    LaneMaskLt,
    LaneMaskLe,
    LaneMaskGt,
    LaneMaskGe,
    VariableRate,
    Unsupported(u8),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IntCmp {
    False,
    Lt,
    Eq,
    Le,
    Gt,
    Ne,
    Ge,
    True,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BoolComb {
    And,
    Or,
    Xor,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FloatCmp {
    False,
    Lt,
    Eq,
    Le,
    Gt,
    Ne,
    Ge,
    Num,
    Nan,
    Ltu,
    Equ,
    Leu,
    Gtu,
    Neu,
    Geu,
    True,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MufuOp {
    CosTurns,
    SinTurns,
    Ex2,
    Lg2,
    Rcp,
    Rsq,
    Sqrt,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MemWidth {
    U8,
    S8,
    U16,
    S16,
    B32,
    B64,
    B128,
}

impl MemWidth {
    #[inline]
    pub const fn from_bits(bits: u32) -> Option<Self> {
        match bits {
            0 => Some(Self::U8),
            1 => Some(Self::S8),
            2 => Some(Self::U16),
            3 => Some(Self::S16),
            4 => Some(Self::B32),
            5 => Some(Self::B64),
            6 => Some(Self::B128),
            _ => None,
        }
    }

    #[inline]
    pub const fn word_count(self) -> u32 {
        match self {
            Self::B64 => 2,
            Self::B128 => 4,
            _ => 1,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShflMode {
    Idx,
    Up,
    Down,
    Bfly,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VoteMode {
    All,
    Any,
    Eq,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MemBarrierScope {
    Workgroup,
    Device,
    System,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AtomicOp {
    Add,
    Min,
    Max,
    Inc,
    Dec,
    And,
    Or,
    Xor,
    Exch,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DecodedOp {
    Nop,
    Exit,
    Bra {
        target_index: i32,
        extra_pred: PredGuard,
    },
    Mov {
        dst: u8,
        src: SrcOperand,
    },
    Umov {
        dst_ur: u8,
        src: SrcOperand,
    },
    Sel {
        dst: u8,
        a: SrcOperand,
        b: SrcOperand,
        pred: PredGuard,
    },
    Cs2rZero {
        dst: u8,
    },
    S2r {
        dst: u8,
        special: S2rSpecial,
    },
    S2ur {
        dst_ur: u8,
        special: S2rSpecial,
    },
    R2ur {
        dst_ur: u8,
        src: u8,
    },
    Iadd3 {
        dst: u8,
        a: SrcOperand,
        b: SrcOperand,
        c: SrcOperand,
        neg_a: bool,
        neg_b: bool,
        neg_c: bool,
        x_mode: bool,
        carry_in0: u8,
        carry_in1: u8,
        carry_out0: u8,
        carry_out1: u8,
    },
    Uiadd3 {
        dst_ur: u8,
        a: SrcOperand,
        b: SrcOperand,
        c: SrcOperand,
        neg_a: bool,
        neg_b: bool,
        x_mode: bool,
        carry_in: u8,
        carry_out: u8,
    },
    Imad {
        dst: u8,
        a: SrcOperand,
        b: SrcOperand,
        c: SrcOperand,
        neg_c: bool,
        signed: bool,
        hi: bool,
        wide: bool,
    },
    Uimad {
        dst_ur: u8,
        a: SrcOperand,
        b: SrcOperand,
        c: SrcOperand,
        neg_c: bool,
        signed: bool,
        wide: bool,
    },
    Lea {
        dst: u8,
        a: SrcOperand,
        b: SrcOperand,
        c_hi: SrcOperand,
        scale: u8,
        hi: bool,
        neg_b: bool,
        x_mode: bool,
        carry_out: u8,
    },
    Ulea {
        dst_ur: u8,
        a: SrcOperand,
        b: SrcOperand,
        scale: u8,
        hi: bool,
        neg_b: bool,
    },
    Lop3 {
        dst: u8,
        a: SrcOperand,
        b: SrcOperand,
        c: SrcOperand,
        lut: u8,
        pred_out: u8,
    },
    Ulop3 {
        dst_ur: u8,
        a: SrcOperand,
        b: SrcOperand,
        c: SrcOperand,
        lut: u8,
        pred_out: u8,
    },
    Shf {
        dst: u8,
        a: SrcOperand,
        shift: SrcOperand,
        c: SrcOperand,
        right: bool,
        hi: bool,
        signed: bool,
        wrap: bool,
    },
    Ushf {
        dst_ur: u8,
        a: SrcOperand,
        shift: SrcOperand,
        c: SrcOperand,
        right: bool,
        hi: bool,
        signed: bool,
        wrap: bool,
    },
    Isetp {
        dst_p: u8,
        dst_q: u8,
        a: SrcOperand,
        b: SrcOperand,
        cmp: IntCmp,
        signed: bool,
        comb: BoolComb,
        accum: PredGuard,
    },
    Uisetp {
        dst_up: u8,
        a: SrcOperand,
        b: SrcOperand,
        cmp: IntCmp,
        signed: bool,
        comb: BoolComb,
        accum: PredGuard,
    },
    Imnmx {
        dst: u8,
        a: SrcOperand,
        b: SrcOperand,
        pred_min: PredGuard,
        signed: bool,
    },
    Iabs {
        dst: u8,
        src: SrcOperand,
    },
    Popc {
        dst: u8,
        src: SrcOperand,
    },
    Brev {
        dst: u8,
        src: SrcOperand,
    },
    Flo {
        dst: u8,
        src: SrcOperand,
        signed: bool,
        inv: bool,
        shamt: bool,
    },
    Uflo {
        dst_ur: u8,
        src: SrcOperand,
        shamt: bool,
    },
    Sgxt {
        dst: u8,
        src: SrcOperand,
        width: u32,
        signed: bool,
    },
    Prmt {
        dst: u8,
        a: SrcOperand,
        sel: u16,
        c: SrcOperand,
    },
    Fadd {
        dst: u8,
        a: FloatSrc,
        b: FloatSrc,
        sat: bool,
    },
    Fmul {
        dst: u8,
        a: FloatSrc,
        b: FloatSrc,
        sat: bool,
    },
    Ffma {
        dst: u8,
        a: FloatSrc,
        b: FloatSrc,
        c: FloatSrc,
        sat: bool,
    },
    Fmnmx {
        dst: u8,
        a: FloatSrc,
        b: FloatSrc,
        pred_min: PredGuard,
    },
    Fsel {
        dst: u8,
        a: FloatSrc,
        b: FloatSrc,
        pred: PredGuard,
    },
    Fsetp {
        dst_p: u8,
        dst_q: u8,
        a: FloatSrc,
        b: FloatSrc,
        cmp: FloatCmp,
        comb: BoolComb,
        accum: PredGuard,
    },
    Mufu {
        dst: u8,
        src: SrcOperand,
        op: MufuOp,
        abs: bool,
        neg: bool,
        sat: bool,
    },
    I2f {
        dst: u8,
        src: SrcOperand,
        src_signed: bool,
        src_bits: u8,
        byte_lane: u8,
        dst_f16: bool,
        abs: bool,
    },
    F2i {
        dst: u8,
        src: SrcOperand,
        dst_signed: bool,
        dst_bits: u8,
        src_f16: bool,
        half_lane: u8,
        abs: bool,
        neg: bool,
        round_mode: u8,
    },
    F2f {
        dst: u8,
        src: SrcOperand,
        src_bits: u8,
        dst_bits: u8,
        half_lane: u8,
        sat: bool,
        abs: bool,
        neg: bool,
    },
    Hadd2 {
        dst: u8,
        a: HalfSrc,
        b: HalfSrc,
        sat: bool,
        f32_out: bool,
    },
    Hmul2 {
        dst: u8,
        a: HalfSrc,
        b: HalfSrc,
        sat: bool,
    },
    Hfma2 {
        dst: u8,
        a: HalfSrc,
        b: HalfSrc,
        c: HalfSrc,
        sat: bool,
        relu: bool,
    },
    Ldc {
        dst: u8,
        bank: u8,
        offset: u16,
        index_reg: Option<u8>,
        width: MemWidth,
    },
    Uldc {
        dst_ur: u8,
        bank: u8,
        offset: u16,
        width: MemWidth,
    },
    Ldg {
        dst: u8,
        base_reg: u8,
        uniform_reg: Option<u8>,
        imm_offset: i32,
        width: MemWidth,
        reg_64bit: bool,
        coherent: bool,
        descriptor_mode: bool,
    },
    Stg {
        src_reg: u8,
        base_reg: u8,
        uniform_reg: Option<u8>,
        imm_offset: i32,
        width: MemWidth,
        reg_64bit: bool,
        descriptor_mode: bool,
    },
    Lds {
        dst: u8,
        base_reg: u8,
        uniform_reg: Option<u8>,
        imm_offset: i32,
        stride: u8,
        width: MemWidth,
    },
    Sts {
        src_reg: u8,
        base_reg: u8,
        uniform_reg: Option<u8>,
        imm_offset: i32,
        stride: u8,
        width: MemWidth,
    },
    Ldl {
        dst: u8,
        base_reg: u8,
        offset: u32,
        width: MemWidth,
    },
    Stl {
        src_reg: u8,
        base_reg: u8,
        offset: u32,
        width: MemWidth,
    },
    Atoms {
        dst: u8,
        base_reg: u8,
        imm_offset: i32,
        src_a: u8,
        op: AtomicOp,
        is_64bit: bool,
    },
    Atomg {
        dst: Option<u8>,
        base_reg: u8,
        uniform_reg: Option<u8>,
        imm_offset: i32,
        src_a: u8,
        op: AtomicOp,
    },
    Shfl {
        dst: u8,
        pred_out: u8,
        src: u8,
        lane: SrcOperand,
        mask: SrcOperand,
        mode: ShflMode,
    },
    Vote {
        dst_reg: u8,
        dst_uniform: bool,
        pred_out: u8,
        src_pred: PredGuard,
        mode: VoteMode,
    },
    BarSync {
        defer_blocking: bool,
    },
    Membar {
        scope: MemBarrierScope,
    },
    Hmma {
        dst: u8,
        ra: u8,
        rb: u8,
        rc: u8,
        depth: u32,
    },
    Unsupported {
        raw_opcode: u32,
        reason: &'static str,
    },
}

/// Complete decoded SM86 instruction with execution predicate guard.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DecodedInsn {
    pub index: usize,
    pub raw_opcode: u32,
    pub guard: PredGuard,
    pub op: DecodedOp,
}

#[inline]
fn bank_from_insn(insn: &Instruction) -> Option<u8> {
    let raw = insn.bits(54, 5);
    let mapped = if raw == 24 { 17 } else { raw };
    if mapped < 18 {
        Some(mapped as u8)
    } else {
        None
    }
}

#[inline]
fn cbuf_from_insn(insn: &Instruction) -> Option<CbufRef> {
    if insn.bits(91, 1) != 0 {
        return None;
    }
    let bank = bank_from_insn(insn)?;
    let offset = insn.bits(38, 16) as u16;
    if (offset & 3) != 0 {
        return None;
    }
    Some(CbufRef { bank, offset })
}

#[inline]
fn parse_guard(bits: u32) -> PredGuard {
    PredGuard {
        neg: (bits & 8) != 0,
        id: (bits & 7) as u8,
    }
}

fn decode_alu_operand(insn: &Instruction, which: u32) -> Option<(SrcOperand, bool, bool)> {
    let form = insn.bits(9, 3);
    if which == 0 {
        let reg = insn.bits(24, 8);
        let abs = insn.bits(73, 1) != 0;
        let neg = insn.bits(72, 1) != 0;
        return Some((SrcOperand::reg(reg), abs, neg));
    }
    if !(1..=7).contains(&form) {
        return None;
    }
    let wide = if which == 1 {
        matches!(form, 4..=6)
    } else {
        matches!(form, 2 | 3 | 7)
    };
    if wide {
        let abs = insn.bits(62, 1) != 0;
        let neg = insn.bits(63, 1) != 0;
        match form {
            2 | 4 => Some((SrcOperand::Imm32(insn.words[1]), false, false)),
            3 | 5 => Some((SrcOperand::Cbuf(cbuf_from_insn(insn)?), abs, neg)),
            6 | 7 => Some((SrcOperand::ureg(insn.bits(32, 8)), abs, neg)),
            _ => None,
        }
    } else {
        let upper = if which == 1 {
            matches!(form, 2 | 3 | 7)
        } else {
            true
        };
        let reg = insn.bits(if upper { 64 } else { 32 }, 8);
        let abs = insn.bits(if upper { 74 } else { 62 }, 1) != 0;
        let neg = insn.bits(if upper { 75 } else { 63 }, 1) != 0;
        Some((SrcOperand::reg(reg), abs, neg))
    }
}

fn decode_half_operand(insn: &Instruction, which: u32) -> Option<HalfSrc> {
    if which == 0 {
        return Some(HalfSrc {
            op: SrcOperand::reg(insn.bits(24, 8)),
            swizzle: insn.bits(74, 2) as u8,
            abs: insn.bits(73, 1) != 0,
            neg: insn.bits(72, 1) != 0,
        });
    }
    let form = insn.bits(9, 3);
    let wide = if which == 1 {
        form == 4 || form == 5
    } else {
        form == 2 || form == 3
    };
    if wide {
        let immediate = form == 2 || form == 4;
        let op = if immediate {
            SrcOperand::Imm32(insn.words[1])
        } else {
            SrcOperand::Cbuf(cbuf_from_insn(insn)?)
        };
        Some(HalfSrc {
            op,
            swizzle: insn.bits(60, 2) as u8,
            abs: insn.bits(62, 1) != 0,
            neg: insn.bits(63, 1) != 0,
        })
    } else {
        let upper = which == 2 || form == 2 || form == 3;
        let reg = insn.bits(if upper { 64 } else { 32 }, 8);
        Some(HalfSrc {
            op: SrcOperand::reg(reg),
            swizzle: insn.bits(if upper { 68 } else { 60 }, 2) as u8,
            abs: insn.bits(if upper { 70 } else { 62 }, 1) != 0,
            neg: insn.bits(if upper { 71 } else { 63 }, 1) != 0,
        })
    }
}

fn decode_int_cmp(bits: u32) -> Option<IntCmp> {
    match bits {
        0 => Some(IntCmp::False),
        1 => Some(IntCmp::Lt),
        2 => Some(IntCmp::Eq),
        3 => Some(IntCmp::Le),
        4 => Some(IntCmp::Gt),
        5 => Some(IntCmp::Ne),
        6 => Some(IntCmp::Ge),
        7 => Some(IntCmp::True),
        _ => None,
    }
}

fn decode_float_cmp(bits: u32) -> Option<FloatCmp> {
    match bits {
        0 => Some(FloatCmp::False),
        1 => Some(FloatCmp::Lt),
        2 => Some(FloatCmp::Eq),
        3 => Some(FloatCmp::Le),
        4 => Some(FloatCmp::Gt),
        5 => Some(FloatCmp::Ne),
        6 => Some(FloatCmp::Ge),
        7 => Some(FloatCmp::Num),
        8 => Some(FloatCmp::Nan),
        9 => Some(FloatCmp::Ltu),
        10 => Some(FloatCmp::Equ),
        11 => Some(FloatCmp::Leu),
        12 => Some(FloatCmp::Gtu),
        13 => Some(FloatCmp::Neu),
        14 => Some(FloatCmp::Geu),
        15 => Some(FloatCmp::True),
        _ => None,
    }
}

fn decode_bool_comb(bits: u32) -> Option<BoolComb> {
    match bits {
        0 => Some(BoolComb::And),
        1 => Some(BoolComb::Or),
        2 => Some(BoolComb::Xor),
        _ => None,
    }
}

fn decode_special(special: u32) -> S2rSpecial {
    match special {
        0 => S2rSpecial::LaneId,
        17 => S2rSpecial::InvocationId,
        33 => S2rSpecial::TidX,
        34 => S2rSpecial::TidY,
        35 => S2rSpecial::TidZ,
        37 => S2rSpecial::CtaIdX,
        38 => S2rSpecial::CtaIdY,
        39 => S2rSpecial::CtaIdZ,
        56 => S2rSpecial::LaneMaskEq,
        57 => S2rSpecial::LaneMaskLt,
        58 => S2rSpecial::LaneMaskLe,
        59 => S2rSpecial::LaneMaskGt,
        60 => S2rSpecial::LaneMaskGe,
        132 => S2rSpecial::VariableRate,
        other => S2rSpecial::Unsupported(other as u8),
    }
}

fn decode_atomic_op(bits: u32) -> Option<AtomicOp> {
    match bits {
        0 => Some(AtomicOp::Add),
        1 => Some(AtomicOp::Min),
        2 => Some(AtomicOp::Max),
        3 => Some(AtomicOp::Inc),
        4 => Some(AtomicOp::Dec),
        5 => Some(AtomicOp::And),
        6 => Some(AtomicOp::Or),
        7 => Some(AtomicOp::Xor),
        8 => Some(AtomicOp::Exch),
        _ => None,
    }
}

/// Decodes a slice of 128-bit SM86 native instructions into structured `DecodedInsn`s.
pub fn decode_program(code: &[Instruction]) -> Vec<DecodedInsn> {
    code.iter().enumerate().map(|(i, insn)| decode_insn(i, insn)).collect()
}

/// Decodes one 128-bit SM86 instruction at instruction index `index`.
pub fn decode_insn(index: usize, insn: &Instruction) -> DecodedInsn {
    let raw_opcode = insn.bits(0, 12);
    let pred_bits = insn.bits(12, 4);
    if pred_bits == 15 {
        return DecodedInsn {
            index,
            raw_opcode,
            guard: PredGuard::always_true(),
            op: DecodedOp::Nop,
        };
    }
    let guard = parse_guard(pred_bits);
    let op = decode_op(index, insn, raw_opcode);
    DecodedInsn {
        index,
        raw_opcode,
        guard,
        op,
    }
}

fn unsup(raw_opcode: u32, reason: &'static str) -> DecodedOp {
    DecodedOp::Unsupported { raw_opcode, reason }
}

fn decode_op(index: usize, insn: &Instruction, raw_opcode: u32) -> DecodedOp {
    let dst = insn.bits(16, 8) as u8;
    let unif_dst = (dst & 63) as u8;
    let form = insn.bits(9, 3);

    match raw_opcode {
        // Control flow & no-ops
        0x918 | 0x91a | 0x9ab => DecodedOp::Nop,
        0x94d => DecodedOp::Exit,
        0x947 => {
            let mode = insn.bits(32, 2);
            if mode == 2 {
                return DecodedOp::Nop;
            }
            if mode != 0 || insn.bits(91, 1) != 0 {
                return unsup(raw_opcode, "unsupported BRA mode");
            }
            let extra_pred = parse_guard(insn.bits(87, 4));
            let raw = ((insn.words[1] & !3u32) as u64) | (((insn.words[2] & 0x3_ffffu32) as u64) << 32);
            let rel_bytes = ((raw << 14) as i64) >> 14;
            let target_bytes = (index as i64) * 16 + 16 + rel_bytes;
            if target_bytes < 0 || target_bytes % 16 != 0 {
                return unsup(raw_opcode, "unaligned BRA target");
            }
            DecodedOp::Bra {
                target_index: (target_bytes / 16) as i32,
                extra_pred,
            }
        }

        // Moves & Special registers
        0x202 | 0x802 | 0xa02 | 0xc02 => {
            let Some((src, _, _)) = decode_alu_operand(insn, 1) else {
                return unsup(raw_opcode, "invalid MOV source");
            };
            DecodedOp::Mov { dst, src }
        }
        0x882 | 0xc82 | 0xa82 => {
            if unif_dst == 63 {
                return DecodedOp::Nop;
            }
            let src = match raw_opcode {
                0x882 => SrcOperand::Imm32(insn.words[1]),
                0xa82 => {
                    let Some(cbuf) = cbuf_from_insn(insn) else {
                        return unsup(raw_opcode, "invalid UMOV cbuf");
                    };
                    SrcOperand::Cbuf(cbuf)
                }
                _ => SrcOperand::ureg(insn.bits(32, 8)),
            };
            DecodedOp::Umov { dst_ur: unif_dst, src }
        }
        0x207 | 0x807 | 0xa07 | 0xc07 => {
            let Some((a, _, _)) = decode_alu_operand(insn, 0) else {
                return unsup(raw_opcode, "invalid SEL source a");
            };
            let Some((b, _, _)) = decode_alu_operand(insn, 1) else {
                return unsup(raw_opcode, "invalid SEL source b");
            };
            DecodedOp::Sel {
                dst,
                a,
                b,
                pred: parse_guard(insn.bits(87, 4)),
            }
        }
        0x805 => {
            if insn.bits(72, 8) == 255 {
                DecodedOp::Cs2rZero { dst }
            } else {
                unsup(raw_opcode, "unsupported CS2R register")
            }
        }
        0x919 => DecodedOp::S2r {
            dst,
            special: decode_special(insn.bits(72, 8)),
        },
        0x9c3 => DecodedOp::S2ur {
            dst_ur: unif_dst,
            special: decode_special(insn.bits(72, 8)),
        },
        0x3c2 => DecodedOp::R2ur {
            dst_ur: unif_dst,
            src: insn.bits(24, 8) as u8,
        },

        // Integer & Bitwise ALU
        0x210 | 0x810 | 0xa10 | 0xc10 => {
            let Some((a, _, _)) = decode_alu_operand(insn, 0) else {
                return unsup(raw_opcode, "invalid IADD3 a");
            };
            let Some((b, _, _)) = decode_alu_operand(insn, 1) else {
                return unsup(raw_opcode, "invalid IADD3 b");
            };
            let Some((c, _, _)) = decode_alu_operand(insn, 2) else {
                return unsup(raw_opcode, "invalid IADD3 c");
            };
            DecodedOp::Iadd3 {
                dst,
                a,
                b,
                c,
                neg_a: insn.bits(72, 1) != 0,
                neg_b: insn.bits(63, 1) != 0,
                neg_c: insn.bits(75, 1) != 0,
                x_mode: insn.bits(74, 1) != 0,
                carry_in0: insn.bits(87, 4) as u8,
                carry_in1: insn.bits(77, 4) as u8,
                carry_out0: insn.bits(81, 3) as u8,
                carry_out1: insn.bits(84, 3) as u8,
            }
        }
        0x290 | 0x890 => {
            let a = SrcOperand::ureg(insn.bits(24, 8));
            let b = if raw_opcode == 0x890 {
                SrcOperand::Imm32(insn.words[1])
            } else {
                SrcOperand::ureg(insn.bits(32, 8))
            };
            let c = SrcOperand::ureg(insn.bits(64, 8));
            DecodedOp::Uiadd3 {
                dst_ur: unif_dst,
                a,
                b,
                c,
                neg_a: insn.bits(72, 1) != 0,
                neg_b: insn.bits(63, 1) != 0,
                x_mode: insn.bits(74, 1) != 0,
                carry_in: insn.bits(87, 4) as u8,
                carry_out: insn.bits(81, 3) as u8,
            }
        }
        0x224 | 0x424 | 0x624 | 0x824 | 0xa24 | 0xc24 | 0xe24 | 0x225 | 0x227 | 0x625 | 0x825
        | 0x827 | 0xa27 => {
            let Some((a, _, _)) = decode_alu_operand(insn, 0) else {
                return unsup(raw_opcode, "invalid IMAD a");
            };
            let Some((b, _, _)) = decode_alu_operand(insn, 1) else {
                return unsup(raw_opcode, "invalid IMAD b");
            };
            let Some((c, _, _)) = decode_alu_operand(insn, 2) else {
                return unsup(raw_opcode, "invalid IMAD c");
            };
            let low = raw_opcode & 0xff;
            let wide = low == 0x25;
            let hi = low == 0x27;
            DecodedOp::Imad {
                dst,
                a,
                b,
                c,
                neg_c: insn.bits(75, 1) != 0,
                signed: insn.bits(73, 1) != 0,
                hi,
                wide,
            }
        }
        0x2a4 | 0x4a4 | 0x8a4 | 0x2a5 => {
            let a = SrcOperand::ureg(insn.bits(24, 8));
            let wide = (raw_opcode & 0xff) == 0xa5;
            let imm_b = raw_opcode == 0x8a4 || form == 4;
            let imm_c = form == 2;
            let b = if imm_b {
                SrcOperand::Imm32(insn.words[1])
            } else {
                SrcOperand::ureg(insn.bits(32, 8))
            };
            let c = if imm_c {
                SrcOperand::Imm32(insn.words[1])
            } else {
                SrcOperand::ureg(insn.bits(64, 8))
            };
            DecodedOp::Uimad {
                dst_ur: unif_dst,
                a,
                b,
                c,
                neg_c: insn.bits(75, 1) != 0,
                signed: insn.bits(73, 1) != 0,
                wide,
            }
        }
        0x211 | 0x811 | 0xc11 => {
            let Some((a, _, _)) = decode_alu_operand(insn, 0) else {
                return unsup(raw_opcode, "invalid LEA a");
            };
            let Some((b, _, _)) = decode_alu_operand(insn, 1) else {
                return unsup(raw_opcode, "invalid LEA b");
            };
            let Some((c_hi, _, _)) = decode_alu_operand(insn, 2) else {
                return unsup(raw_opcode, "invalid LEA c");
            };
            DecodedOp::Lea {
                dst,
                a,
                b,
                c_hi,
                scale: insn.bits(75, 5) as u8,
                hi: insn.bits(80, 1) != 0,
                neg_b: insn.bits(63, 1) != 0,
                x_mode: insn.bits(74, 1) != 0,
                carry_out: insn.bits(81, 3) as u8,
            }
        }
        0x291 | 0x891 => {
            let a = SrcOperand::ureg(insn.bits(24, 8));
            let b = if raw_opcode == 0x891 {
                SrcOperand::Imm32(insn.words[1])
            } else {
                SrcOperand::ureg(insn.bits(32, 8))
            };
            DecodedOp::Ulea {
                dst_ur: unif_dst,
                a,
                b,
                scale: insn.bits(75, 5) as u8,
                hi: insn.bits(80, 1) != 0,
                neg_b: insn.bits(63, 1) != 0,
            }
        }
        0x212 | 0x812 | 0xa12 | 0xc12 => {
            let Some((a, _, _)) = decode_alu_operand(insn, 0) else {
                return unsup(raw_opcode, "invalid LOP3 a");
            };
            let Some((b, _, _)) = decode_alu_operand(insn, 1) else {
                return unsup(raw_opcode, "invalid LOP3 b");
            };
            let Some((c, _, _)) = decode_alu_operand(insn, 2) else {
                return unsup(raw_opcode, "invalid LOP3 c");
            };
            DecodedOp::Lop3 {
                dst,
                a,
                b,
                c,
                lut: insn.bits(72, 8) as u8,
                pred_out: insn.bits(81, 3) as u8,
            }
        }
        0x292 | 0x892 => {
            let a = SrcOperand::ureg(insn.bits(24, 8));
            let b = if raw_opcode == 0x892 {
                SrcOperand::Imm32(insn.words[1])
            } else {
                SrcOperand::ureg(insn.bits(32, 8))
            };
            let c = SrcOperand::ureg(insn.bits(64, 8));
            DecodedOp::Ulop3 {
                dst_ur: unif_dst,
                a,
                b,
                c,
                lut: insn.bits(72, 8) as u8,
                pred_out: insn.bits(81, 3) as u8,
            }
        }
        0x219 | 0x419 | 0x819 | 0xa19 | 0xc19 => {
            let Some((a, _, _)) = decode_alu_operand(insn, 0) else {
                return unsup(raw_opcode, "invalid SHF a");
            };
            let Some((shift, _, _)) = decode_alu_operand(insn, 1) else {
                return unsup(raw_opcode, "invalid SHF shift");
            };
            let Some((c, _, _)) = decode_alu_operand(insn, 2) else {
                return unsup(raw_opcode, "invalid SHF c");
            };
            let ty = insn.bits(73, 2);
            if ty < 2 {
                return unsup(raw_opcode, "unsupported 64-bit SHF type");
            }
            DecodedOp::Shf {
                dst,
                a,
                shift,
                c,
                right: insn.bits(76, 1) != 0,
                hi: insn.bits(80, 1) != 0,
                signed: ty == 2,
                wrap: insn.bits(75, 1) != 0,
            }
        }
        0x299 | 0x499 | 0x899 => {
            let a = SrcOperand::ureg(insn.bits(24, 8));
            let shift = if raw_opcode == 0x899 || form == 4 {
                SrcOperand::Imm32(insn.words[1])
            } else {
                SrcOperand::ureg(insn.bits(32, 8))
            };
            let c = if raw_opcode == 0x499 || form == 2 {
                SrcOperand::Imm32(insn.words[1])
            } else {
                SrcOperand::ureg(insn.bits(64, 8))
            };
            let ty = insn.bits(73, 2);
            if ty < 2 {
                return unsup(raw_opcode, "unsupported 64-bit USHF type");
            }
            DecodedOp::Ushf {
                dst_ur: unif_dst,
                a,
                shift,
                c,
                right: insn.bits(76, 1) != 0,
                hi: insn.bits(80, 1) != 0,
                signed: ty == 2,
                wrap: insn.bits(75, 1) != 0,
            }
        }
        0x20c | 0x80c | 0xa0c | 0xc0c => {
            if insn.bits(72, 1) != 0 {
                return unsup(raw_opcode, "extended 64-bit ISETP.EX");
            }
            let Some((a, _, _)) = decode_alu_operand(insn, 0) else {
                return unsup(raw_opcode, "invalid ISETP a");
            };
            let Some((b, _, _)) = decode_alu_operand(insn, 1) else {
                return unsup(raw_opcode, "invalid ISETP b");
            };
            let Some(cmp) = decode_int_cmp(insn.bits(76, 3)) else {
                return unsup(raw_opcode, "invalid ISETP cmp");
            };
            let Some(comb) = decode_bool_comb(insn.bits(74, 2)) else {
                return unsup(raw_opcode, "invalid ISETP comb");
            };
            DecodedOp::Isetp {
                dst_p: insn.bits(81, 3) as u8,
                dst_q: insn.bits(84, 3) as u8,
                a,
                b,
                cmp,
                signed: insn.bits(73, 1) != 0,
                comb,
                accum: parse_guard(insn.bits(87, 4)),
            }
        }
        0x28c | 0x88c => {
            if insn.bits(72, 1) != 0 {
                return unsup(raw_opcode, "extended UISETP.EX");
            }
            let a = SrcOperand::ureg(insn.bits(24, 8));
            let b = if raw_opcode == 0x88c {
                SrcOperand::Imm32(insn.words[1])
            } else {
                SrcOperand::ureg(insn.bits(32, 8))
            };
            let Some(cmp) = decode_int_cmp(insn.bits(76, 3)) else {
                return unsup(raw_opcode, "invalid UISETP cmp");
            };
            let Some(comb) = decode_bool_comb(insn.bits(74, 2)) else {
                return unsup(raw_opcode, "invalid UISETP comb");
            };
            DecodedOp::Uisetp {
                dst_up: insn.bits(81, 3) as u8,
                a,
                b,
                cmp,
                signed: insn.bits(73, 1) != 0,
                comb,
                accum: parse_guard(insn.bits(87, 4)),
            }
        }
        0x217 | 0x817 | 0xa17 | 0xc17 => {
            let Some((a, _, _)) = decode_alu_operand(insn, 0) else {
                return unsup(raw_opcode, "invalid IMNMX a");
            };
            let Some((b, _, _)) = decode_alu_operand(insn, 1) else {
                return unsup(raw_opcode, "invalid IMNMX b");
            };
            DecodedOp::Imnmx {
                dst,
                a,
                b,
                pred_min: parse_guard(insn.bits(87, 4)),
                signed: insn.bits(73, 1) != 0,
            }
        }
        0x213 | 0xa13 | 0xc13 => {
            let Some((src, _, _)) = decode_alu_operand(insn, 1) else {
                return unsup(raw_opcode, "invalid IABS src");
            };
            DecodedOp::Iabs { dst, src }
        }
        0x309 | 0xd09 => {
            let src = if raw_opcode == 0xd09 {
                SrcOperand::ureg(insn.bits(32, 8))
            } else {
                let Some((s, _, _)) = decode_alu_operand(insn, 1) else {
                    return unsup(raw_opcode, "invalid POPC src");
                };
                s
            };
            DecodedOp::Popc { dst, src }
        }
        0x301 => {
            let Some((src, _, _)) = decode_alu_operand(insn, 1) else {
                return unsup(raw_opcode, "invalid BREV src");
            };
            DecodedOp::Brev { dst, src }
        }
        0x300 | 0xd00 => {
            let src = if raw_opcode == 0xd00 {
                SrcOperand::ureg(insn.bits(32, 8))
            } else {
                let Some((s, _, _)) = decode_alu_operand(insn, 1) else {
                    return unsup(raw_opcode, "invalid FLO src");
                };
                s
            };
            DecodedOp::Flo {
                dst,
                src,
                signed: insn.bits(73, 1) != 0,
                inv: insn.bits(63, 1) != 0,
                shamt: insn.bits(74, 1) != 0,
            }
        }
        0x2bd => DecodedOp::Uflo {
            dst_ur: unif_dst,
            src: SrcOperand::ureg(insn.bits(32, 8)),
            shamt: insn.bits(74, 1) != 0,
        },
        0x81a => DecodedOp::Sgxt {
            dst,
            src: SrcOperand::reg(insn.bits(24, 8)),
            width: insn.words[1] & 0x3f,
            signed: insn.bits(73, 1) != 0,
        },
        0x216 | 0x816 | 0xc16 => {
            let Some((a, _, _)) = decode_alu_operand(insn, 0) else {
                return unsup(raw_opcode, "invalid PRMT a");
            };
            let Some((c, _, _)) = decode_alu_operand(insn, 2) else {
                return unsup(raw_opcode, "invalid PRMT c");
            };
            let sel = if raw_opcode == 0x816 {
                insn.words[1] as u16
            } else {
                return unsup(raw_opcode, "non-immediate PRMT selector");
            };
            DecodedOp::Prmt { dst, a, sel, c }
        }

        // Float ALU
        0x221 | 0x421 | 0x621 | 0x821 | 0xa21 | 0xe21 => {
            let Some((a, abs_a, neg_a)) = decode_alu_operand(insn, 0) else {
                return unsup(raw_opcode, "invalid FADD a");
            };
            let b_which = if matches!(form, 2 | 3 | 7) { 2 } else { 1 };
            let Some((b, abs_b, neg_b)) = decode_alu_operand(insn, b_which) else {
                return unsup(raw_opcode, "invalid FADD b");
            };
            DecodedOp::Fadd {
                dst,
                a: FloatSrc { op: a, abs: abs_a, neg: neg_a },
                b: FloatSrc { op: b, abs: abs_b, neg: neg_b },
                sat: insn.bits(77, 1) != 0,
            }
        }
        0x220 | 0x820 | 0xa20 | 0xc20 => {
            if insn.bits(84, 3) != 4 {
                return unsup(raw_opcode, "non-unit FMUL scale");
            }
            let Some((a, abs_a, neg_a)) = decode_alu_operand(insn, 0) else {
                return unsup(raw_opcode, "invalid FMUL a");
            };
            let Some((b, abs_b, neg_b)) = decode_alu_operand(insn, 1) else {
                return unsup(raw_opcode, "invalid FMUL b");
            };
            DecodedOp::Fmul {
                dst,
                a: FloatSrc { op: a, abs: abs_a, neg: neg_a },
                b: FloatSrc { op: b, abs: abs_b, neg: neg_b },
                sat: insn.bits(77, 1) != 0,
            }
        }
        0x223 | 0x423 | 0x623 | 0x823 | 0xa23 | 0xc23 | 0xe23 => {
            let Some((a, abs_a, neg_a)) = decode_alu_operand(insn, 0) else {
                return unsup(raw_opcode, "invalid FFMA a");
            };
            let Some((b, abs_b, neg_b)) = decode_alu_operand(insn, 1) else {
                return unsup(raw_opcode, "invalid FFMA b");
            };
            let Some((c, abs_c, neg_c)) = decode_alu_operand(insn, 2) else {
                return unsup(raw_opcode, "invalid FFMA c");
            };
            DecodedOp::Ffma {
                dst,
                a: FloatSrc { op: a, abs: abs_a, neg: neg_a },
                b: FloatSrc { op: b, abs: abs_b, neg: neg_b },
                c: FloatSrc { op: c, abs: abs_c, neg: neg_c },
                sat: insn.bits(77, 1) != 0,
            }
        }
        0x209 | 0x809 | 0xa09 | 0xc09 => {
            let Some((a, abs_a, neg_a)) = decode_alu_operand(insn, 0) else {
                return unsup(raw_opcode, "invalid FMNMX a");
            };
            let Some((b, abs_b, neg_b)) = decode_alu_operand(insn, 1) else {
                return unsup(raw_opcode, "invalid FMNMX b");
            };
            DecodedOp::Fmnmx {
                dst,
                a: FloatSrc { op: a, abs: abs_a, neg: neg_a },
                b: FloatSrc { op: b, abs: abs_b, neg: neg_b },
                pred_min: parse_guard(insn.bits(87, 4)),
            }
        }
        0x208 | 0x808 | 0xa08 => {
            let Some((a, abs_a, neg_a)) = decode_alu_operand(insn, 0) else {
                return unsup(raw_opcode, "invalid FSEL a");
            };
            let Some((b, abs_b, neg_b)) = decode_alu_operand(insn, 1) else {
                return unsup(raw_opcode, "invalid FSEL b");
            };
            DecodedOp::Fsel {
                dst,
                a: FloatSrc { op: a, abs: abs_a, neg: neg_a },
                b: FloatSrc { op: b, abs: abs_b, neg: neg_b },
                pred: parse_guard(insn.bits(87, 4)),
            }
        }
        0x20b | 0x80b | 0xa0b | 0xc0b => {
            let Some((a, abs_a, neg_a)) = decode_alu_operand(insn, 0) else {
                return unsup(raw_opcode, "invalid FSETP a");
            };
            let Some((b, abs_b, neg_b)) = decode_alu_operand(insn, 1) else {
                return unsup(raw_opcode, "invalid FSETP b");
            };
            let Some(cmp) = decode_float_cmp(insn.bits(76, 4)) else {
                return unsup(raw_opcode, "invalid FSETP cmp");
            };
            let Some(comb) = decode_bool_comb(insn.bits(74, 2)) else {
                return unsup(raw_opcode, "invalid FSETP comb");
            };
            DecodedOp::Fsetp {
                dst_p: insn.bits(81, 3) as u8,
                dst_q: insn.bits(84, 3) as u8,
                a: FloatSrc { op: a, abs: abs_a, neg: neg_a },
                b: FloatSrc { op: b, abs: abs_b, neg: neg_b },
                cmp,
                comb,
                accum: parse_guard(insn.bits(87, 4)),
            }
        }
        0x308 | 0xb08 | 0xd08 => {
            let op_bits = insn.bits(74, 6);
            let op = match op_bits {
                0 => MufuOp::CosTurns,
                1 => MufuOp::SinTurns,
                2 => MufuOp::Ex2,
                3 => MufuOp::Lg2,
                4 => MufuOp::Rcp,
                5 => MufuOp::Rsq,
                8 => MufuOp::Sqrt,
                _ => return unsup(raw_opcode, "unsupported MUFU operation"),
            };
            if insn.bits(73, 1) != 0 {
                return unsup(raw_opcode, "half MUFU mode");
            }
            let (src, abs, neg) = if raw_opcode == 0xd08 {
                (SrcOperand::ureg(insn.bits(32, 8)), false, false)
            } else {
                let Some(s) = decode_alu_operand(insn, 1) else {
                    return unsup(raw_opcode, "invalid MUFU src");
                };
                s
            };
            DecodedOp::Mufu {
                dst,
                src,
                op,
                abs,
                neg,
                sat: insn.bits(77, 1) != 0,
            }
        }
        0x306 | 0x906 | 0xd06 => {
            let is_signed = insn.bits(74, 1) != 0;
            let dst_f16 = insn.bits(76, 2) == 1 && insn.bits(72, 1) == 0;
            let src_fmt = insn.bits(72, 2);
            let src_bits = match insn.bits(75, 2) {
                0 => 8,
                1 => 16,
                _ => 32,
            };
            let _ = src_fmt;
            let src = match raw_opcode {
                0x906 => SrcOperand::Imm32(insn.words[1]),
                0xd06 => SrcOperand::ureg(insn.bits(32, 8)),
                _ => SrcOperand::reg(insn.bits(32, 8)),
            };
            let _ = is_signed;
            DecodedOp::I2f {
                dst,
                src,
                src_signed: insn.bits(74, 1) != 0,
                src_bits: if insn.bits(84, 1) != 0 {
                    16
                } else if insn.bits(75, 2) == 2 || insn.bits(85, 1) != 0 {
                    32
                } else {
                    src_bits
                },
                byte_lane: insn.bits(60, 2) as u8,
                dst_f16,
                abs: insn.bits(62, 1) != 0,
            }
        }
        0x305 | 0xb05 | 0xd05 => {
            let src = match raw_opcode {
                0xb05 => SrcOperand::Cbuf(match cbuf_from_insn(insn) {
                    Some(c) => c,
                    None => return unsup(raw_opcode, "invalid F2I cbuf"),
                }),
                0xd05 => SrcOperand::ureg(insn.bits(32, 8)),
                _ => SrcOperand::reg(insn.bits(32, 8)),
            };
            let dst_bits = match insn.bits(75, 2) {
                0 => 8,
                1 => 16,
                2 => 32,
                3 => 64,
                _ => 32,
            };
            if dst_bits == 64 {
                return unsup(raw_opcode, "F2I 64-bit destination");
            }
            DecodedOp::F2i {
                dst,
                src,
                dst_signed: insn.bits(74, 1) != 0,
                dst_bits,
                src_f16: insn.bits(84, 1) != 0,
                half_lane: insn.bits(60, 2) as u8,
                abs: insn.bits(62, 1) != 0,
                neg: insn.bits(63, 1) != 0,
                round_mode: insn.bits(78, 2) as u8,
            }
        }
        0x304 => {
            if insn.words[2] != 0x0020_0800 {
                return unsup(raw_opcode, "unsupported F2F conversion mode");
            }
            DecodedOp::F2f {
                dst,
                src: SrcOperand::reg(insn.bits(32, 8)),
                src_bits: 16,
                dst_bits: 32,
                half_lane: insn.bits(60, 2) as u8,
                sat: false,
                abs: insn.bits(62, 1) != 0,
                neg: insn.bits(63, 1) != 0,
            }
        }

        // Packed FP16x2 ALU
        0x230 | 0x430 | 0x630 => {
            let Some(a) = decode_half_operand(insn, 0) else {
                return unsup(raw_opcode, "invalid HADD2 a");
            };
            let b_which = if raw_opcode == 0x230 { 1 } else { 2 };
            let Some(b) = decode_half_operand(insn, b_which) else {
                return unsup(raw_opcode, "invalid HADD2 b");
            };
            DecodedOp::Hadd2 {
                dst,
                a,
                b,
                sat: insn.bits(77, 1) != 0,
                f32_out: insn.bits(78, 1) != 0,
            }
        }
        0x232 | 0x832 | 0xa32 => {
            let Some(a) = decode_half_operand(insn, 0) else {
                return unsup(raw_opcode, "invalid HMUL2 a");
            };
            let Some(b) = decode_half_operand(insn, 1) else {
                return unsup(raw_opcode, "invalid HMUL2 b");
            };
            DecodedOp::Hmul2 {
                dst,
                a,
                b,
                sat: insn.bits(77, 1) != 0,
            }
        }
        0x231 | 0x431 | 0x631 | 0x831 | 0xa31 => {
            let Some(a) = decode_half_operand(insn, 0) else {
                return unsup(raw_opcode, "invalid HFMA2 a");
            };
            let Some(b) = decode_half_operand(insn, 1) else {
                return unsup(raw_opcode, "invalid HFMA2 b");
            };
            let Some(c) = decode_half_operand(insn, 2) else {
                return unsup(raw_opcode, "invalid HFMA2 c");
            };
            DecodedOp::Hfma2 {
                dst,
                a,
                b,
                c,
                sat: insn.bits(77, 1) != 0,
                relu: insn.bits(87, 4) != 7 && insn.bits(81, 1) != 0,
            }
        }

        // Constant loads (LDC / ULDC)
        0xb82 => {
            let Some(width) = MemWidth::from_bits(insn.bits(73, 3)) else {
                return unsup(raw_opcode, "invalid LDC width");
            };
            let mode = insn.bits(78, 2);
            if mode != 0 && mode != 2 {
                return unsup(raw_opcode, "unsupported LDC addressing mode");
            }
            let Some(bank) = bank_from_insn(insn) else {
                return unsup(raw_opcode, "invalid LDC cbuf bank");
            };
            let idx = insn.bits(24, 8);
            DecodedOp::Ldc {
                dst,
                bank,
                offset: insn.bits(38, 16) as u16,
                index_reg: if idx == 255 { None } else { Some(idx as u8) },
                width,
            }
        }
        0xab9 | 0xabb => {
            let Some(width) = MemWidth::from_bits(insn.bits(73, 3)) else {
                return unsup(raw_opcode, "invalid ULDC width");
            };
            let Some(bank) = bank_from_insn(insn) else {
                return unsup(raw_opcode, "invalid ULDC cbuf bank");
            };
            if unif_dst == 63 {
                return DecodedOp::Nop;
            }
            DecodedOp::Uldc {
                dst_ur: unif_dst,
                bank,
                offset: insn.bits(38, 16) as u16,
                width,
            }
        }

        // Global memory
        0x381 | 0x981 => {
            let Some(width) = MemWidth::from_bits(insn.bits(73, 3)) else {
                return unsup(raw_opcode, "invalid LDG width");
            };
            let imm_offset = ((insn.bits(40, 24) as i32) << 8) >> 8;
            if raw_opcode == 0x381 {
                DecodedOp::Ldg {
                    dst,
                    base_reg: insn.bits(24, 8) as u8,
                    uniform_reg: None,
                    imm_offset,
                    width,
                    reg_64bit: true,
                    coherent: insn.bits(77, 1) != 0,
                    descriptor_mode: false,
                }
            } else {
                let ur = insn.bits(32, 6);
                let descriptor_mode = (insn.words[2] & !0xe3fu32) == 0x0c1e_1100;
                DecodedOp::Ldg {
                    dst,
                    base_reg: insn.bits(24, 8) as u8,
                    uniform_reg: if ur == 63 { None } else { Some(ur as u8) },
                    imm_offset,
                    width,
                    reg_64bit: insn.bits(90, 1) != 0,
                    coherent: insn.bits(77, 1) != 0,
                    descriptor_mode,
                }
            }
        }
        0x386 | 0x986 => {
            let Some(width) = MemWidth::from_bits(insn.bits(73, 3)) else {
                return unsup(raw_opcode, "invalid STG width");
            };
            let imm_offset = ((insn.bits(40, 24) as i32) << 8) >> 8;
            if raw_opcode == 0x386 {
                DecodedOp::Stg {
                    src_reg: insn.bits(32, 8) as u8,
                    base_reg: insn.bits(24, 8) as u8,
                    uniform_reg: None,
                    imm_offset,
                    width,
                    reg_64bit: true,
                    descriptor_mode: false,
                }
            } else {
                let ur = insn.bits(64, 6);
                let descriptor_mode = (insn.words[2] & !0xe3fu32) == 0x0c10_1100;
                DecodedOp::Stg {
                    src_reg: insn.bits(32, 8) as u8,
                    base_reg: insn.bits(24, 8) as u8,
                    uniform_reg: if ur == 63 { None } else { Some(ur as u8) },
                    imm_offset,
                    width,
                    reg_64bit: insn.bits(90, 1) != 0,
                    descriptor_mode,
                }
            }
        }

        // Shared memory
        0x984 => {
            let Some(width) = MemWidth::from_bits(insn.bits(73, 3)) else {
                return unsup(raw_opcode, "invalid LDS width");
            };
            let uniform_base = insn.bits(91, 1) != 0;
            let ur = if uniform_base {
                Some((insn.bits(32, 8) & 63) as u8)
            } else {
                None
            };
            DecodedOp::Lds {
                dst,
                base_reg: insn.bits(24, 8) as u8,
                uniform_reg: ur.filter(|&u| u != 63),
                imm_offset: ((insn.bits(40, 24) as i32) << 8) >> 8,
                stride: insn.bits(78, 2) as u8,
                width,
            }
        }
        0x388 | 0x988 => {
            let Some(width) = MemWidth::from_bits(insn.bits(73, 3)) else {
                return unsup(raw_opcode, "invalid STS width");
            };
            let uniform_base = raw_opcode == 0x988 && insn.bits(91, 1) != 0;
            let ur = if uniform_base {
                Some((insn.bits(64, 8) & 63) as u8)
            } else {
                None
            };
            DecodedOp::Sts {
                src_reg: insn.bits(32, 8) as u8,
                base_reg: insn.bits(24, 8) as u8,
                uniform_reg: ur.filter(|&u| u != 63),
                imm_offset: ((insn.bits(40, 24) as i32) << 8) >> 8,
                stride: insn.bits(78, 2) as u8,
                width,
            }
        }

        // Local memory spill
        0x983 => {
            let Some(width) = MemWidth::from_bits(insn.bits(73, 3)) else {
                return unsup(raw_opcode, "invalid LDL width");
            };
            DecodedOp::Ldl {
                dst,
                base_reg: insn.bits(24, 8) as u8,
                offset: insn.bits(40, 24),
                width,
            }
        }
        0x387 => {
            let Some(width) = MemWidth::from_bits(insn.bits(73, 3)) else {
                return unsup(raw_opcode, "invalid STL width");
            };
            DecodedOp::Stl {
                src_reg: insn.bits(32, 8) as u8,
                base_reg: insn.bits(24, 8) as u8,
                offset: insn.bits(40, 24),
                width,
            }
        }

        // Atomics
        0x38c => {
            let Some(op) = decode_atomic_op(insn.bits(87, 4)) else {
                return unsup(raw_opcode, "unsupported ATOMS op");
            };
            DecodedOp::Atoms {
                dst,
                base_reg: insn.bits(24, 8) as u8,
                imm_offset: ((insn.bits(40, 24) as i32) << 8) >> 8,
                src_a: insn.bits(32, 8) as u8,
                op,
                is_64bit: insn.bits(73, 1) != 0,
            }
        }
        0x3a8 | 0x9a8 | 0x98e => {
            if raw_opcode == 0x98e {
                return unsup(raw_opcode, "RED atomic requires dedicated opcode check");
            }
            let ur = if raw_opcode == 0x9a8 {
                let u = insn.bits(64, 6) as u8;
                if u == 63 { None } else { Some(u) }
            } else {
                None
            };
            DecodedOp::Atomg {
                dst: if dst == 255 { None } else { Some(dst) },
                base_reg: insn.bits(24, 8) as u8,
                uniform_reg: ur,
                imm_offset: ((insn.bits(40, 24) as i32) << 8) >> 8,
                src_a: insn.bits(32, 8) as u8,
                op: AtomicOp::Add,
            }
        }

        // Warp ops & barriers
        0x589 | 0x989 | 0xf89 => {
            let mode = match insn.bits(58, 2) {
                0 => ShflMode::Idx,
                1 => ShflMode::Up,
                2 => ShflMode::Down,
                3 => ShflMode::Bfly,
                _ => unreachable!(),
            };
            let lane = if raw_opcode == 0x589 {
                SrcOperand::reg(insn.bits(32, 8))
            } else {
                SrcOperand::Imm32(insn.bits(53, 5))
            };
            let mask = if raw_opcode == 0x989 {
                SrcOperand::reg(insn.bits(64, 8))
            } else {
                SrcOperand::Imm32(insn.bits(40, 13))
            };
            DecodedOp::Shfl {
                dst,
                pred_out: insn.bits(81, 3) as u8,
                src: insn.bits(24, 8) as u8,
                lane,
                mask,
                mode,
            }
        }
        0x806 | 0x886 => {
            let mode = match insn.bits(72, 2) {
                0 => VoteMode::All,
                1 => VoteMode::Any,
                2 => VoteMode::Eq,
                _ => return unsup(raw_opcode, "unsupported VOTE mode"),
            };
            DecodedOp::Vote {
                dst_reg: if raw_opcode == 0x886 { unif_dst } else { dst },
                dst_uniform: raw_opcode == 0x886,
                pred_out: insn.bits(81, 3) as u8,
                src_pred: parse_guard(insn.bits(87, 4)),
                mode,
            }
        }
        0xb1d => DecodedOp::BarSync {
            defer_blocking: insn.bits(80, 1) != 0,
        },
        0x992 => {
            let scope = match insn.bits(76, 3) {
                0 | 2 => MemBarrierScope::Workgroup,
                3 => MemBarrierScope::Device,
                5 => MemBarrierScope::System,
                _ => return unsup(raw_opcode, "unsupported MEMBAR scope"),
            };
            DecodedOp::Membar { scope }
        }

        // Tensor core (HMMA)
        0x23c => {
            let shape = insn.words[2] & !0xffu32;
            if shape != 0 && shape != 0x800 {
                return unsup(raw_opcode, "unsupported HMMA shape");
            }
            DecodedOp::Hmma {
                dst,
                ra: insn.bits(24, 8) as u8,
                rb: insn.bits(32, 8) as u8,
                rc: insn.bits(64, 8) as u8,
                depth: if shape == 0 { 8 } else { 16 },
            }
        }

        _ => unsup(raw_opcode, "unimplemented SM86 opcode"),
    }
}
