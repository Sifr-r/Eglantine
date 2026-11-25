// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::collections::{BTreeMap, BTreeSet};

use crate::coop_matrix::{
    emit_native_coop_hmma, CoopSpirvBuilder, CAPABILITY_COOPERATIVE_MATRIX_KHR,
    OP_TYPE_COOPERATIVE_MATRIX_KHR, SCOPE_SUBGROUP,
};
use crate::decode_sm86::{AtomicOp, FloatCmp, IntCmp, MemBarrierScope, S2rSpecial, ShflMode, VoteMode};
use crate::reflect::{SPIRV_1_6_VERSION};
use crate::ssa_ir::{GlslOp, MergeHint, SsaOp, SsaProgram, Terminator, ValType, ValueId};

pub const SPIRV_MAGIC: u32 = 0x0723_0203;
const GENERATOR_ID: u32 = 0x4547_4C00; // "EGL\0"

const OP_NAME: u16 = 5;
const OP_EXTENSION: u16 = 10;
const OP_EXT_INST_IMPORT: u16 = 11;
const OP_EXT_INST: u16 = 12;
const OP_MEMORY_MODEL: u16 = 14;
const OP_ENTRY_POINT: u16 = 15;
const OP_EXECUTION_MODE: u16 = 16;
const OP_CAPABILITY: u16 = 17;

const OP_TYPE_VOID: u16 = 19;
const OP_TYPE_BOOL: u16 = 20;
const OP_TYPE_INT: u16 = 21;
const OP_TYPE_FLOAT: u16 = 22;
const OP_TYPE_VECTOR: u16 = 23;
const OP_TYPE_ARRAY: u16 = 28;
const OP_TYPE_RUNTIME_ARRAY: u16 = 29;
const OP_TYPE_STRUCT: u16 = 30;
const OP_TYPE_POINTER: u16 = 32;
const OP_TYPE_FUNCTION: u16 = 33;

const OP_CONSTANT_TRUE: u16 = 41;
const OP_CONSTANT_FALSE: u16 = 42;
const OP_CONSTANT: u16 = 43;
const OP_UNDEF: u16 = 44;

const OP_FUNCTION: u16 = 54;
const OP_FUNCTION_END: u16 = 56;
const OP_VARIABLE: u16 = 59;
const OP_LOAD: u16 = 61;
const OP_STORE: u16 = 62;
const OP_ACCESS_CHAIN: u16 = 65;
const OP_DECORATE: u16 = 71;
const OP_MEMBER_DECORATE: u16 = 72;

const OP_COMPOSITE_CONSTRUCT: u16 = 80;
const OP_COMPOSITE_EXTRACT: u16 = 81;
const OP_COPY_OBJECT: u16 = 83;

const OP_CONVERT_F_TO_U: u16 = 109;
const OP_CONVERT_F_TO_S: u16 = 110;
const OP_CONVERT_S_TO_F: u16 = 111;
const OP_CONVERT_U_TO_F: u16 = 112;
const OP_F_CONVERT: u16 = 115;
const OP_BITCAST: u16 = 124;
const OP_S_NEGATE: u16 = 126;
const OP_F_NEGATE: u16 = 127;
const OP_I_ADD: u16 = 128;
const OP_F_ADD: u16 = 129;
const OP_I_SUB: u16 = 130;
const OP_F_SUB: u16 = 131;
const OP_I_MUL: u16 = 132;
const OP_F_MUL: u16 = 133;
const OP_F_DIV: u16 = 136;
const OP_U_MUL_EXTENDED: u16 = 151;
const OP_S_MUL_EXTENDED: u16 = 152;
const OP_IS_NAN: u16 = 156;

const OP_LOGICAL_EQUAL: u16 = 164;
const OP_LOGICAL_OR: u16 = 166;
const OP_LOGICAL_AND: u16 = 167;
const OP_LOGICAL_NOT: u16 = 168;
const OP_SELECT: u16 = 169;
const OP_I_EQUAL: u16 = 170;
const OP_I_NOT_EQUAL: u16 = 171;
const OP_U_GREATER_THAN: u16 = 172;
const OP_S_GREATER_THAN: u16 = 173;
const OP_U_GREATER_THAN_EQUAL: u16 = 174;
const OP_S_GREATER_THAN_EQUAL: u16 = 175;
const OP_U_LESS_THAN: u16 = 176;
const OP_S_LESS_THAN: u16 = 177;
const OP_U_LESS_THAN_EQUAL: u16 = 178;
const OP_S_LESS_THAN_EQUAL: u16 = 179;

const OP_F_ORD_EQUAL: u16 = 180;
const OP_F_UNORD_EQUAL: u16 = 181;
const OP_F_ORD_NOT_EQUAL: u16 = 182;
const OP_F_UNORD_NOT_EQUAL: u16 = 183;
const OP_F_ORD_LESS_THAN: u16 = 184;
const OP_F_UNORD_LESS_THAN: u16 = 185;
const OP_F_ORD_GREATER_THAN: u16 = 186;
const OP_F_UNORD_GREATER_THAN: u16 = 187;
const OP_F_ORD_LESS_THAN_EQUAL: u16 = 188;
const OP_F_UNORD_LESS_THAN_EQUAL: u16 = 189;
const OP_F_ORD_GREATER_THAN_EQUAL: u16 = 190;
const OP_F_UNORD_GREATER_THAN_EQUAL: u16 = 191;

const OP_SHIFT_RIGHT_LOGICAL: u16 = 194;
const OP_SHIFT_RIGHT_ARITHMETIC: u16 = 195;
const OP_SHIFT_LEFT_LOGICAL: u16 = 196;
const OP_BITWISE_OR: u16 = 197;
const OP_BITWISE_XOR: u16 = 198;
const OP_BITWISE_AND: u16 = 199;
const OP_NOT: u16 = 200;
const OP_BIT_FIELD_S_EXTRACT: u16 = 202;
const OP_BIT_FIELD_U_EXTRACT: u16 = 203;
const OP_BIT_REVERSE: u16 = 204;
const OP_BIT_COUNT: u16 = 205;

const OP_CONTROL_BARRIER: u16 = 224;
const OP_MEMORY_BARRIER: u16 = 225;
const OP_ATOMIC_EXCHANGE: u16 = 229;
const OP_ATOMIC_I_ADD: u16 = 234;
const OP_ATOMIC_S_MIN: u16 = 236;
const OP_ATOMIC_U_MIN: u16 = 237;
const OP_ATOMIC_S_MAX: u16 = 238;
const OP_ATOMIC_U_MAX: u16 = 239;
const OP_ATOMIC_AND: u16 = 240;
const OP_ATOMIC_OR: u16 = 241;
const OP_ATOMIC_XOR: u16 = 242;

const OP_PHI: u16 = 245;
const OP_LOOP_MERGE: u16 = 246;
const OP_SELECTION_MERGE: u16 = 247;
const OP_LABEL: u16 = 248;
const OP_BRANCH: u16 = 249;
const OP_BRANCH_CONDITIONAL: u16 = 250;
const OP_RETURN: u16 = 253;
const OP_UNREACHABLE: u16 = 255;

const OP_GROUP_NON_UNIFORM_ALL: u16 = 334;
const OP_GROUP_NON_UNIFORM_ANY: u16 = 335;
const OP_GROUP_NON_UNIFORM_ALL_EQUAL: u16 = 336;
const OP_GROUP_NON_UNIFORM_BALLOT: u16 = 339;
const OP_GROUP_NON_UNIFORM_SHUFFLE: u16 = 345;
const OP_GROUP_NON_UNIFORM_SHUFFLE_XOR: u16 = 346;
const OP_GROUP_NON_UNIFORM_SHUFFLE_UP: u16 = 347;
const OP_GROUP_NON_UNIFORM_SHUFFLE_DOWN: u16 = 348;

const CAP_SHADER: u32 = 1;
const CAP_FLOAT16: u32 = 9;
const CAP_INT64: u32 = 11;
const CAP_GROUP_NON_UNIFORM: u32 = 61;
const CAP_GROUP_NON_UNIFORM_VOTE: u32 = 62;
const CAP_GROUP_NON_UNIFORM_BALLOT: u32 = 64;
const CAP_GROUP_NON_UNIFORM_SHUFFLE: u32 = 65;
const CAP_GROUP_NON_UNIFORM_SHUFFLE_RELATIVE: u32 = 66;
const CAP_VULKAN_MEMORY_MODEL: u32 = 5345;
const CAP_VULKAN_MEMORY_MODEL_DEVICE_SCOPE: u32 = 5346;

const EXEC_MODEL_GL_COMPUTE: u32 = 5;
const EXEC_MODE_LOCAL_SIZE: u32 = 17;

const SC_INPUT: u32 = 1;
const SC_UNIFORM: u32 = 2;
const SC_WORKGROUP: u32 = 4;
const SC_FUNCTION: u32 = 7;
const SC_STORAGE_BUFFER: u32 = 12;

const DEC_BLOCK: u32 = 2;
const DEC_ARRAY_STRIDE: u32 = 6;
const DEC_BUILTIN: u32 = 11;
const DEC_NON_WRITABLE: u32 = 24;
const DEC_BINDING: u32 = 33;
const DEC_DESCRIPTOR_SET: u32 = 34;
const DEC_OFFSET: u32 = 35;

const BI_WORKGROUP_ID: u32 = 26;
const BI_LOCAL_INVOCATION_ID: u32 = 27;
const BI_GLOBAL_INVOCATION_ID: u32 = 28;
const BI_SUBGROUP_LOCAL_INVOCATION_ID: u32 = 41;
const BI_SUBGROUP_EQ_MASK: u32 = 4416;
const BI_SUBGROUP_GE_MASK: u32 = 4417;
const BI_SUBGROUP_GT_MASK: u32 = 4418;
const BI_SUBGROUP_LE_MASK: u32 = 4419;
const BI_SUBGROUP_LT_MASK: u32 = 4420;

const SCOPE_DEVICE: u32 = 1;
const SCOPE_WORKGROUP: u32 = 2;
const SCOPE_SYSTEM_CROSS_DEVICE: u32 = 0;

const SEM_NONE: u32 = 0;
const SEM_ACQUIRE_RELEASE: u32 = 0x8;
const SEM_UNIFORM_MEMORY: u32 = 0x40;
const SEM_WORKGROUP_MEMORY: u32 = 0x100;

fn push_inst(buf: &mut Vec<u32>, opcode: u16, words: &[u32]) {
    let count = (words.len() + 1) as u32;
    buf.push((count << 16) | u32::from(opcode));
    buf.extend_from_slice(words);
}

fn encode_str_words(s: &str) -> Vec<u32> {
    let bytes = s.as_bytes();
    let total = bytes.len() + 1;
    let word_len = total.div_ceil(4);
    let mut out = vec![0u32; word_len];
    for (i, &b) in bytes.iter().enumerate() {
        out[i / 4] |= u32::from(b) << ((i % 4) * 8);
    }
    out
}

fn glsl_op_code(op: GlslOp) -> u32 {
    match op {
        GlslOp::RoundEven => 2,
        GlslOp::Floor => 8,
        GlslOp::Ceil => 9,
        GlslOp::SAbs => 5,
        GlslOp::Sin => 13,
        GlslOp::Cos => 14,
        GlslOp::Exp2 => 29,
        GlslOp::Log2 => 30,
        GlslOp::Sqrt => 31,
        GlslOp::InverseSqrt => 32,
        GlslOp::FMin => 37,
        GlslOp::UMin => 38,
        GlslOp::SMin => 39,
        GlslOp::FMax => 40,
        GlslOp::UMax => 41,
        GlslOp::SMax => 42,
        GlslOp::PackHalf2x16 => 58,
        GlslOp::UnpackHalf2x16 => 62,
        GlslOp::FindILsb => 73,
        GlslOp::FindSMsb => 74,
        GlslOp::FindUMsb => 75,
    }
}

struct SpirvModule {
    next_id: u32,
    capabilities: BTreeSet<u32>,
    extensions: BTreeSet<&'static str>,
    glsl_ext_id: u32,
    main_fn_id: u32,
    interface_vars: Vec<u32>,
    names: Vec<u32>,
    decorations: Vec<u32>,
    types_consts_globals: Vec<u32>,
    fn_decls: Vec<u32>,
    fn_body: Vec<u32>,

    // Deduplicated core types & constants
    ty_void: Option<u32>,
    ty_bool: Option<u32>,
    ty_u32: Option<u32>,
    ty_f16: Option<u32>,
    ty_f32: Option<u32>,
    ty_u64: Option<u32>,
    ty_f16x2: Option<u32>,
    ty_u32x3: Option<u32>,
    ty_u32x4: Option<u32>,
    ty_u32_pair_struct: Option<u32>,
    ptr_types: BTreeMap<(u32, u32), u32>,
    const_bool: [Option<u32>; 2],
    const_u32: BTreeMap<u32, u32>,
    const_u64: BTreeMap<u64, u32>,
    coop_types: BTreeMap<(u32, u32, u32, u32), u32>,

    // Descriptor & storage variable caches
    cbuf_vars: BTreeMap<u32, u32>,
    ssbo_vars: BTreeMap<usize, u32>,
    shared_var: Option<(u32, u32)>, // (var_id, ptr_u32_ty)
    local_var: Option<(u32, u32)>,
    builtin_vars: BTreeMap<u32, u32>,
}

impl SpirvModule {
    fn new() -> Self {
        let mut m = Self {
            next_id: 1,
            capabilities: BTreeSet::new(),
            extensions: BTreeSet::new(),
            glsl_ext_id: 0,
            main_fn_id: 0,
            interface_vars: Vec::new(),
            names: Vec::new(),
            decorations: Vec::new(),
            types_consts_globals: Vec::new(),
            fn_decls: Vec::new(),
            fn_body: Vec::new(),
            ty_void: None,
            ty_bool: None,
            ty_u32: None,
            ty_f16: None,
            ty_f32: None,
            ty_u64: None,
            ty_f16x2: None,
            ty_u32x3: None,
            ty_u32x4: None,
            ty_u32_pair_struct: None,
            ptr_types: BTreeMap::new(),
            const_bool: [None, None],
            const_u32: BTreeMap::new(),
            const_u64: BTreeMap::new(),
            coop_types: BTreeMap::new(),
            cbuf_vars: BTreeMap::new(),
            ssbo_vars: BTreeMap::new(),
            shared_var: None,
            local_var: None,
            builtin_vars: BTreeMap::new(),
        };
        m.capabilities.insert(CAP_SHADER);
        m.capabilities.insert(CAP_VULKAN_MEMORY_MODEL);
        m.capabilities.insert(CAP_VULKAN_MEMORY_MODEL_DEVICE_SCOPE);
        m.glsl_ext_id = m.alloc();
        m.main_fn_id = m.alloc();
        m
    }

    #[inline]
    fn alloc(&mut self) -> u32 {
        let id = self.next_id;
        self.next_id += 1;
        id
    }

    fn decorate(&mut self, target: u32, dec: u32, args: &[u32]) {
        let mut ops = Vec::with_capacity(2 + args.len());
        ops.push(target);
        ops.push(dec);
        ops.extend_from_slice(args);
        push_inst(&mut self.decorations, OP_DECORATE, &ops);
    }

    fn member_decorate(&mut self, target: u32, member: u32, dec: u32, args: &[u32]) {
        let mut ops = Vec::with_capacity(3 + args.len());
        ops.push(target);
        ops.push(member);
        ops.push(dec);
        ops.extend_from_slice(args);
        push_inst(&mut self.decorations, OP_MEMBER_DECORATE, &ops);
    }

    fn name(&mut self, target: u32, text: &str) {
        let mut ops = Vec::new();
        ops.push(target);
        ops.extend(encode_str_words(text));
        push_inst(&mut self.names, OP_NAME, &ops);
    }

    fn ty_void(&mut self) -> u32 {
        if let Some(id) = self.ty_void {
            return id;
        }
        let id = self.alloc();
        push_inst(&mut self.types_consts_globals, OP_TYPE_VOID, &[id]);
        self.ty_void = Some(id);
        id
    }

    fn ty_bool(&mut self) -> u32 {
        if let Some(id) = self.ty_bool {
            return id;
        }
        let id = self.alloc();
        push_inst(&mut self.types_consts_globals, OP_TYPE_BOOL, &[id]);
        self.ty_bool = Some(id);
        id
    }

    fn ty_u32(&mut self) -> u32 {
        if let Some(id) = self.ty_u32 {
            return id;
        }
        let id = self.alloc();
        push_inst(&mut self.types_consts_globals, OP_TYPE_INT, &[id, 32, 0]);
        self.ty_u32 = Some(id);
        id
    }

    fn ty_f16(&mut self) -> u32 {
        if let Some(id) = self.ty_f16 {
            return id;
        }
        self.capabilities.insert(CAP_FLOAT16);
        let id = self.alloc();
        push_inst(&mut self.types_consts_globals, OP_TYPE_FLOAT, &[id, 16]);
        self.ty_f16 = Some(id);
        id
    }

    fn ty_f32(&mut self) -> u32 {
        if let Some(id) = self.ty_f32 {
            return id;
        }
        let id = self.alloc();
        push_inst(&mut self.types_consts_globals, OP_TYPE_FLOAT, &[id, 32]);
        self.ty_f32 = Some(id);
        id
    }

    fn ty_u64(&mut self) -> u32 {
        if let Some(id) = self.ty_u64 {
            return id;
        }
        self.capabilities.insert(CAP_INT64);
        let id = self.alloc();
        push_inst(&mut self.types_consts_globals, OP_TYPE_INT, &[id, 64, 0]);
        self.ty_u64 = Some(id);
        id
    }

    fn ty_f16x2(&mut self) -> u32 {
        if let Some(id) = self.ty_f16x2 {
            return id;
        }
        let f16 = self.ty_f16();
        let id = self.alloc();
        push_inst(&mut self.types_consts_globals, OP_TYPE_VECTOR, &[id, f16, 2]);
        self.ty_f16x2 = Some(id);
        id
    }

    fn ty_u32x3(&mut self) -> u32 {
        if let Some(id) = self.ty_u32x3 {
            return id;
        }
        let u32_t = self.ty_u32();
        let id = self.alloc();
        push_inst(&mut self.types_consts_globals, OP_TYPE_VECTOR, &[id, u32_t, 3]);
        self.ty_u32x3 = Some(id);
        id
    }

    fn ty_u32x4(&mut self) -> u32 {
        if let Some(id) = self.ty_u32x4 {
            return id;
        }
        let u32_t = self.ty_u32();
        let id = self.alloc();
        push_inst(&mut self.types_consts_globals, OP_TYPE_VECTOR, &[id, u32_t, 4]);
        self.ty_u32x4 = Some(id);
        id
    }

    fn ty_u32_pair_struct(&mut self) -> u32 {
        if let Some(id) = self.ty_u32_pair_struct {
            return id;
        }
        let u32_t = self.ty_u32();
        let id = self.alloc();
        push_inst(&mut self.types_consts_globals, OP_TYPE_STRUCT, &[id, u32_t, u32_t]);
        self.ty_u32_pair_struct = Some(id);
        id
    }

    fn ty_ptr(&mut self, storage: u32, pointee: u32) -> u32 {
        if let Some(&id) = self.ptr_types.get(&(storage, pointee)) {
            return id;
        }
        let id = self.alloc();
        push_inst(&mut self.types_consts_globals, OP_TYPE_POINTER, &[id, storage, pointee]);
        self.ptr_types.insert((storage, pointee), id);
        id
    }

    fn spv_ty(&mut self, ty: ValType) -> u32 {
        match ty {
            ValType::Void => self.ty_void(),
            ValType::Bool => self.ty_bool(),
            ValType::U32 => self.ty_u32(),
            ValType::F32 => self.ty_f32(),
            ValType::U64 => self.ty_u64(),
            ValType::F16x2 => self.ty_f16x2(),
        }
    }

    fn c_bool(&mut self, b: bool) -> u32 {
        let idx = usize::from(b);
        if let Some(id) = self.const_bool[idx] {
            return id;
        }
        let b_ty = self.ty_bool();
        let id = self.alloc();
        let op = if b { OP_CONSTANT_TRUE } else { OP_CONSTANT_FALSE };
        push_inst(&mut self.types_consts_globals, op, &[b_ty, id]);
        self.const_bool[idx] = Some(id);
        id
    }

    fn c_u32(&mut self, val: u32) -> u32 {
        if let Some(&id) = self.const_u32.get(&val) {
            return id;
        }
        let u_ty = self.ty_u32();
        let id = self.alloc();
        push_inst(&mut self.types_consts_globals, OP_CONSTANT, &[u_ty, id, val]);
        self.const_u32.insert(val, id);
        id
    }

    fn c_u64(&mut self, val: u64) -> u32 {
        if let Some(&id) = self.const_u64.get(&val) {
            return id;
        }
        let u_ty = self.ty_u64();
        let id = self.alloc();
        push_inst(
            &mut self.types_consts_globals,
            OP_CONSTANT,
            &[u_ty, id, val as u32, (val >> 32) as u32],
        );
        self.const_u64.insert(val, id);
        id
    }

    fn global_builtin_var(&mut self, builtin: u32, val_ty: u32) -> u32 {
        if let Some(&id) = self.builtin_vars.get(&builtin) {
            return id;
        }
        let ptr_ty = self.ty_ptr(SC_INPUT, val_ty);
        let var_id = self.alloc();
        push_inst(&mut self.types_consts_globals, OP_VARIABLE, &[ptr_ty, var_id, SC_INPUT]);
        self.decorate(var_id, DEC_BUILTIN, &[builtin]);
        self.interface_vars.push(var_id);
        self.builtin_vars.insert(builtin, var_id);
        var_id
    }

    fn emit_read_builtin(&mut self, special: S2rSpecial) -> u32 {
        let u32_t = self.ty_u32();
        match special {
            S2rSpecial::TidX | S2rSpecial::TidY | S2rSpecial::TidZ => {
                let comp = match special {
                    S2rSpecial::TidX => 0,
                    S2rSpecial::TidY => 1,
                    _ => 2,
                };
                let vec_t = self.ty_u32x3();
                let var = self.global_builtin_var(BI_LOCAL_INVOCATION_ID, vec_t);
                let vec_val = self.alloc();
                push_inst(&mut self.fn_body, OP_LOAD, &[vec_t, vec_val, var]);
                let out = self.alloc();
                push_inst(&mut self.fn_body, OP_COMPOSITE_EXTRACT, &[u32_t, out, vec_val, comp]);
                out
            }
            S2rSpecial::CtaIdX | S2rSpecial::CtaIdY | S2rSpecial::CtaIdZ => {
                let comp = match special {
                    S2rSpecial::CtaIdX => 0,
                    S2rSpecial::CtaIdY => 1,
                    _ => 2,
                };
                let vec_t = self.ty_u32x3();
                let var = self.global_builtin_var(BI_WORKGROUP_ID, vec_t);
                let vec_val = self.alloc();
                push_inst(&mut self.fn_body, OP_LOAD, &[vec_t, vec_val, var]);
                let out = self.alloc();
                push_inst(&mut self.fn_body, OP_COMPOSITE_EXTRACT, &[u32_t, out, vec_val, comp]);
                out
            }
            S2rSpecial::InvocationId => {
                let vec_t = self.ty_u32x3();
                let var = self.global_builtin_var(BI_GLOBAL_INVOCATION_ID, vec_t);
                let vec_val = self.alloc();
                push_inst(&mut self.fn_body, OP_LOAD, &[vec_t, vec_val, var]);
                let out = self.alloc();
                push_inst(&mut self.fn_body, OP_COMPOSITE_EXTRACT, &[u32_t, out, vec_val, 0]);
                out
            }
            S2rSpecial::LaneId => {
                self.capabilities.insert(CAP_GROUP_NON_UNIFORM);
                let var = self.global_builtin_var(BI_SUBGROUP_LOCAL_INVOCATION_ID, u32_t);
                let out = self.alloc();
                push_inst(&mut self.fn_body, OP_LOAD, &[u32_t, out, var]);
                out
            }
            S2rSpecial::LaneMaskEq
            | S2rSpecial::LaneMaskLt
            | S2rSpecial::LaneMaskLe
            | S2rSpecial::LaneMaskGt
            | S2rSpecial::LaneMaskGe => {
                self.capabilities.insert(CAP_GROUP_NON_UNIFORM);
                self.capabilities.insert(CAP_GROUP_NON_UNIFORM_BALLOT);
                let bi = match special {
                    S2rSpecial::LaneMaskEq => BI_SUBGROUP_EQ_MASK,
                    S2rSpecial::LaneMaskLt => BI_SUBGROUP_LT_MASK,
                    S2rSpecial::LaneMaskLe => BI_SUBGROUP_LE_MASK,
                    S2rSpecial::LaneMaskGt => BI_SUBGROUP_GT_MASK,
                    _ => BI_SUBGROUP_GE_MASK,
                };
                let vec4_t = self.ty_u32x4();
                let var = self.global_builtin_var(bi, vec4_t);
                let vec_val = self.alloc();
                push_inst(&mut self.fn_body, OP_LOAD, &[vec4_t, vec_val, var]);
                let out = self.alloc();
                push_inst(&mut self.fn_body, OP_COMPOSITE_EXTRACT, &[u32_t, out, vec_val, 0]);
                out
            }
            S2rSpecial::VariableRate => self.c_u32(0x0001_0100),
            S2rSpecial::Unsupported(_) => self.c_u32(0),
        }
    }
}

impl CoopSpirvBuilder for SpirvModule {
    fn alloc_id(&mut self) -> u32 {
        self.alloc()
    }

    fn const_u32(&mut self, value: u32) -> u32 {
        self.c_u32(value)
    }

    fn type_u32(&mut self) -> u32 {
        self.ty_u32()
    }

    fn type_f16(&mut self) -> u32 {
        self.ty_f16()
    }

    fn type_f32(&mut self) -> u32 {
        self.ty_f32()
    }

    fn type_f16x2(&mut self) -> u32 {
        self.ty_f16x2()
    }

    fn type_coop_matrix(&mut self, elem_ty: u32, rows: u32, cols: u32, use_kind: u32) -> u32 {
        let key = (elem_ty, rows, cols, use_kind);
        if let Some(&id) = self.coop_types.get(&key) {
            return id;
        }
        self.extensions.insert("SPV_KHR_cooperative_matrix");
        self.capabilities.insert(CAPABILITY_COOPERATIVE_MATRIX_KHR);
        let scope_id = self.c_u32(SCOPE_SUBGROUP);
        let rows_id = self.c_u32(rows);
        let cols_id = self.c_u32(cols);
        let use_id = self.c_u32(use_kind);
        let id = self.alloc();
        push_inst(
            &mut self.types_consts_globals,
            OP_TYPE_COOPERATIVE_MATRIX_KHR,
            &[id, elem_ty, scope_id, rows_id, cols_id, use_id],
        );
        self.coop_types.insert(key, id);
        id
    }

    fn push_fn_op(&mut self, opcode: u16, operands: &[u32]) {
        push_inst(&mut self.fn_body, opcode, operands);
    }
}

/// Emits a complete, valid SPIR-V 1.6 module (`Vec<u32>`) from an optimized `SsaProgram`.
pub fn emit_spirv_1_6(program: &SsaProgram) -> Vec<u32> {
    let mut m = SpirvModule::new();
    let ty_void = m.ty_void();
    let ty_u32 = m.ty_u32();
    let _ty_bool = m.ty_bool();
    let c0 = m.c_u32(0);

    // Declare constant buffers matching SpirvEmitContext descriptor binding order.
    let mut ubo_binding = if program.profile.unified_descriptor_binding {
        program.initial_bindings.unified
    } else {
        program.initial_bindings.uniform_buffer
    };
    if !program.info.constant_buffer_descriptors.is_empty() {
        let len_id = m.c_u32(65536 / 4);
        let arr_ty = m.alloc();
        push_inst(&mut m.types_consts_globals, OP_TYPE_ARRAY, &[arr_ty, ty_u32, len_id]);
        m.decorate(arr_ty, DEC_ARRAY_STRIDE, &[4]);

        let struct_ty = m.alloc();
        push_inst(&mut m.types_consts_globals, OP_TYPE_STRUCT, &[struct_ty, arr_ty]);
        m.decorate(struct_ty, DEC_BLOCK, &[]);
        m.member_decorate(struct_ty, 0, DEC_OFFSET, &[0]);

        let ptr_struct = m.ty_ptr(SC_UNIFORM, struct_ty);
        for desc in &program.info.constant_buffer_descriptors {
            let var = m.alloc();
            push_inst(&mut m.types_consts_globals, OP_VARIABLE, &[ptr_struct, var, SC_UNIFORM]);
            m.decorate(var, DEC_DESCRIPTOR_SET, &[0]);
            m.decorate(var, DEC_BINDING, &[ubo_binding]);
            m.name(var, &format!("c{}", desc.index));
            m.interface_vars.push(var);
            m.cbuf_vars.insert(desc.index, var);
            ubo_binding += desc.count;
        }
    }

    // Declare storage buffers.
    let mut ssbo_binding = if program.profile.unified_descriptor_binding {
        ubo_binding
    } else {
        program.initial_bindings.storage_buffer
    };
    if !program.info.storage_buffer_descriptors.is_empty() {
        let rt_arr = m.alloc();
        push_inst(&mut m.types_consts_globals, OP_TYPE_RUNTIME_ARRAY, &[rt_arr, ty_u32]);
        m.decorate(rt_arr, DEC_ARRAY_STRIDE, &[4]);

        let struct_ty_ro = m.alloc();
        push_inst(&mut m.types_consts_globals, OP_TYPE_STRUCT, &[struct_ty_ro, rt_arr]);
        m.decorate(struct_ty_ro, DEC_BLOCK, &[]);
        m.member_decorate(struct_ty_ro, 0, DEC_OFFSET, &[0]);
        m.member_decorate(struct_ty_ro, 0, DEC_NON_WRITABLE, &[]);

        let struct_ty_rw = m.alloc();
        push_inst(&mut m.types_consts_globals, OP_TYPE_STRUCT, &[struct_ty_rw, rt_arr]);
        m.decorate(struct_ty_rw, DEC_BLOCK, &[]);
        m.member_decorate(struct_ty_rw, 0, DEC_OFFSET, &[0]);

        let ptr_ro = m.ty_ptr(SC_STORAGE_BUFFER, struct_ty_ro);
        let ptr_rw = m.ty_ptr(SC_STORAGE_BUFFER, struct_ty_rw);

        for (idx, desc) in program.info.storage_buffer_descriptors.iter().enumerate() {
            let ptr_ty = if desc.is_written { ptr_rw } else { ptr_ro };
            let var = m.alloc();
            push_inst(&mut m.types_consts_globals, OP_VARIABLE, &[ptr_ty, var, SC_STORAGE_BUFFER]);
            m.decorate(var, DEC_DESCRIPTOR_SET, &[0]);
            m.decorate(var, DEC_BINDING, &[ssbo_binding]);
            m.name(var, &format!("ssbo{}", idx));
            m.interface_vars.push(var);
            m.ssbo_vars.insert(idx, var);
            ssbo_binding += desc.count;
        }
    }

    // Declare Workgroup shared memory if used.
    if program.shared_memory_bytes > 0 {
        let words = (program.shared_memory_bytes / 4).max(1);
        let len_id = m.c_u32(words);
        let arr_ty = m.alloc();
        push_inst(&mut m.types_consts_globals, OP_TYPE_ARRAY, &[arr_ty, ty_u32, len_id]);
        let ptr_arr = m.ty_ptr(SC_WORKGROUP, arr_ty);
        let ptr_elem = m.ty_ptr(SC_WORKGROUP, ty_u32);
        let var = m.alloc();
        push_inst(&mut m.types_consts_globals, OP_VARIABLE, &[ptr_arr, var, SC_WORKGROUP]);
        m.name(var, "shared_mem");
        m.interface_vars.push(var);
        m.shared_var = Some((var, ptr_elem));
    }

    // Pre-allocate SPIR-V IDs for every SSA block label and every SSA ValueId.
    let mut blk_labels = Vec::with_capacity(program.blocks.len());
    for _ in &program.blocks {
        blk_labels.push(m.alloc());
    }

    let mut val_ids: BTreeMap<ValueId, u32> = BTreeMap::new();
    let mut hmma_results: BTreeMap<ValueId, [u32; 2]> = BTreeMap::new();
    for block in &program.blocks {
        for insn in &block.insns {
            match &insn.op {
                SsaOp::ConstU32(v) => {
                    val_ids.insert(insn.id, m.c_u32(*v));
                }
                SsaOp::ConstBool(b) => {
                    val_ids.insert(insn.id, m.c_bool(*b));
                }
                SsaOp::ConstU64(v) => {
                    val_ids.insert(insn.id, m.c_u64(*v));
                }
                SsaOp::CoopHmma { .. } => {}
                _ => {
                    val_ids.insert(insn.id, m.alloc());
                }
            }
        }
    }

    // Main function signature.
    let fn_ty = m.alloc();
    push_inst(&mut m.types_consts_globals, OP_TYPE_FUNCTION, &[fn_ty, ty_void]);
    m.name(m.main_fn_id, "main");
    push_inst(&mut m.fn_body, OP_FUNCTION, &[ty_void, m.main_fn_id, 0, fn_ty]);

    let mut did_emit_fn_local = false;
    let ptr_ubo_u32 = m.ty_ptr(SC_UNIFORM, ty_u32);
    let ptr_ssbo_u32 = m.ty_ptr(SC_STORAGE_BUFFER, ty_u32);

    let rpo_blocks = compute_rpo(&program.blocks);
    for &b_idx in &rpo_blocks {
        let block = &program.blocks[b_idx];
        push_inst(&mut m.fn_body, OP_LABEL, &[blk_labels[b_idx]]);

        if !did_emit_fn_local {
            did_emit_fn_local = true;
            if program.local_memory_bytes > 0 {
                let words = (program.local_memory_bytes / 4).max(1);
                let len_id = m.c_u32(words);
                let arr_ty = m.alloc();
                push_inst(&mut m.types_consts_globals, OP_TYPE_ARRAY, &[arr_ty, ty_u32, len_id]);
                let ptr_arr = m.ty_ptr(SC_FUNCTION, arr_ty);
                let ptr_elem = m.ty_ptr(SC_FUNCTION, ty_u32);
                let lvar = m.alloc();
                push_inst(&mut m.fn_body, OP_VARIABLE, &[ptr_arr, lvar, SC_FUNCTION]);
                m.local_var = Some((lvar, ptr_elem));
            }
        }

        // First emit all Phis in the block (mandatory SPIR-V invariant: OpPhi before non-Phi).
        for insn in &block.insns {
            if let SsaOp::Phi(entries) = &insn.op {
                let res_id = val_ids[&insn.id];
                let res_ty = m.spv_ty(insn.ty);
                let mut ops = Vec::with_capacity(2 + entries.len() * 2);
                ops.push(res_ty);
                ops.push(res_id);
                for &(pred_blk, src_v) in entries {
                    ops.push(val_ids[&src_v]);
                    ops.push(blk_labels[pred_blk]);
                }
                push_inst(&mut m.fn_body, OP_PHI, &ops);
            }
        }

        // Emit all non-Phi instructions.
        for insn in &block.insns {
            if matches!(insn.op, SsaOp::Phi(_) | SsaOp::ConstU32(_) | SsaOp::ConstBool(_) | SsaOp::ConstU64(_)) {
                continue;
            }
            let res_ty = m.spv_ty(insn.ty);
            let res_id = val_ids.get(&insn.id).copied().unwrap_or(0);

            match &insn.op {
                SsaOp::Undef => {
                    push_inst(&mut m.fn_body, OP_UNDEF, &[res_ty, res_id]);
                }
                SsaOp::Copy(s) => {
                    push_inst(&mut m.fn_body, OP_COPY_OBJECT, &[res_ty, res_id, val_ids[s]]);
                }
                SsaOp::ReadBuiltin(spec) => {
                    let bid = m.emit_read_builtin(*spec);
                    push_inst(&mut m.fn_body, OP_BITCAST, &[res_ty, res_id, bid]);
                }
                SsaOp::ReadCbuf { bank, word_offset } => {
                    let var = m.cbuf_vars[bank];
                    let off_id = m.c_u32(*word_offset);
                    let ptr = m.alloc();
                    push_inst(&mut m.fn_body, OP_ACCESS_CHAIN, &[ptr_ubo_u32, ptr, var, c0, off_id]);
                    push_inst(&mut m.fn_body, OP_LOAD, &[res_ty, res_id, ptr]);
                }
                SsaOp::ReadCbufDynamic { bank, byte_offset } => {
                    let var = m.cbuf_vars[bank];
                    let two = m.c_u32(2);
                    let word_idx = m.alloc();
                    push_inst(&mut m.fn_body, OP_SHIFT_RIGHT_LOGICAL, &[ty_u32, word_idx, val_ids[byte_offset], two]);
                    let ptr = m.alloc();
                    push_inst(&mut m.fn_body, OP_ACCESS_CHAIN, &[ptr_ubo_u32, ptr, var, c0, word_idx]);
                    push_inst(&mut m.fn_body, OP_LOAD, &[res_ty, res_id, ptr]);
                }
                SsaOp::Select { cond, t, f } => {
                    push_inst(&mut m.fn_body, OP_SELECT, &[res_ty, res_id, val_ids[cond], val_ids[t], val_ids[f]]);
                }
                SsaOp::Bitcast { val, .. } => {
                    push_inst(&mut m.fn_body, OP_BITCAST, &[res_ty, res_id, val_ids[val]]);
                }
                SsaOp::IAdd(a, b) => push_inst(&mut m.fn_body, OP_I_ADD, &[res_ty, res_id, val_ids[a], val_ids[b]]),
                SsaOp::ISub(a, b) => push_inst(&mut m.fn_body, OP_I_SUB, &[res_ty, res_id, val_ids[a], val_ids[b]]),
                SsaOp::IMul(a, b) => push_inst(&mut m.fn_body, OP_I_MUL, &[res_ty, res_id, val_ids[a], val_ids[b]]),
                SsaOp::UMulHi(a, b) | SsaOp::SMulHi(a, b) => {
                    let pair_ty = m.ty_u32_pair_struct();
                    let pair_id = m.alloc();
                    let op = if matches!(insn.op, SsaOp::SMulHi(..)) {
                        OP_S_MUL_EXTENDED
                    } else {
                        OP_U_MUL_EXTENDED
                    };
                    push_inst(&mut m.fn_body, op, &[pair_ty, pair_id, val_ids[a], val_ids[b]]);
                    push_inst(&mut m.fn_body, OP_COMPOSITE_EXTRACT, &[res_ty, res_id, pair_id, 1]);
                }
                SsaOp::BitAnd(a, b) => push_inst(&mut m.fn_body, OP_BITWISE_AND, &[res_ty, res_id, val_ids[a], val_ids[b]]),
                SsaOp::BitOr(a, b) => push_inst(&mut m.fn_body, OP_BITWISE_OR, &[res_ty, res_id, val_ids[a], val_ids[b]]),
                SsaOp::BitXor(a, b) => push_inst(&mut m.fn_body, OP_BITWISE_XOR, &[res_ty, res_id, val_ids[a], val_ids[b]]),
                SsaOp::BitNot(a) => push_inst(&mut m.fn_body, OP_NOT, &[res_ty, res_id, val_ids[a]]),
                SsaOp::Shl(a, b) => push_inst(&mut m.fn_body, OP_SHIFT_LEFT_LOGICAL, &[res_ty, res_id, val_ids[a], val_ids[b]]),
                SsaOp::LShr(a, b) => push_inst(&mut m.fn_body, OP_SHIFT_RIGHT_LOGICAL, &[res_ty, res_id, val_ids[a], val_ids[b]]),
                SsaOp::AShr(a, b) => push_inst(&mut m.fn_body, OP_SHIFT_RIGHT_ARITHMETIC, &[res_ty, res_id, val_ids[a], val_ids[b]]),
                SsaOp::INeg(a) => push_inst(&mut m.fn_body, OP_S_NEGATE, &[res_ty, res_id, val_ids[a]]),
                SsaOp::BitCount(a) => push_inst(&mut m.fn_body, OP_BIT_COUNT, &[res_ty, res_id, val_ids[a]]),
                SsaOp::BitReverse(a) => push_inst(&mut m.fn_body, OP_BIT_REVERSE, &[res_ty, res_id, val_ids[a]]),
                SsaOp::SBitFieldExtract { val, offset, count } => {
                    let off_id = m.c_u32(*offset);
                    let cnt_id = m.c_u32(*count);
                    push_inst(&mut m.fn_body, OP_BIT_FIELD_S_EXTRACT, &[res_ty, res_id, val_ids[val], off_id, cnt_id]);
                }
                SsaOp::UBitFieldExtract { val, offset, count } => {
                    let off_id = m.c_u32(*offset);
                    let cnt_id = m.c_u32(*count);
                    push_inst(&mut m.fn_body, OP_BIT_FIELD_U_EXTRACT, &[res_ty, res_id, val_ids[val], off_id, cnt_id]);
                }
                SsaOp::IntCmp { cmp, signed, a, b } => {
                    let va = val_ids[a];
                    let vb = val_ids[b];
                    match cmp {
                        IntCmp::False => {
                            let f = m.c_bool(false);
                            let t = m.c_bool(true);
                            push_inst(&mut m.fn_body, OP_LOGICAL_EQUAL, &[res_ty, res_id, f, t]);
                        }
                        IntCmp::True => {
                            let t = m.c_bool(true);
                            push_inst(&mut m.fn_body, OP_LOGICAL_EQUAL, &[res_ty, res_id, t, t]);
                        }
                        IntCmp::Eq => push_inst(&mut m.fn_body, OP_I_EQUAL, &[res_ty, res_id, va, vb]),
                        IntCmp::Ne => push_inst(&mut m.fn_body, OP_I_NOT_EQUAL, &[res_ty, res_id, va, vb]),
                        IntCmp::Lt => push_inst(&mut m.fn_body, if *signed { OP_S_LESS_THAN } else { OP_U_LESS_THAN }, &[res_ty, res_id, va, vb]),
                        IntCmp::Le => push_inst(&mut m.fn_body, if *signed { OP_S_LESS_THAN_EQUAL } else { OP_U_LESS_THAN_EQUAL }, &[res_ty, res_id, va, vb]),
                        IntCmp::Gt => push_inst(&mut m.fn_body, if *signed { OP_S_GREATER_THAN } else { OP_U_GREATER_THAN }, &[res_ty, res_id, va, vb]),
                        IntCmp::Ge => push_inst(&mut m.fn_body, if *signed { OP_S_GREATER_THAN_EQUAL } else { OP_U_GREATER_THAN_EQUAL }, &[res_ty, res_id, va, vb]),
                    }
                }
                SsaOp::BoolAnd(a, b) => push_inst(&mut m.fn_body, OP_LOGICAL_AND, &[res_ty, res_id, val_ids[a], val_ids[b]]),
                SsaOp::BoolOr(a, b) => push_inst(&mut m.fn_body, OP_LOGICAL_OR, &[res_ty, res_id, val_ids[a], val_ids[b]]),
                SsaOp::BoolXor(a, b) => {
                    let na = m.alloc();
                    let nb = m.alloc();
                    let t1 = m.alloc();
                    let t2 = m.alloc();
                    let va = val_ids[a];
                    let vb = val_ids[b];
                    push_inst(&mut m.fn_body, OP_LOGICAL_NOT, &[res_ty, na, va]);
                    push_inst(&mut m.fn_body, OP_LOGICAL_NOT, &[res_ty, nb, vb]);
                    push_inst(&mut m.fn_body, OP_LOGICAL_AND, &[res_ty, t1, va, nb]);
                    push_inst(&mut m.fn_body, OP_LOGICAL_AND, &[res_ty, t2, na, vb]);
                    push_inst(&mut m.fn_body, OP_LOGICAL_OR, &[res_ty, res_id, t1, t2]);
                }
                SsaOp::BoolNot(a) => push_inst(&mut m.fn_body, OP_LOGICAL_NOT, &[res_ty, res_id, val_ids[a]]),
                SsaOp::FAdd(a, b) => push_inst(&mut m.fn_body, OP_F_ADD, &[res_ty, res_id, val_ids[a], val_ids[b]]),
                SsaOp::FSub(a, b) => push_inst(&mut m.fn_body, OP_F_SUB, &[res_ty, res_id, val_ids[a], val_ids[b]]),
                SsaOp::FMul(a, b) => push_inst(&mut m.fn_body, OP_F_MUL, &[res_ty, res_id, val_ids[a], val_ids[b]]),
                SsaOp::FDiv(a, b) => push_inst(&mut m.fn_body, OP_F_DIV, &[res_ty, res_id, val_ids[a], val_ids[b]]),
                SsaOp::FNeg(a) => push_inst(&mut m.fn_body, OP_F_NEGATE, &[res_ty, res_id, val_ids[a]]),
                SsaOp::FAbs(a) => {
                    let ext = m.glsl_ext_id;
                    push_inst(&mut m.fn_body, OP_EXT_INST, &[res_ty, res_id, ext, 4, val_ids[a]]);
                }
                SsaOp::FSet { cmp, a, b } => {
                    let va = val_ids[a];
                    let vb = val_ids[b];
                    match cmp {
                        FloatCmp::False => {
                            let f = m.c_bool(false);
                            let t = m.c_bool(true);
                            push_inst(&mut m.fn_body, OP_LOGICAL_EQUAL, &[res_ty, res_id, f, t]);
                        }
                        FloatCmp::True => {
                            let t = m.c_bool(true);
                            push_inst(&mut m.fn_body, OP_LOGICAL_EQUAL, &[res_ty, res_id, t, t]);
                        }
                        FloatCmp::Num => {
                            let na = m.alloc();
                            let nb = m.alloc();
                            let ored = m.alloc();
                            push_inst(&mut m.fn_body, OP_IS_NAN, &[res_ty, na, va]);
                            push_inst(&mut m.fn_body, OP_IS_NAN, &[res_ty, nb, vb]);
                            push_inst(&mut m.fn_body, OP_LOGICAL_OR, &[res_ty, ored, na, nb]);
                            push_inst(&mut m.fn_body, OP_LOGICAL_NOT, &[res_ty, res_id, ored]);
                        }
                        FloatCmp::Nan => {
                            let na = m.alloc();
                            let nb = m.alloc();
                            push_inst(&mut m.fn_body, OP_IS_NAN, &[res_ty, na, va]);
                            push_inst(&mut m.fn_body, OP_IS_NAN, &[res_ty, nb, vb]);
                            push_inst(&mut m.fn_body, OP_LOGICAL_OR, &[res_ty, res_id, na, nb]);
                        }
                        _ => {
                            let op = match cmp {
                                FloatCmp::Lt => OP_F_ORD_LESS_THAN,
                                FloatCmp::Eq => OP_F_ORD_EQUAL,
                                FloatCmp::Le => OP_F_ORD_LESS_THAN_EQUAL,
                                FloatCmp::Gt => OP_F_ORD_GREATER_THAN,
                                FloatCmp::Ne => OP_F_ORD_NOT_EQUAL,
                                FloatCmp::Ge => OP_F_ORD_GREATER_THAN_EQUAL,
                                FloatCmp::Ltu => OP_F_UNORD_LESS_THAN,
                                FloatCmp::Equ => OP_F_UNORD_EQUAL,
                                FloatCmp::Leu => OP_F_UNORD_LESS_THAN_EQUAL,
                                FloatCmp::Gtu => OP_F_UNORD_GREATER_THAN,
                                FloatCmp::Neu => OP_F_UNORD_NOT_EQUAL,
                                FloatCmp::Geu => OP_F_UNORD_GREATER_THAN_EQUAL,
                                _ => OP_F_ORD_EQUAL,
                            };
                            push_inst(&mut m.fn_body, op, &[res_ty, res_id, va, vb]);
                        }
                    }
                }
                SsaOp::Glsl1(gop, a) => {
                    let ext = m.glsl_ext_id;
                    push_inst(&mut m.fn_body, OP_EXT_INST, &[res_ty, res_id, ext, glsl_op_code(*gop), val_ids[a]]);
                }
                SsaOp::Glsl2(gop, a, b) => {
                    let ext = m.glsl_ext_id;
                    push_inst(&mut m.fn_body, OP_EXT_INST, &[res_ty, res_id, ext, glsl_op_code(*gop), val_ids[a], val_ids[b]]);
                }
                SsaOp::SConvertToF32(a) => push_inst(&mut m.fn_body, OP_CONVERT_S_TO_F, &[res_ty, res_id, val_ids[a]]),
                SsaOp::UConvertToF32(a) => push_inst(&mut m.fn_body, OP_CONVERT_U_TO_F, &[res_ty, res_id, val_ids[a]]),
                SsaOp::F32ConvertToS(a) => push_inst(&mut m.fn_body, OP_CONVERT_F_TO_S, &[res_ty, res_id, val_ids[a]]),
                SsaOp::F32ConvertToU(a) => push_inst(&mut m.fn_body, OP_CONVERT_F_TO_U, &[res_ty, res_id, val_ids[a]]),
                SsaOp::FConvertF32ToF16Lo(a) => {
                    let f16_t = m.ty_f16();
                    let f16x2_t = m.ty_f16x2();
                    let h0 = m.alloc();
                    push_inst(&mut m.fn_body, OP_F_CONVERT, &[f16_t, h0, val_ids[a]]);
                    let h_zero = m.alloc();
                    let c0f = m.alloc();
                    let ty_f32 = m.ty_f32();
                    push_inst(&mut m.fn_body, OP_BITCAST, &[ty_f32, c0f, c0]);
                    push_inst(&mut m.fn_body, OP_F_CONVERT, &[f16_t, h_zero, c0f]);
                    let vec_h = m.alloc();
                    push_inst(&mut m.fn_body, OP_COMPOSITE_CONSTRUCT, &[f16x2_t, vec_h, h0, h_zero]);
                    push_inst(&mut m.fn_body, OP_BITCAST, &[res_ty, res_id, vec_h]);
                }
                SsaOp::FConvertF16LoToF32(a) => {
                    let f16_t = m.ty_f16();
                    let f16x2_t = m.ty_f16x2();
                    let vec_h = m.alloc();
                    push_inst(&mut m.fn_body, OP_BITCAST, &[f16x2_t, vec_h, val_ids[a]]);
                    let h0 = m.alloc();
                    push_inst(&mut m.fn_body, OP_COMPOSITE_EXTRACT, &[f16_t, h0, vec_h, 0]);
                    push_inst(&mut m.fn_body, OP_F_CONVERT, &[res_ty, res_id, h0]);
                }
                SsaOp::HAdd2 { a, b } => {
                    push_inst(&mut m.fn_body, OP_F_ADD, &[res_ty, res_id, val_ids[a], val_ids[b]]);
                }
                SsaOp::HMul2 { a, b } => {
                    push_inst(&mut m.fn_body, OP_F_MUL, &[res_ty, res_id, val_ids[a], val_ids[b]]);
                }
                SsaOp::HFma2 { a, b, c } => {
                    let p = m.alloc();
                    push_inst(&mut m.fn_body, OP_F_MUL, &[res_ty, p, val_ids[a], val_ids[b]]);
                    push_inst(&mut m.fn_body, OP_F_ADD, &[res_ty, res_id, p, val_ids[c]]);
                }
                SsaOp::LoadSsboWord { ssbo_idx, word_idx } => {
                    let var = m.ssbo_vars[ssbo_idx];
                    let ptr = m.alloc();
                    push_inst(&mut m.fn_body, OP_ACCESS_CHAIN, &[ptr_ssbo_u32, ptr, var, c0, val_ids[word_idx]]);
                    push_inst(&mut m.fn_body, OP_LOAD, &[res_ty, res_id, ptr]);
                }
                SsaOp::StoreSsboWord { ssbo_idx, word_idx, val } => {
                    let var = m.ssbo_vars[ssbo_idx];
                    let ptr = m.alloc();
                    push_inst(&mut m.fn_body, OP_ACCESS_CHAIN, &[ptr_ssbo_u32, ptr, var, c0, val_ids[word_idx]]);
                    push_inst(&mut m.fn_body, OP_STORE, &[ptr, val_ids[val]]);
                }
                SsaOp::AtomicSsboWord { ssbo_idx, word_idx, op, signed, data } => {
                    let var = m.ssbo_vars[ssbo_idx];
                    let ptr = m.alloc();
                    push_inst(&mut m.fn_body, OP_ACCESS_CHAIN, &[ptr_ssbo_u32, ptr, var, c0, val_ids[word_idx]]);
                    let scope = m.c_u32(SCOPE_DEVICE);
                    let sem = m.c_u32(SEM_NONE);
                    push_inst(&mut m.fn_body, atomic_spv_op(*op, *signed), &[res_ty, res_id, ptr, scope, sem, val_ids[data]]);
                }
                SsaOp::LoadSharedWord { word_idx } => {
                    let (var, ptr_ty) = m.shared_var.expect("shared_var declared when shared_bytes > 0");
                    let ptr = m.alloc();
                    push_inst(&mut m.fn_body, OP_ACCESS_CHAIN, &[ptr_ty, ptr, var, val_ids[word_idx]]);
                    push_inst(&mut m.fn_body, OP_LOAD, &[res_ty, res_id, ptr]);
                }
                SsaOp::StoreSharedWord { word_idx, val } => {
                    let (var, ptr_ty) = m.shared_var.expect("shared_var declared when shared_bytes > 0");
                    let ptr = m.alloc();
                    push_inst(&mut m.fn_body, OP_ACCESS_CHAIN, &[ptr_ty, ptr, var, val_ids[word_idx]]);
                    push_inst(&mut m.fn_body, OP_STORE, &[ptr, val_ids[val]]);
                }
                SsaOp::AtomicSharedWord { word_idx, op, data } => {
                    let (var, ptr_ty) = m.shared_var.expect("shared_var declared when shared_bytes > 0");
                    let ptr = m.alloc();
                    push_inst(&mut m.fn_body, OP_ACCESS_CHAIN, &[ptr_ty, ptr, var, val_ids[word_idx]]);
                    let scope = m.c_u32(SCOPE_WORKGROUP);
                    let sem = m.c_u32(SEM_NONE);
                    push_inst(&mut m.fn_body, atomic_spv_op(*op, false), &[res_ty, res_id, ptr, scope, sem, val_ids[data]]);
                }
                SsaOp::LoadLocalWord { word_idx } => {
                    let (var, ptr_ty) = m.local_var.expect("local_var declared when local_bytes > 0");
                    let ptr = m.alloc();
                    push_inst(&mut m.fn_body, OP_ACCESS_CHAIN, &[ptr_ty, ptr, var, val_ids[word_idx]]);
                    push_inst(&mut m.fn_body, OP_LOAD, &[res_ty, res_id, ptr]);
                }
                SsaOp::StoreLocalWord { word_idx, val } => {
                    let (var, ptr_ty) = m.local_var.expect("local_var declared when local_bytes > 0");
                    let ptr = m.alloc();
                    push_inst(&mut m.fn_body, OP_ACCESS_CHAIN, &[ptr_ty, ptr, var, val_ids[word_idx]]);
                    push_inst(&mut m.fn_body, OP_STORE, &[ptr, val_ids[val]]);
                }
                SsaOp::SubgroupShuffle { mode, val, lane_or_delta } => {
                    m.capabilities.insert(CAP_GROUP_NON_UNIFORM);
                    let scope = m.c_u32(SCOPE_SUBGROUP);
                    let op = match mode {
                        ShflMode::Idx => {
                            m.capabilities.insert(CAP_GROUP_NON_UNIFORM_SHUFFLE);
                            OP_GROUP_NON_UNIFORM_SHUFFLE
                        }
                        ShflMode::Bfly => {
                            m.capabilities.insert(CAP_GROUP_NON_UNIFORM_SHUFFLE);
                            OP_GROUP_NON_UNIFORM_SHUFFLE_XOR
                        }
                        ShflMode::Up => {
                            m.capabilities.insert(CAP_GROUP_NON_UNIFORM_SHUFFLE_RELATIVE);
                            OP_GROUP_NON_UNIFORM_SHUFFLE_UP
                        }
                        ShflMode::Down => {
                            m.capabilities.insert(CAP_GROUP_NON_UNIFORM_SHUFFLE_RELATIVE);
                            OP_GROUP_NON_UNIFORM_SHUFFLE_DOWN
                        }
                    };
                    push_inst(&mut m.fn_body, op, &[res_ty, res_id, scope, val_ids[val], val_ids[lane_or_delta]]);
                }
                SsaOp::SubgroupVote { mode, pred } => {
                    m.capabilities.insert(CAP_GROUP_NON_UNIFORM);
                    m.capabilities.insert(CAP_GROUP_NON_UNIFORM_VOTE);
                    let scope = m.c_u32(SCOPE_SUBGROUP);
                    let op = match mode {
                        VoteMode::All => OP_GROUP_NON_UNIFORM_ALL,
                        VoteMode::Any => OP_GROUP_NON_UNIFORM_ANY,
                        VoteMode::Eq => OP_GROUP_NON_UNIFORM_ALL_EQUAL,
                    };
                    push_inst(&mut m.fn_body, op, &[res_ty, res_id, scope, val_ids[pred]]);
                }
                SsaOp::SubgroupBallot { pred } => {
                    m.capabilities.insert(CAP_GROUP_NON_UNIFORM);
                    m.capabilities.insert(CAP_GROUP_NON_UNIFORM_BALLOT);
                    let vec4_t = m.ty_u32x4();
                    let scope = m.c_u32(SCOPE_SUBGROUP);
                    let vec4_id = m.alloc();
                    push_inst(&mut m.fn_body, OP_GROUP_NON_UNIFORM_BALLOT, &[vec4_t, vec4_id, scope, val_ids[pred]]);
                    push_inst(&mut m.fn_body, OP_COMPOSITE_EXTRACT, &[res_ty, res_id, vec4_id, 0]);
                }
                SsaOp::ControlBarrier => {
                    let exec_scope = m.c_u32(SCOPE_WORKGROUP);
                    let mem_scope = m.c_u32(SCOPE_WORKGROUP);
                    let sem = m.c_u32(SEM_ACQUIRE_RELEASE | SEM_WORKGROUP_MEMORY);
                    push_inst(&mut m.fn_body, OP_CONTROL_BARRIER, &[exec_scope, mem_scope, sem]);
                }
                SsaOp::MemoryBarrier { scope } => {
                    let mem_scope = match scope {
                        MemBarrierScope::Workgroup => m.c_u32(SCOPE_WORKGROUP),
                        MemBarrierScope::Device => m.c_u32(SCOPE_DEVICE),
                        MemBarrierScope::System => m.c_u32(SCOPE_SYSTEM_CROSS_DEVICE),
                    };
                    let sem = m.c_u32(SEM_ACQUIRE_RELEASE | SEM_WORKGROUP_MEMORY | SEM_UNIFORM_MEMORY);
                    push_inst(&mut m.fn_body, OP_MEMORY_BARRIER, &[mem_scope, sem]);
                }
                SsaOp::CoopHmma { depth, a, a_len, b, b_len, c } => {
                    let a_ids: Vec<u32> = a[..*a_len].iter().map(|v| val_ids[v]).collect();
                    let b_ids: Vec<u32> = b[..*b_len].iter().map(|v| val_ids[v]).collect();
                    let c_ids = [val_ids[&c[0]], val_ids[&c[1]]];
                    let out_words = emit_native_coop_hmma(
                        &mut m,
                        &program.coop_config,
                        *depth,
                        &a_ids,
                        &b_ids,
                        &c_ids,
                    );
                    hmma_results.insert(insn.id, out_words);
                }
                SsaOp::CoopHmmaExtract { hmma, word_idx } => {
                    let word = hmma_results[hmma][(*word_idx).min(1)];
                    push_inst(&mut m.fn_body, OP_BITCAST, &[res_ty, res_id, word]);
                }
                SsaOp::ConstU32(_) | SsaOp::ConstBool(_) | SsaOp::ConstU64(_) | SsaOp::Phi(_) => {}
            }
        }

        // Emit structured merge hint and terminator.
        match &block.merge {
            MergeHint::None => {}
            MergeHint::Selection { merge_block } => {
                push_inst(&mut m.fn_body, OP_SELECTION_MERGE, &[blk_labels[*merge_block], 0]);
            }
            MergeHint::Loop { merge_block, continue_block } => {
                push_inst(&mut m.fn_body, OP_LOOP_MERGE, &[blk_labels[*merge_block], blk_labels[*continue_block], 0]);
            }
        }

        match &block.terminator {
            Terminator::Branch { target } => {
                push_inst(&mut m.fn_body, OP_BRANCH, &[blk_labels[*target]]);
            }
            Terminator::BranchCond { cond, true_target, false_target } => {
                push_inst(&mut m.fn_body, OP_BRANCH_CONDITIONAL, &[val_ids[cond], blk_labels[*true_target], blk_labels[*false_target]]);
            }
            Terminator::Return => {
                push_inst(&mut m.fn_body, OP_RETURN, &[]);
            }
            Terminator::Unreachable => {
                push_inst(&mut m.fn_body, OP_UNREACHABLE, &[]);
            }
        }
    }

    push_inst(&mut m.fn_body, OP_FUNCTION_END, &[]);

    // Assemble final SPIR-V 1.6 module in canonical section order.
    let mut out = Vec::with_capacity(512 + m.fn_body.len());
    out.push(SPIRV_MAGIC);
    out.push(SPIRV_1_6_VERSION);
    out.push(GENERATOR_ID);
    out.push(m.next_id);
    out.push(0);

    for cap in &m.capabilities {
        push_inst(&mut out, OP_CAPABILITY, &[*cap]);
    }
    for ext in &m.extensions {
        let words = encode_str_words(ext);
        push_inst(&mut out, OP_EXTENSION, &words);
    }
    {
        let mut ops = vec![m.glsl_ext_id];
        ops.extend(encode_str_words("GLSL.std.450"));
        push_inst(&mut out, OP_EXT_INST_IMPORT, &ops);
    }
    // Logical (0) + Vulkan Memory Model (3).
    push_inst(&mut out, OP_MEMORY_MODEL, &[0, 3]);

    {
        let mut ops = Vec::new();
        ops.push(EXEC_MODEL_GL_COMPUTE);
        ops.push(m.main_fn_id);
        ops.extend(encode_str_words("main"));
        ops.extend_from_slice(&m.interface_vars);
        push_inst(&mut out, OP_ENTRY_POINT, &ops);
    }

    push_inst(
        &mut out,
        OP_EXECUTION_MODE,
        &[
            m.main_fn_id,
            EXEC_MODE_LOCAL_SIZE,
            program.workgroup_size[0],
            program.workgroup_size[1],
            program.workgroup_size[2],
        ],
    );

    out.extend_from_slice(&m.names);
    out.extend_from_slice(&m.decorations);
    out.extend_from_slice(&m.types_consts_globals);
    out.extend_from_slice(&m.fn_decls);
    out.extend_from_slice(&m.fn_body);
    out
}

fn compute_rpo(blocks: &[crate::ssa_ir::SsaBlock]) -> Vec<usize> {
    if blocks.is_empty() {
        return Vec::new();
    }
    let mut visited = vec![false; blocks.len()];
    let mut postorder = Vec::with_capacity(blocks.len());
    fn dfs(
        blk: usize,
        blocks: &[crate::ssa_ir::SsaBlock],
        visited: &mut [bool],
        postorder: &mut Vec<usize>,
    ) {
        if blk >= blocks.len() || visited[blk] {
            return;
        }
        visited[blk] = true;
        match blocks[blk].merge {
            MergeHint::Loop { merge_block, continue_block } => {
                dfs(merge_block, blocks, visited, postorder);
                dfs(continue_block, blocks, visited, postorder);
            }
            MergeHint::Selection { merge_block } => {
                // Visit targets before merge so merge ends up earlier in postorder -> later in RPO.
                match blocks[blk].terminator {
                    Terminator::Branch { target } => dfs(target, blocks, visited, postorder),
                    Terminator::BranchCond { true_target, false_target, .. } => {
                        dfs(false_target, blocks, visited, postorder);
                        dfs(true_target, blocks, visited, postorder);
                    }
                    Terminator::Return | Terminator::Unreachable => {}
                }
                dfs(merge_block, blocks, visited, postorder);
                postorder.push(blk);
                return;
            }
            MergeHint::None => {}
        }
        match blocks[blk].terminator {
            Terminator::Branch { target } => dfs(target, blocks, visited, postorder),
            Terminator::BranchCond { true_target, false_target, .. } => {
                dfs(false_target, blocks, visited, postorder);
                dfs(true_target, blocks, visited, postorder);
            }
            Terminator::Return | Terminator::Unreachable => {}
        }
        postorder.push(blk);
    }
    dfs(0, blocks, &mut visited, &mut postorder);
    postorder.reverse();
    postorder
}

fn atomic_spv_op(op: AtomicOp, signed: bool) -> u16 {
    match op {
        AtomicOp::Add | AtomicOp::Inc | AtomicOp::Dec => OP_ATOMIC_I_ADD,
        AtomicOp::Min => if signed { OP_ATOMIC_S_MIN } else { OP_ATOMIC_U_MIN },
        AtomicOp::Max => if signed { OP_ATOMIC_S_MAX } else { OP_ATOMIC_U_MAX },
        AtomicOp::And => OP_ATOMIC_AND,
        AtomicOp::Or => OP_ATOMIC_OR,
        AtomicOp::Xor => OP_ATOMIC_XOR,
        AtomicOp::Exch => OP_ATOMIC_EXCHANGE,
    }
}
